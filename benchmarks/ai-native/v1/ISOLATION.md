# AI-Native Benchmark v1 — Isolation Threat Model (Isolation M1 + M2)

This document is the isolation/threat-model reference for the containerized
sandbox substrate under `scripts/prepare-isolated-trial.sh`,
`scripts/sandbox-exec.sh`, `scripts/cleanup-isolated-trials.sh`,
`scripts/collect-isolated-trial.sh`, `scripts/test-isolation.sh` (M1), and
`scripts/tool-sandbox-exec.sh`, `scripts/isolation/{stage-toolchain.sh,
broker.py, generate-tool-surface.sh}`, `scripts/test-tool-boundary.sh`
(M2). It supplements, and does not replace, `README.md`'s existing
"Isolation" section, which still governs the original, uncontained
workflow (`scripts/prepare-run.sh` + `scripts/validate-run.py`).

**Status: infrastructure only.** As of Isolation M2, no formal benchmark
trial has been run. Formal trial counts remain:

```
textual  = 0
semantic = 0
```

Both milestones exist to build and verify a sandbox substrate a future
agent adapter can safely use — neither runs an agent itself, and neither
changes those counts. M1 established filesystem/network isolation and
safe result collection; M2 adds the enforced, condition-specific tool
boundary (see "Isolation M2" below) — the piece that makes a formal
trial's textual-vs-semantic comparison technically meaningful rather than
merely a naming convention.

## Why a second isolation layer

The repository is now public. Hiding `reference/` and `expected/` from a
prepared trial *directory* (what `scripts/prepare-run.sh` already does) is
not sufficient on its own if the trial's execution environment can simply
reach the internet and fetch this repository's own `reference/`/`expected/`
files from GitHub. Isolation M1 adds a **network-disabled container
sandbox** around trial execution, on top of (not instead of) the existing
directory-level isolation.

## A. Protected host-side material

The sandbox must never expose:

- `benchmarks/ai-native/v1/reference/` (known-correct solutions)
- `benchmarks/ai-native/v1/expected/` (expected stdout)
- `scripts/validate-run.py`, `scripts/summarize-results.py`, or any other
  validator/scoring logic
- previous trials' `result.json` files or summaries
- the KAI-CC repository's `.git` history and any other repository file not
  explicitly copied into the trial workspace
- other benchmark tasks' `TASK.md`/source (a task-01 trial must not see
  `tasks/task-02.md`)
- the invoking host's real `$HOME`, SSH keys, shell config, browser data,
  or any other host filesystem content outside the prepared workspace

## B. Permitted trial-visible material

A prepared workspace (`<trial>/workspace/`, bind-mounted read-write as
`/workspace` inside the sandbox) contains exactly:

- `TASK.md` — that task's agent-visible prompt
- `benchmark.kai` — the pristine baseline source
- `trial.json` — the sandbox-visible manifest (schema below)

Nothing else. A future condition-specific tool adapter (compiler/semantic
queries) is a separate, explicitly-provided interface — see "Tooling
surface" below — never the repository itself.

## C. Outputs permitted to leave the sandbox

Only explicit, host-initiated collection is permitted:

- the final `benchmark.kai` (the agent's edited solution)
- `result.json` metrics recorded by the operator (per the existing
  measurement protocol in `README.md`)
- a command/tool transcript, if instrumentation is enabled in a future
  milestone
- timing/statistics captured by the host wrapper, not by the sandbox
  itself

`benchmark.kai` collection is implemented and tested
(`scripts/collect-isolated-trial.sh`) — see "Result collection and the
validation boundary" below for the exact mechanism, including its
symlink/special-file defenses. What Isolation M1 does *not* yet do is
invoke a real agent to produce something worth collecting (see "Status"
above) — the collector itself is ready for that future milestone to use
unmodified.

## D. Threats this milestone defends against

- accidental context contamination from unrelated repository files
- agent filesystem traversal beyond the prepared workspace
- reading `reference/`/`expected/` solutions, directly or via traversal
- reading the source repository (including via a known absolute host path)
- `.git` history/metadata inspection
- reading previous trial results or summaries
- host `$HOME` access (SSH keys, config, tokens, browser data)
- sibling-task inspection (e.g. a task-01 trial seeing task-02's prompt)
- **internet retrieval of the public repository or reference answers** —
  addressed by disabling the sandbox's network entirely
  (`--network=none`), not merely by hiding files on disk

## E. Threats neither M1 nor M2 solves

Stated explicitly, per the project's own convention of never claiming
stronger isolation than implemented. (Textual-vs-semantic tool
enforcement — previously listed here as M1's single biggest gap — is now
implemented; see "Isolation M2" below.)

- **Model-provider training contamination cannot be controlled.** A model
  may already have memorized KAI's public repository, including
  `reference/`/`expected/`, from its training data. Filesystem/network
  sandboxing isolates trial-visible **tools and context**, not what a
  model already "knows." See "Explicit limitations" below.
- **No agent adapter exists yet.** M2 hardens the tool boundary a future
  agent will use; it does not itself integrate Claude/OpenAI/Codex/
  Copilot/MCP. A real adapter needs authenticated API access to reach its
  model provider — the intended architecture keeps that host-side model
  adapter *outside* the networkless execution sandbox, relaying only
  narrowly-defined tool requests in. Designing and building that adapter
  is future work (see "Recommended next milestone" territory).
- **Model nondeterminism** is unaffected by this sandbox and is out of
  scope here.
- **Token accounting differences between providers** are unaffected by
  this sandbox.
- **Broker transcript sequence numbers are per-invocation, not
  per-trial.** See Isolation M2's "Host-only tool transcript" section for
  what this means in practice and how a future long-lived session avoids
  it.

## Directory layout

```
/tmp/kai-ai-native-v1/isolated/<trial-id>/
    host/
        orchestration.json    # host-only; NEVER mounted into the sandbox
        result/                # M1 - collector output; NEVER mounted
        broker/                 # M2 - host-only; NEVER mounted
            scratch/              # per-request TOCTOU-safe copies
            transcript.jsonl       # append-only tool-call audit log
    workspace/                  # bind-mounted read-write as /workspace
        TASK.md
        benchmark.kai
        trial.json              # sandbox-visible manifest
        benchmark_out            # M2 - appears after a successful compile
    tools/                      # M2 - bind-mounted READ-ONLY as /tools
        _client.py                # generic transport, both conditions
        kai-compile                # both conditions, byte-identical
        kai-inspect                # semantic only
        kai-definition              # semantic only
        kai-references              # semantic only
        kai-callers                  # semantic only
        kai-callees                   # semantic only
        kai-call-graph                 # semantic only

/tmp/kai-ai-native-v1/toolchains/<staged-id>/   # M2 - staged, verified
                                                  # kaicc; NEVER mounted
/tmp/kai-ai-native-v1/sockets/<trial-id>.sock    # M2 - the ONE file
                                                   # bind-mounted into the
                                                   # sandbox, as
                                                   # /run/kai-tool-bridge.sock
                                                   # (a top-level, short,
                                                   # flat path - AF_UNIX
                                                   # socket paths are
                                                   # limited to ~108 bytes
                                                   # on Linux, and nesting
                                                   # it under the
                                                   # read-only /tools
                                                   # mount was found to
                                                   # leave a container-
                                                   # privileged,
                                                   # host-unremovable
                                                   # directory behind
                                                   # under rootless
                                                   # Podman)
```

`/tmp/kai-ai-native-v1/isolated/` is a **sibling** of, never inside,
`scripts/prepare-run.sh`'s own `/tmp/kai-ai-native-v1/` trial root — the
two preparation mechanisms (uncontained vs. containerized) never collide.
Override the root with `$KAI_BENCH_ISOLATED_ROOT` or `--output-root`.

Only `workspace/` is ever bind-mounted into the container. `host/` never
leaves the host filesystem.

## Trial manifest (`trial.json`, schemaVersion 1)

```json
{
  "schemaVersion": 1,
  "benchmarkVersion": "ai-native-v1",
  "trialId": "task-01-textual-20260827T010904Z-15dcd7b5",
  "taskId": "task-01",
  "condition": "textual",
  "createdAt": "2026-08-27T01:09:04Z",
  "inputHashes": {
    "benchmark.kai": "sha256:...",
    "TASK.md": "sha256:..."
  },
  "allowedFiles": ["TASK.md", "benchmark.kai", "trial.json"],
  "toolPolicyId": "textual-v1"
}
```

`inputHashes` lets a reviewer later prove that a textual and a semantic
trial for the same task started from byte-identical `TASK.md`/
`benchmark.kai` — never reconstruct trust from memory. `toolPolicyId` is
currently a **label only** (see "enforcement status" below) — it does not
yet gate anything technically.

Deliberately **excluded** from `trial.json`: reference solution content,
expected output content, validator logic, or any scoring data. Host-only
orchestration metadata (`host/orchestration.json`) may carry additional
information (absolute host paths, the benchmark root) precisely because it
is never mounted into the sandbox.

## Container isolation mechanism

- **Engine:** rootless Podman preferred, Docker fallback — mirrors
  `scripts/build-release-linux-x86_64.sh`'s own detection logic via the
  shared `scripts/isolation/container-engine.sh` helper (never a third,
  independently-drifting copy of that ~10-line detection).
- **Image:** `benchmarks/ai-native/v1/sandbox/Containerfile`, a small base
  pinned by exact manifest digest —
  `docker.io/library/debian@sha256:5ae3c39eb...` (the amd64 manifest for
  the `12-slim` line, resolved and verified working on 2026-08-27; a bare
  `12-slim` tag is version-tagged but not immutable, since Debian
  periodically rebuilds it in place with security patches under the same
  tag — see the Containerfile's own comment for how to re-resolve and
  update this digest later) — with only `bash`, `coreutils`, `git`, and
  `ca-certificates` — no compiler, no LLVM. This is deliberately not the
  heavy release-builder image
  (`release/linux-x86_64/Containerfile`); nothing in M1's isolation
  verification needs a real compiler.
- **No repository mount, no HOME mount:** the container never receives
  `-v <repo>:...` or `-v $HOME:...` or any equivalent. Only
  `<trial>/workspace/` is bind-mounted, as `/workspace`.
- **Network:** `--network=none` — no network namespace access at all, not
  merely a restricted one.
- **Non-root:** the runtime UID/GID is set to match the host user that
  owns the bind-mounted workspace (`--user "$(id -u):$(id -g)"`, plus
  `--userns=keep-id` under rootless Podman so that mapping is exact) —
  never uid 0. The image also bakes in a synthetic non-root `sandbox` user
  as a defense-in-depth default for anyone who runs the image directly
  without the wrapper script.
- **No elevated privileges:** `--cap-drop=ALL`,
  `--security-opt=no-new-privileges`. No `--privileged`, no host PID
  namespace, no host network, no Docker/Podman socket mount.
- **Read-only root filesystem:** `--read-only`, with `/tmp` and
  `/home/sandbox` provided as small `tmpfs` mounts (`mode=1777`, like
  `/tmp` conventionally is) so ordinary scratch/write needs work without
  ever needing `chmod 777` on anything persistent. `/workspace` itself is
  writable because it is its own bind mount, independent of the root
  filesystem's read-only flag.
- **Explicit environment allowlist:** only `LANG`, `LC_ALL`, `PATH`, and
  `HOME` are set, to fixed deterministic values
  (`LANG=C.UTF-8`/`LC_ALL=C.UTF-8` so correctness never depends on the
  developer's own locale). No host environment variable is forwarded
  implicitly — no API keys, tokens, SSH agent sockets, or `XDG_*` paths
  ever reach the sandbox.
- **No `.git`:** the workspace is built by copying two individual files
  (`benchmark.kai`, `TASK.md`) plus a generated `trial.json` — never a
  directory copy — so no `.git`/`.gitmodules`/worktree metadata can ever
  end up inside it, verified directly (see automated tests below).

## Isolation M2: enforced condition-specific tool boundary

**Status: implemented.** M1 left the textual/semantic split enforced only
by "what the human operator chooses to make available" — a wrapper-naming
convention, not a technical barrier. Isolation M2 closes that gap: **a
textual trial is now technically unable to invoke KAI semantic queries**,
regardless of what it discovers about its own tooling or how cleverly it
tries to bypass it.

### Architecture

```
sandbox tool client (/tools/kai-* or /tools/_client.py)
        |
        | JSON over a Unix-domain socket (/run/kai-tool-bridge.sock)
        v
HOST-SIDE broker (scripts/isolation/broker.py) <-- THE enforcement boundary
        |
        v
staged, verified kaicc (scripts/isolation/stage-toolchain.sh output,
                         under /tmp/kai-ai-native-v1/toolchains/ - NEVER
                         mounted into any sandbox)
```

**Raw `kaicc` never enters the sandbox.** It is staged and verified
entirely on the host (see "Compiler staging" below) and invoked only by
the broker process, which also runs entirely on the host. Nothing the
sandbox can do reaches the compiler binary directly - confirmed by
`scripts/test-tool-boundary.sh`'s search for `kaicc`/
`libkai_runtime.a`/`libz3.so*` anywhere reasonable inside a running
sandbox (finds nothing, in both conditions).

### The authoritative condition source

The broker reads a trial's condition **exactly once, at broker startup**,
from `<trial>/host/orchestration.json` — the same host-only file M1
already established as authoritative, never mounted into any sandbox.
**It never reads `workspace/trial.json` for this purpose.** That file is
sandbox-visible and sandbox-writable, and is therefore untrusted input,
used only for display/audit.

Tested directly: editing a prepared textual trial's
`workspace/trial.json` from `"textual"` to `"semantic"` and then
requesting a semantic operation is still denied — the running broker's
authorization decision was fixed at startup from the host-only file and
cannot be changed by anything the sandbox does afterward.

### Why a wrapper name alone is not enforcement

The generated `/tools/_client.py` is a dumb, **condition-blind** JSON
transport client, present identically in both conditions. It has no
authorization logic at all - it will forward whatever operation name it
is asked to send. This is intentional, not an oversight: the security
boundary is the **host broker's** own condition check, not which wrapper
scripts happen to exist. Both are tested explicitly:

- a textual trial invoking `/tools/_client.py inspect` directly (bypassing
  the absent `kai-inspect` wrapper) is **denied by the broker**
- a textual trial that hand-crafts the raw wire protocol over the socket
  itself, without using `_client.py` at all, requesting `call-graph`, is
  **also denied by the broker**, with the identical reason

Absence of a `kai-inspect` wrapper in a textual trial's `/tools/` is
convenience/UX for the common case — it means a well-behaved agent never
even sees a semantic command to try. It is not what actually stops a
determined one.

### Compiler staging

`scripts/isolation/stage-toolchain.sh --compiler-root <portable-install>`
copies a portable KAI-CC release tree (e.g. `dist/kai-linux-x86_64/`) to
`/tmp/kai-ai-native-v1/toolchains/<staged-id>/` — outside the repository,
never mounted into any sandbox — after confirming the source actually has
the expected `bin/kaicc` + `lib/kai/libkai_runtime.a` shape (refuses
`build/`, `compiler/`, or anything else that isn't a real portable
release). It then verifies the staged copy by actually running
`kaicc --version` (never assuming any particular version string) and
records a host-only manifest: `stagedId`, `sourceRoot`, `stagedRoot`,
`compilerVersion`, and SHA-256 of `bin/kaicc` and every `lib/kai/*` file
present (including `libz3.so.4` when the source build bundled it).
`scripts/tool-sandbox-exec.sh` re-stages and re-verifies fresh on every
invocation, so the broker never operates on a copy whose integrity it
merely assumes.

### Condition-specific tool surface

`scripts/isolation/generate-tool-surface.sh` writes, per trial, into
`<trial>/tools/` (bind-mounted **read-only** as `/tools`):

| File | Textual | Semantic |
|---|---|---|
| `_client.py` (generic transport, no authorization logic) | present | present |
| `kai-compile` | present | present, **byte-identical** to textual's copy |
| `kai-inspect` | absent | present |
| `kai-definition` | absent | present |
| `kai-references` | absent | present |
| `kai-callers` | absent | present |
| `kai-callees` | absent | present |
| `kai-call-graph` | absent | present |

`kai-compile` being byte-identical between conditions is verified
directly (`scripts/isolation/generate-tool-surface.sh`'s own logic writes
it from one shared code path regardless of condition) - the semantic
condition never receives a nicer or different compile interface, only
additional capability on top of the identical baseline.

### Wire protocol (schemaVersion 1)

One JSON object per Unix-stream connection, client sends then shuts down
its write side, broker responds then closes:

```json
// request
{"schemaVersion": 1, "operation": "inspect", "params": {}}
// request (position-based)
{"schemaVersion": 1, "operation": "definition", "params": {"line": 14, "column": 4}}
// response
{"schemaVersion": 1, "operation": "inspect", "allowed": true,
 "exitCode": 0, "stdout": "...", "stderr": ""}
// response (denied)
{"schemaVersion": 1, "operation": "call-graph", "allowed": false,
 "exitCode": null, "stdout": "", "stderr": "",
 "error": "operation 'call-graph' is not permitted for this trial's condition ('textual')"}
```

Known operations: `compile`, `inspect`, `definition`, `references`,
`callers`, `callees`, `call-graph` — the exact set the real `kaicc` CLI
supports today (see `docs/CLI.md`), no hypothetical future commands. The
client can **never name an arbitrary path** — every operation always
operates on that trial's own `workspace/benchmark.kai`.

**Params are schema-validated per operation, not merely read** —
`validate_params()` rejects unknown/extra keys, missing required keys,
and wrong types outright, before the source is even read:

| Operation | Accepted `params` |
|---|---|
| `compile`, `inspect`, `call-graph` | none (`{}` only) |
| `definition`, `references`, `callers`, `callees` | exactly `{"line": <positive int>, "column": <positive int>}` |

A request with any other key — `path`, `source`, `workspace`, `trialId`,
`condition`, `executable`, `command`, `env`, an output path, or anything
else — is rejected with `invalid params`, never silently ignored and
never executed. Tested directly: injecting `path`/`trialId`/`condition`
into `params` cannot select a different source file, another trial, or a
different condition.

Hardening: requests over 64 KiB are rejected — the broker stops reading
immediately upon detecting the overrun, drains and discards the
remainder off the socket (so the connection closes cleanly rather than
being TCP-reset, which was observed to otherwise prevent the client from
ever seeing the rejection response) without holding, parsing, or logging
any of the oversized payload, and continues serving further requests
normally; malformed (non-JSON) requests are rejected; unknown operation
names are rejected distinctly from "known but not permitted for this
condition"; every `subprocess.run(...)` invocation of `kaicc` uses
`shell=False` with an explicit argument list (never a shell string, never
`eval`); each invocation has a bounded timeout (30s compile, 15s
queries); the subprocess environment is an explicit allowlist (`PATH`,
`LANG`, `LC_ALL` only) — never the broker's own inherited environment.
The host-only transcript entry for any rejected request (oversized,
malformed, or invalid params) is exactly as small as any other entry —
`normalizedArgs` only ever contains a clean `{line, column}` pair or
`{}`, never an echo of arbitrary client-supplied keys/values — so a
malicious payload can never turn into unbounded or unsafe transcript
data. All of the above is exercised by real socket-level tests in
`scripts/test-tool-boundary.sh`, not by inspecting `broker.py`'s source.

### Symlink/special-file defense at tool-invocation time

M1's collector protects a one-shot, post-exit file copy. M2's broker
faces a **live** trial that can make many requests over the sandbox's
whole lifetime, so the same defense is applied on **every single
operation**, not just once: before reading `workspace/benchmark.kai` for
any `compile`/semantic-query request, the broker `lstat()`s it on the
host, refuses anything that is not a genuine regular file (symlink, FIFO,
socket, device, directory), confirms the resolved real parent directory
is still the workspace, and opens it with `O_NOFOLLOW` as a second,
TOCTOU-resistant check — the identical mechanism and rationale as
`scripts/collect-isolated-trial.sh` (see "Result collection" above).
Tested directly: an absolute symlink to a real host file, a relative
symlink escaping the workspace, and a FIFO are all refused before
`kaicc` ever runs, and the FIFO case refuses immediately rather than
blocking (opening a FIFO for reading blocks until a writer connects).

### Host-only tool transcript

Every broker request — allowed or denied — is appended to
`<trial>/host/broker/transcript.jsonl` (never inside `workspace/`, never
sandbox-visible): `schemaVersion`, `sequence`, `timestamp`, `trialId`,
`condition`, `operation`, `normalizedArgs` (e.g. `{"line":14,"column":4}`
— never a host path), `allowed`, `exitCode`, `durationMs`, `stdoutBytes`/
`stderrBytes` (byte counts only, never full output content, and never
secrets). **Note:** `sequence` numbers restart at 1 for each separate
`scripts/tool-sandbox-exec.sh` invocation (each one starts a fresh broker
process for the one sandbox command it launches) — a real trial that
needs a persistent multi-call session should run one long-lived command
(e.g. an interactive shell) as that single invocation's command, so all
of its tool calls share one broker process and one monotonic sequence.

### A discovered SELinux interaction (Fedora/rootless Podman)

Passing the bridge socket into the sandbox initially failed with
`PermissionError: [Errno 13] Permission denied` on Fedora, even after
podman's `:Z` correctly relabeled the socket's on-disk path to
`container_file_t` (verified with `ls -Z` before/after) - SELinux's
`unix_stream_socket connectto` check is evaluated against the *listening
process's* own domain (the host broker, running unconfined), not solely
the path's on-disk label, so relabeling the path alone could not fix it.
The fix, `--security-opt label=disable` on the sandbox container, is the
standard, narrowly-scoped mechanism for exactly this host↔container
Unix-socket pattern: it affects **only** this container's SELinux (MAC)
confinement for filesystem/socket access. `--network=none`,
`--cap-drop=ALL`, `--security-opt=no-new-privileges`, `--read-only`,
non-root execution, and every mount restriction remain fully independent
DAC/namespace controls and are unaffected. It is a no-op on non-SELinux
hosts (e.g. Docker on Ubuntu GitHub Actions runners using AppArmor).

## Result collection and the validation boundary

**Trial sandbox** (inside the network-disabled container):
sees the task, edits `benchmark.kai`, invokes whatever tools are actually
made available to it, cannot see any hidden answer/validator.

**Host validator** (outside the sandbox, on the normal host):
receives only the explicitly collected `benchmark.kai` (and, later,
recorded metrics), has access to `reference/`/`expected/` as needed, and
performs scoring via the existing `scripts/validate-run.py` — completely
unmodified by this milestone. Hidden validation logic must never be
mounted into the trial sandbox.

### Safe result collection (implemented)

`scripts/collect-isolated-trial.sh --trial <trial-id> [--root DIR]` is the
**only** sanctioned way to move a trial's output from the workspace into a
host-only result area:

```
<trial>/host/result/benchmark.kai
<trial>/host/result/collection.json   # {schemaVersion, trialId, collectedAt, sha256}
```

**Only `benchmark.kai` is permitted to cross from the trial workspace into
the host result area.** `TASK.md`, `trial.json`, the rest of the
workspace, the container filesystem, and any other file a trial happens
to create are never copied — the collector's implementation never globs
or loops over workspace contents; it reads exactly one hardcoded
filename.

**Why a naive `cp` is unsafe, and what this script does instead:** a
sandboxed process can freely rewrite the *contents* of `/workspace`
(it is the one writable bind mount), including replacing `benchmark.kai`
with a symlink to a real host file the sandbox itself could never read
directly (e.g. an SSH private key — no such path is bind-mounted into
it). The sandbox cannot read through that symlink itself, but a naive
host-side `cp benchmark.kai ...`, run *after* the container has exited and
therefore fully in the host's own filesystem namespace, could
dereference it and accidentally copy real host material into what looks
like a benign trial result.

The collector defends against this by never trusting the filesystem
entry's apparent type:

1. **`os.lstat()`** the exact path on the host — `lstat`, unlike `stat`,
   never follows a symlink, so this reports the type of the path itself.
2. **Reject anything that is not a regular file** (`stat.S_ISREG`) —
   symlink, FIFO, socket, block/character device, or directory are all
   refused immediately, before any `open()` call. This matters
   specifically for a FIFO: opening one for reading *blocks* until a
   writer connects, so the type check must gate before any open, not
   merely produce a slower failure.
3. **Defense in depth:** even for a confirmed regular file, its real
   (symlink-resolved) parent directory must still equal the workspace
   directory's own real path — guarding against a symlinked *ancestor*
   directory, not just the file itself.
4. **`os.open(path, O_RDONLY | O_NOFOLLOW)`** for the actual read — a
   second, TOCTOU-resistant guarantee of the same property, independent
   of the `lstat` result, in case anything changed between the two calls.
   (Collection is documented to run only after the one-shot sandbox
   command has already exited, so there is no concurrent sandbox writer
   in practice — this is nonetheless a cheap additional guarantee.)

Only after all of the above succeeds does the collector compute a SHA-256
of the file's bytes, write the copy to `<trial>/host/result/benchmark.kai`,
and record `collection.json`. On any rejection, nothing is written to the
result area at all.

Trial IDs passed to the collector are validated with the exact same
strict pattern `scripts/sandbox-exec.sh` and
`scripts/cleanup-isolated-trials.sh` already use (see
`scripts/isolation/trial-id.sh`) — the pattern permits no `/` character
anywhere, so a value matching it cannot contain `../`, a leading `/`, or
any other path-traversal component before it is ever used to build a
path.

### The conceptual full pipeline

A future orchestration layer should compose these three, already-usable
pieces in order:

```
scripts/sandbox-exec.sh <trial-id> -- <agent/tool invocation>
        |
        v   (only after the sandbox command has exited)
scripts/collect-isolated-trial.sh --trial <trial-id>
        |
        v   (host-only; benchmark.kai never touched reference/expected)
scripts/validate-run.py <task-number> <trial>/host/result/benchmark.kai
```

Each step is already independently usable and tested today; only the
"invoke a real agent inside step one" piece remains unimplemented,
because no agent runs in Isolation M1 (see "Status" at the top of this
document).

## How to audit a trial

A human reviewer should be able to answer all of the following purely from
files on disk plus the commands below — never by trusting a summary:

1. **What files was the agent given?** `ls -la <trial>/workspace/` — must
   be exactly `TASK.md`, `benchmark.kai`, `trial.json`.
2. **What exact starting source was used?**
   `sha256sum <trial>/workspace/benchmark.kai` and compare against
   `inputHashes["benchmark.kai"]` in `<trial>/workspace/trial.json`.
3. **Which condition was active?** `<trial>/workspace/trial.json`'s
   `"condition"` field, and `<trial>/host/orchestration.json` for the
   same, independently recorded host-side.
4. **Could it see `reference/`/`expected/`?** No — verify with
   `find <trial>/workspace -iname 'reference*' -o -iname 'expected*'`
   (must print nothing), and confirm the sandbox never received a
   repository bind mount at all (see item 5).
5. **Could it see the repository?** Run
   `scripts/sandbox-exec.sh <trial-id> -- test -e <absolute-repo-path>`
   and confirm it fails (non-zero exit, "No such file or directory").
6. **Was network disabled?** Run
   `scripts/sandbox-exec.sh <trial-id> -- timeout 3 bash -c 'cat < /dev/tcp/1.1.1.1/80'`
   and confirm it fails immediately with "Network is unreachable" (or
   equivalent) rather than succeeding or hanging.
7. **What files came back?** Exactly `<trial>/host/result/benchmark.kai`
   plus `collection.json` — produced only by
   `scripts/collect-isolated-trial.sh`, never more (see "Result
   collection" above for why a naive `cp` is unsafe and what this script
   does instead).
8. **Were textual and semantic trials seeded identically?** Compare
   `inputHashes` between the two trials' `trial.json` files for the same
   task — they must match exactly (see `scripts/test-isolation.sh`'s own
   "Section M" check, which asserts this automatically).
9. **Exact compiler version/hash?**
   `/tmp/kai-ai-native-v1/toolchains/<staged-id>.manifest.json`'s
   `compilerVersion` and `sha256["bin/kaicc"]` — recorded by
   `scripts/isolation/stage-toolchain.sh` from the actual staged binary,
   never assumed.
10. **Which tool operations were permitted, actually requested, and
    denied, with exit codes/timing?**
    `<trial>/host/broker/transcript.jsonl` — one JSON line per request,
    `allowed`/`exitCode`/`durationMs` for every one, host-only.
11. **Was raw `kaicc` trial-visible?** No — run
    `scripts/tool-sandbox-exec.sh <trial-id> --compiler-root <root> --
    bash -c 'which kaicc; find / -xdev -iname kaicc'` and confirm both
    print nothing.
12. **Could a textual trial elevate itself?** No — see the "Isolation M2"
    section's own tested cases (direct client bypass, raw-protocol
    bypass, `trial.json` tampering), all denied by the host broker
    independent of anything sandbox-visible.

## Explicit limitations

Filesystem/network sandboxing, as implemented here, does **not** prove:

- the model has never seen KAI's public repository in training
- the model has no memorized knowledge of KAI or this specific benchmark
- provider-side systems (rate limits, model snapshot, infrastructure) are
  identical between trials run at different times
- perfect reproducibility across stochastic inference

This milestone isolates trial-visible **tools and context** — it does not,
and cannot, claim model amnesia.
