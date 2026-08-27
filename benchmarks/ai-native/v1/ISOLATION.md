# AI-Native Benchmark v1 — Isolation Threat Model (Isolation M1)

This document is the isolation/threat-model reference for the containerized
sandbox substrate under `scripts/prepare-isolated-trial.sh`,
`scripts/sandbox-exec.sh`, `scripts/cleanup-isolated-trials.sh`, and
`scripts/test-isolation.sh`. It supplements, and does not replace,
`README.md`'s existing "Isolation" section, which still governs the
original, uncontained workflow (`scripts/prepare-run.sh` +
`scripts/validate-run.py`).

**Status: infrastructure only.** As of Isolation M1, no formal benchmark
trial has been run. Formal trial counts remain:

```
textual  = 0
semantic = 0
```

This milestone exists to build and verify a sandbox substrate a future
agent adapter can safely use — it does not itself run an agent, and it
does not change those counts.

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

## E. Threats Isolation M1 does NOT yet solve

Stated explicitly, per the project's own convention of never claiming
stronger isolation than implemented:

- **Model-provider training contamination cannot be controlled.** A model
  may already have memorized KAI's public repository, including
  `reference/`/`expected/`, from its training data. Filesystem/network
  sandboxing isolates trial-visible **tools and context**, not what a
  model already "knows." See "Explicit limitations" below.
- **A future agent adapter's own network needs are unsolved.** A real
  Claude/OpenAI/other adapter needs authenticated API access to reach its
  model provider. This milestone deliberately does not solve that
  transport problem — the intended architecture keeps a host-side model
  adapter *outside* the networkless execution sandbox, relaying only
  narrowly-defined tool requests in, per the milestone's own instructions.
  Designing that boundary precisely is future work.
- **Model nondeterminism** is unaffected by this sandbox and is out of
  scope here.
- **Token accounting differences between providers** are unaffected by
  this sandbox.
- **Textual-vs-semantic tool enforcement is NOT yet implemented** — see
  "Current textual-vs-semantic enforcement status" below. This is the
  single most important remaining gap before a formal trial is credible.

## Directory layout

```
/tmp/kai-ai-native-v1/isolated/<trial-id>/
    host/
        orchestration.json    # host-only; NEVER mounted into the sandbox
    workspace/
        TASK.md
        benchmark.kai
        trial.json             # sandbox-visible manifest
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

## Tooling surface (M1 — not yet condition-specific)

M1 does not need to solve the final textual-vs-semantic compiler
capability boundary. It documents where a future tool adapter would be
provided: a controlled `/tools/`-style mount point, populated by first
staging the **minimum portable compiler installation** (e.g. a copy of
`dist/kai-linux-x86_64/`) to a location **outside the repository**, then
bind-mounting that staged copy **read-only**. The sandbox must never gain
access to compiler *source*, and must never mount `build/`, `dist/`, or
the repository directly "for convenience."

## Current textual-vs-semantic enforcement status

**Not yet implemented — filesystem/network isolation only.** `trial.json`
records a `toolPolicyId` (`textual-v1` or `semantic-v1`) as a **label**,
but nothing in this milestone technically prevents a process inside the
sandbox from invoking a semantic query command if one happened to be
present. Today, the textual/semantic split is still enforced only by
*what the human operator chooses to make available* (per the original
benchmark's own fairness protocol in `README.md`), exactly as it was
before this milestone.

**Isolation M2 must expose condition-specific tooling technically, not
just by instruction:**

- **textual:** compile/run + basic diagnostics only
- **semantic:** the same, **plus** `inspect`, `definition`, `references`,
  `callers`, `callees`, `call-graph`, and any future structured
  diagnostics/type queries

A textual trial must eventually be **technically unable** to invoke
semantic queries (e.g. by mounting a restricted wrapper binary/PATH that
simply doesn't expose them), not merely instructed not to use them.
Implementing that correctly was assessed as significant enough scope to
belong to Isolation M2, per this milestone's own instructions, rather than
being half-done here.

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

## Explicit limitations

Filesystem/network sandboxing, as implemented here, does **not** prove:

- the model has never seen KAI's public repository in training
- the model has no memorized knowledge of KAI or this specific benchmark
- provider-side systems (rate limits, model snapshot, infrastructure) are
  identical between trials run at different times
- perfect reproducibility across stochastic inference

This milestone isolates trial-visible **tools and context** — it does not,
and cannot, claim model amnesia.
