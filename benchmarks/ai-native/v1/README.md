# AI-Native Benchmark v1: Textual vs. Semantic Agent Evaluation

This is a reproducible harness for comparing two agent workflows on the same
KAI code-editing tasks: one restricted to textual tools (file reads, grep),
the other additionally permitted to use KAI's compiler-backed semantic query
commands (`kaicc inspect|definition|references|callers|callees|call-graph`).

This directory provides the **fixture, tasks, and instrumentation/validation
protocol**. It does **not** run or score any agent itself - trials are
conducted manually, in separate fresh sessions, so tool availability can be
controlled per condition. See "What this milestone does NOT do" below.

## Hypothesis

> Providing an agent with compiler-backed semantic queries will reduce the
> amount of source exploration and/or number of repair iterations needed to
> complete semantic code-editing tasks, without reducing final correctness.

This is a conservative, falsifiable hypothesis. No claim is made about a
specific magnitude of improvement, and no data exists yet to support or
refute it - that is what running trials against this harness is for.

### Why semantic queries might help

Grep and file reads only see text. They cannot distinguish two locals that
happen to share a name in different scopes, cannot tell a user-defined
function from a builtin it shadows, and cannot directly answer "who calls
this function" without the agent re-deriving that by reading every call
site by hand. KAI's semantic tooling (Semantic Inspection Milestones 1-3)
answers exactly these questions directly from the compiler's own resolved
symbol table - so the theory is that an agent with access to it does less
manual re-derivation of information the compiler already computed. This
benchmark exists to check whether that theory holds up under actual agent
use, not to assume it.

## The two conditions

Both conditions receive the exact same starting source, the exact same task
text, the exact same compiler, and a fresh working copy (see "Preparing a
run" below).

**Condition A - Textual.** The agent may use source-file reads, textual
search (grep/ripgrep/similar), and `kaicc <file> -o <output>` to compile and
run. It may **not** use `kaicc inspect`, `kaicc definition`,
`kaicc references`, `kaicc callers`, `kaicc callees`, or `kaicc call-graph`.

**Condition B - Semantic.** Everything from Condition A, plus all of the
semantic query commands listed above.

The only difference between conditions is whether the semantic-query
commands are permitted. Everything else - task text, starting source,
compiler binary, completion criteria - is identical.

## Scope and limitations

- KAI's semantic queries are currently **single-source-file only**, so this
  benchmark is a single `.kai` file, not a multi-file/module scenario.
- The benchmark program stays within the current KAI 0.1 MVP backend
  subset: primitive `i64`/`bool` values, `let`/`mut`, arithmetic,
  comparisons, `if`/`else`, functions/parameters/calls, and primitive
  `print`. No strings, structs, enums, arrays, `for`, or generics.
- The benchmark is intentionally small (one ~230-line file, 27 functions).
  Results here characterize this scale of task, not arbitrarily large
  codebases.
- A single trial per condition is **not sufficient evidence** either way -
  see "Repeated trials" below.
- Token-usage metrics depend on what the model UI exposes for a given
  session and may simply be unavailable; do not fabricate them.
- Results reflect a specific compiler revision and this specific benchmark
  program; treat conclusions as scoped to this setup, not as a general
  claim about KAI or about semantic tooling in general.

## Benchmark program

`baseline/benchmark.kai` is a small order-pricing and risk-scoring
pipeline: given raw order inputs (unit price, quantity, membership status,
region, shipping speed), it computes subtotal, discount, tax, shipping, and
a final total, then separately derives a risk score used for reporting.
This gives a realistic, non-trivial direct call graph (`process_order` calls
five direct helpers, several of which call further helpers) plus natural
name reuse (a `value` local exists in two different scopes inside
`calculate_discount`, and `clamp_i64` has an unrelated parameter also named
`value`) without any artificial or adversarial naming.

The baseline compiles and runs today with the current `kaicc` - see
"Baseline validation" below for the captured proof.

## Tasks

Each task starts from the **same pristine baseline** - never from another
task's output, and never from a reference solution. Task prompts
(`tasks/task-01.md`, `task-02.md`, `task-03.md`) intentionally do not
mention which semantic-tooling advantage they're designed to probe; that
commentary is kept here so it cannot bias the semantic condition.

| Task | File change required | Semantic property probed |
|------|----------------------|---------------------------|
| 1 | Add a parameter to `calculate_shipping` and update all call sites | `callers` gives the exact direct call-site list precisely, vs. re-deriving it from a grep match list |
| 2 | Fix one of two same-named (`value`) locals in different scopes inside `calculate_discount`, without touching the other | `references` on a specific declaration resolves only that binding's actual uses; grep for `value` cannot distinguish the two scopes |
| 3 | Bring a function in line with a pattern used by another caller of the same helper | `callers`/`callees` on `risk_score_for_order` show directly which caller already applies `compute_risk_penalty` and which doesn't |

## Reference solutions

`reference/task-01.kai`, `task-02.kai`, `task-03.kai` are known-correct
solutions to each task, used only to (a) prove each task is actually
solvable and (b) generate the expected stdout in `expected/`.

**Agents running a benchmark trial must never read the `reference/`
directory.** It exists solely for harness validation and expected-output
generation, and reading it would trivially defeat the benchmark for either
condition.

## Isolation

`prepare-run.sh` creates the agent's workspace **outside the repository by
default** (`/tmp/kai-ai-native-v1/...` on the current Linux MVP), containing
only:

```
benchmark.kai   - the pristine baseline source
TASK.md         - that task's agent-visible prompt
result.json     - a prefilled, mostly-null template to fill in by hand
```

Nothing else. In particular, the workspace never contains `reference/`
solutions, `expected/` stdout, this README, or any other task's prompt -
an agent that can only see the generated directory has no way to discover
the answer, regardless of condition. This matters because an agent trial
run from a workspace inside the repository could stumble onto (or
deliberately search for) `benchmarks/ai-native/v1/reference/` through an
ordinary broad filesystem search, which would contaminate the trial.

**Required procedure for every measured trial:**

- Open **only** the generated run directory as the agent's workspace (e.g.
  point an editor/CLI session's working directory at it directly) - never
  the KAI-CC repository checkout.
- Do not give the agent repository access during a measured trial. It does
  not need the repository: the compiler can be invoked by its **absolute
  path** (e.g. `/home/you/KAI-CC/build/bin/kaicc benchmark.kai -o out`),
  which works identically from outside the repo.
- `reference/` and `expected/` must never be visible to, or readable by,
  the agent's session - this is exactly what running outside the repo
  guarantees by construction.
- Textual and semantic conditions get **identically isolated** workspaces
  - the only difference is which commands the operator permits the agent
    to use, never what the agent can see on disk.
- Do **not** paste expected stdout into the agent conversation, in the
  task prompt, in a system message, or anywhere else the agent can read it
  - the agent must reach the correct output by actually solving the task,
    and validation happens afterward, outside the agent's view.

### Isolation M1/M2/M3A: containerized sandbox + enforced tool boundary + agent orchestration (infrastructure only)

The steps above isolate a trial at the *directory* level, which is not
sufficient on its own now that this repository is public - a trial
environment with network access could simply fetch `reference/`/
`expected/` from GitHub. **Isolation M1** adds a network-disabled
container sandbox on top of this directory-level isolation:
`scripts/prepare-isolated-trial.sh`, `scripts/sandbox-exec.sh`,
`scripts/cleanup-isolated-trials.sh`, `scripts/collect-isolated-trial.sh`,
and `scripts/test-isolation.sh`. **Isolation M2** then makes the
textual-vs-semantic split a *technical* boundary rather than a naming
convention: raw `kaicc` never enters the sandbox, and a host-side broker
(`scripts/isolation/broker.py`) is the only thing that ever invokes it,
enforcing each trial's condition from host-only metadata regardless of
what the sandbox does or discovers
(`scripts/tool-sandbox-exec.sh`, `scripts/test-tool-boundary.sh`).
**Isolation M3A** adds a provider-neutral agent-orchestration layer
(`scripts/agent/`) that will drive a real model in a future milestone -
today it is exercised only by a deterministic, offline `ScriptedAdapter`
(`scripts/run-agent-dry-run.py`, `scripts/test-agent-adapter.py`), never
a real provider. See [`ISOLATION.md`](ISOLATION.md) for the full threat
model, exact mechanisms, and explicit current limitations. No formal
trial has been run through this sandbox yet; this is infrastructure, not
a result.

## Preparing a run

```sh
benchmarks/ai-native/v1/scripts/prepare-run.sh 01 textual
benchmarks/ai-native/v1/scripts/prepare-run.sh 01 semantic
```

This copies the pristine `baseline/benchmark.kai` (never a reference
solution) plus the task's prompt into a fresh, isolated directory outside
the repository:

```
/tmp/kai-ai-native-v1/task-01-textual/benchmark.kai
/tmp/kai-ai-native-v1/task-01-textual/TASK.md
/tmp/kai-ai-native-v1/task-01-textual/result.json
/tmp/kai-ai-native-v1/task-01-semantic/benchmark.kai
/tmp/kai-ai-native-v1/task-01-semantic/TASK.md
/tmp/kai-ai-native-v1/task-01-semantic/result.json
```

Override the output root with a third argument or the `KAI_BENCH_RUN_ROOT`
environment variable (argument takes precedence) if `/tmp` is unsuitable:

```sh
KAI_BENCH_RUN_ROOT=/some/other/path scripts/prepare-run.sh 01 textual
scripts/prepare-run.sh 01 textual /some/other/path
```

If a target directory already exists (e.g. running a second trial of the
same task/condition pair), a `-trial2`, `-trial3`, ... suffix is appended
automatically so prior runs and their `result.json` files are never
overwritten. This never touches the main repository working tree and never
requires `git reset` inside it.

Point the agent at the prepared directory (see "Isolation" above) and its
`TASK.md`. Note whether the session permits the semantic query commands per
that trial's condition (the harness does not enforce this at the tool
level - see "What this milestone does NOT do").

The in-repository `runs/` directory still exists and remains useful for the
*operator's own* script/reference-solution testing (as used throughout this
document's validation steps) - it must not be used as an actual agent
trial's workspace, since a workspace inside the repository defeats the
isolation this section describes.

## Validating a run

```sh
benchmarks/ai-native/v1/scripts/validate-run.py 01 /tmp/kai-ai-native-v1/task-01-semantic/benchmark.kai
```

This compiles the given source with the real `./build/bin/kaicc` (override
with `--kaicc`), runs the resulting executable, and compares its stdout
byte-for-byte against `expected/task-01.stdout`, reporting `RESULT: PASS`
or `RESULT: FAIL` (and exits `0`/`1` accordingly). It runs from inside the
repository and reads the source file the agent produced by path - the
validator itself is allowed to see `expected/`; the agent never is. Pass
`--write-result PATH` to also write a minimal JSON stub with the
validation outcome (or merge the result into the `result.json` that
`prepare-run.sh` already placed in the agent's workspace), which you then
extend by hand with the trial's manual metrics (see below).

## Measurement protocol

For each trial, record a `result.json` (see `result.schema.json` for an
annotated example) with **at least**:

```
condition, task, startTime, endTime, success, validationPass,
compilerInvocations, failedCompilerInvocations, textualSearches,
semanticQueries, sourceReads, sourceLinesRead (when reasonably measurable),
notes
```

If the model UI exposes token usage for the session, you may additionally
record `inputTokens`, `outputTokens`, `totalTokens` - these are optional and
must be left `null` rather than estimated or fabricated when unavailable.

### Primary vs. exploratory metrics

**Primary** (used to judge task outcome and effort): `validationPass`,
`taskCompleted`, `elapsedTime`, `compilerInvocations`,
`failedCompilerInvocations`, `sourceReads`, `textualSearches`,
`semanticQueries`.

**Optional/exploratory**: `sourceLinesRead`, `inputTokens`, `outputTokens`,
`totalTokens`.

Report raw metrics. Do not collapse these into a single composite
"AI-native score" - with only a handful of trials, a composite number
would imply more precision than the data supports. A composite score, if
ever useful, should only be considered after several trials per condition
exist.

### result.json schema (schemaVersion 1)

See `result.schema.json` in this directory for the full annotated example.
Top-level shape:

```json
{
  "schemaVersion": 1,
  "benchmark": "kai-ai-native-v1",
  "task": "task-01",
  "condition": "semantic",
  "startTime": "...",
  "endTime": "...",
  "success": true,
  "validationPass": true,
  "metrics": { "...": "see result.schema.json" },
  "notes": "..."
}
```

## Aggregating results

Once multiple `result.json` files exist (under the isolated run root, e.g.
`/tmp/kai-ai-native-v1/`, or copied into the in-repo `runs/` afterward -
`summarize-results.py` only reads `result.json`, never `benchmark.kai`, so
copying completed results back into the repo for aggregation does not
reintroduce any isolation risk):

```sh
benchmarks/ai-native/v1/scripts/summarize-results.py /tmp/kai-ai-native-v1/
```

groups them by `(task, condition)`, reports `validationPass` counts, and
prints raw metric values plus simple averages. It never modifies or
deletes the underlying `result.json` files. This is intentionally a plain
stdlib script, not an analysis pipeline - treat its output as a starting
point for manual review, not a final verdict.

## Fairness protocol

Both conditions:

- use the same KAI compiler revision
- receive identical task text (`TASK.md`, verbatim, copied from
  `tasks/task-NN.md`)
- start from the same pristine baseline source
- must not see the reference solution
- should run in fresh sessions (a session that has already seen another
  condition's trial, or the reference solutions, is not a valid trial)
- have the same completion criteria (compiles, runs, exact expected stdout)
- get **identically isolated** workspaces (see "Isolation" above) - the
  workspace's contents and access level never differ between conditions,
  only which commands the operator permits inside it

The only difference: the Textual condition must not use
`kaicc inspect|definition|references|callers|callees|call-graph`; the
Semantic condition may use them. Task prompts do not hint at which semantic
command (if any) is relevant to a given task - the semantic-condition agent
decides how, or whether, to use the available tools.

In addition, for every trial regardless of condition:

- open only the generated run directory as the agent's workspace; never
  grant the agent access to the KAI-CC repository checkout itself
- `reference/` and `expected/` must never be visible to the agent's session
- the compiler may be invoked by its absolute path from within the isolated
  workspace (no repository access is needed to compile or run)
- never paste expected stdout into the agent conversation, task prompt, or
  any other agent-visible channel

See "Isolation" above for the full rationale and procedure.

## Repeated trials

**A single run per condition per task is not sufficient evidence of
anything.** Session-to-session and model variance is real. The recommended
protocol going forward is **at least 3 trials per task per condition**,
each in a fresh session, before drawing any conclusion - and even then,
treat results as directional rather than definitive given the small sample
size and the narrow scope of this benchmark (one file, one domain, three
tasks).

## What this milestone does NOT do

This harness does not invoke any model or agent itself, does not hold API
keys, does not add a model SDK dependency, and makes no network calls.
Agent trials are run manually, in separate sessions, so the operator can
control which tools are actually available to satisfy each condition's
rules - the harness does not (and cannot) enforce tool restrictions itself.
This milestone also does not add any new compiler feature or semantic query
command; it only exercises what Semantic Inspection Milestones 1-3 already
shipped.

## Interpreting results conservatively

Given the scope above: a handful of trials on one small, single-domain,
single-file benchmark should be read as an early, narrow signal about
whether this specific tooling helps with this specific class of task - not
as a general verdict on KAI, on semantic tooling, or on AI-native language
design. Treat any observed difference as a hypothesis worth further,
larger-scale investigation rather than a proven result.
