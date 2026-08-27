#!/usr/bin/env python3
"""AI-NATIVE BENCHMARK - shared safe source-file access.

lstat + O_NOFOLLOW based defenses against a sandboxed/scripted process
replacing workspace/benchmark.kai with a symlink, FIFO, socket, device,
or directory to trick host-side code into reading through to, or
blocking on, something outside the workspace.

Extracted from isolation/broker.py (Isolation M2) so the exact same,
already-tested logic is reused - never reimplemented a second time - by:

    isolation/broker.py              (compile/semantic-query source reads)
    agent/orchestrator.py             (read_source/replace_source tools, M3A)

scripts/collect-isolated-trial.sh independently reimplements the
identical read-side check in an inline Python snippet, since it is a
one-shot bash-orchestrated operation that has no natural place to import
a sibling module from - see that script's own comment.
"""

import os
import stat


class InvalidSource(Exception):
    def __init__(self, reason):
        super().__init__(reason)
        self.reason = reason


def _describe_non_regular(mode):
    if stat.S_ISLNK(mode):
        return "a symlink"
    if stat.S_ISFIFO(mode):
        return "a FIFO"
    if stat.S_ISSOCK(mode):
        return "a socket"
    if stat.S_ISBLK(mode):
        return "a block device"
    if stat.S_ISCHR(mode):
        return "a character device"
    if stat.S_ISDIR(mode):
        return "a directory"
    return "not a regular file"


def read_validated_source(workspace_dir, filename="benchmark.kai"):
    """Safely reads <workspace_dir>/<filename>'s bytes on the HOST,
    refusing anything that is not a genuine regular file. Returns the
    file's bytes; never returns a path for a second process to re-open by
    name (that would reopen a TOCTOU window against a live, still-running
    sandbox/agent session)."""
    src = os.path.join(workspace_dir, filename)
    try:
        st = os.lstat(src)
    except FileNotFoundError:
        raise InvalidSource(f"{filename} does not exist")
    except OSError as exc:
        raise InvalidSource(f"could not lstat {filename}: {exc}")

    if not stat.S_ISREG(st.st_mode):
        raise InvalidSource(f"{filename} is {_describe_non_regular(st.st_mode)}, not a regular file")

    real_workspace = os.path.realpath(workspace_dir)
    real_parent = os.path.dirname(os.path.realpath(src))
    if real_parent != real_workspace:
        raise InvalidSource("resolved source path escapes the workspace")

    # O_NOFOLLOW: a second, TOCTOU-resistant guarantee of the same
    # regular-file property confirmed above via lstat.
    try:
        fd = os.open(src, os.O_RDONLY | os.O_NOFOLLOW)
    except OSError as exc:
        raise InvalidSource(f"could not open {filename} safely: {exc}")
    try:
        with os.fdopen(fd, "rb") as f:
            return f.read()
    except OSError as exc:
        raise InvalidSource(f"could not read {filename}: {exc}")


def write_validated_source(workspace_dir, content_bytes, filename="benchmark.kai", max_bytes=1_000_000):
    """Safely REPLACES <workspace_dir>/<filename>'s content. Refuses
    outright if the current entry at that path is anything other than a
    regular file or absent (symlink, FIFO, socket, device, directory) -
    matching read_validated_source's own refusal, rather than silently
    "succeeding" by relying on os.replace()'s own safe-by-construction
    behavior (rename() never follows a symlink at the destination, so it
    would technically be safe to let it through, but an explicit refusal
    is the clearer, more predictable, and more auditable behavior here).

    Never accepts a destination path from any caller - filename is always
    exactly "benchmark.kai" in practice. Writes to a fresh temporary
    regular file in the SAME directory, fsyncs it, then atomically
    renames it onto the target path."""
    if len(content_bytes) > max_bytes:
        raise InvalidSource(f"source exceeds maximum size of {max_bytes} bytes")

    real_workspace = os.path.realpath(workspace_dir)
    if not os.path.isdir(real_workspace):
        raise InvalidSource("workspace directory does not exist")

    dest = os.path.join(real_workspace, filename)

    try:
        st = os.lstat(dest)
    except FileNotFoundError:
        pass
    except OSError as exc:
        raise InvalidSource(f"could not lstat {filename}: {exc}")
    else:
        if not stat.S_ISREG(st.st_mode):
            raise InvalidSource(f"refusing to replace {filename}: it is {_describe_non_regular(st.st_mode)}, not a regular file")

    tmp_path = os.path.join(real_workspace, f".{filename}.tmp-{os.getpid()}-{id(content_bytes)}")
    fd = os.open(tmp_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(content_bytes)
            f.flush()
            os.fsync(f.fileno())
        # rename() replaces the destination directory entry outright and
        # never follows a symlink there - the pre-existing (possibly
        # hostile) filesystem entry's own target is never opened/written.
        os.replace(tmp_path, dest)
    except BaseException:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass
        raise
