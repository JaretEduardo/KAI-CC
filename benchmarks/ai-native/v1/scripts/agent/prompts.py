"""AI-NATIVE BENCHMARK - AGENT ADAPTER M3A: canonical, condition-neutral
prompt and tool-schema construction.

The textual and semantic conditions receive the EXACT SAME system
message, user message, initial source, and baseline tool schemas. The
ONLY difference is that the semantic condition's tool list additionally
includes the semantic-query schemas - see build_tool_schemas() below.
Neither the system prompt nor the user prompt ever mentions "semantic
mode" or otherwise coaches one condition differently; see ISOLATION.md's
"Isolation M3A" section for the fairness rationale (mirrors the
benchmark's own existing fairness protocol for human trials).

Nothing here exposes: the trial ID, host paths, the reference solution,
expected stdout, or the validator's implementation.
"""

import hashlib
import json

# Deliberately does not name a condition, does not say "use semantic
# tools aggressively" or similar, and does not describe the tool list
# itself (the tool list is provided separately, out-of-band, as
# structured tool schemas - exactly like a real provider's function-
# calling API) - identical text for every trial regardless of condition.
SYSTEM_PROMPT = (
    "You are assisting with a small, self-contained programming task in "
    "the KAI language. You will be given a task description and the "
    "current contents of a single source file. You have a small set of "
    "tools to read the source, propose a full replacement for it, "
    "compile it, run the compiled program, and finish when you believe "
    "the task is complete. Use only the tools provided to you - you do "
    "not have general file, shell, or network access."
)


def build_user_prompt(task_markdown, source_text):
    """Builds the single canonical user message. Identical structure and
    wording for both conditions given the same task/source - only the
    embedded task/source text ever varies, and only by task, never by
    condition."""
    return (
        "## Task\n\n"
        f"{task_markdown.strip()}\n\n"
        "## Current source (benchmark.kai)\n\n"
        "```kai\n"
        f"{source_text}\n"
        "```\n"
    )


# Baseline tools: present, byte-/structure-identical, in BOTH conditions.
# Deliberately narrow - no generic file/path/shell parameters anywhere.
# See ISOLATION.md's "Agent-visible tools" section for why each one is
# shaped this way.
BASELINE_TOOL_SCHEMAS = [
    {
        "name": "read_source",
        "description": "Read the full current contents of benchmark.kai.",
        "parameters": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "replace_source",
        "description": "Replace the entire contents of benchmark.kai with new source content.",
        "parameters": {
            "type": "object",
            "properties": {"content": {"type": "string", "description": "The complete new source text."}},
            "required": ["content"],
            "additionalProperties": False,
        },
    },
    {
        "name": "compile",
        "description": "Compile the current benchmark.kai to a native executable.",
        "parameters": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "run",
        "description": "Run the most recently compiled program and return its output.",
        "parameters": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "finish",
        "description": "End the session. Call this once you believe the task is complete.",
        "parameters": {"type": "object", "properties": {}, "additionalProperties": False},
    },
]

# Semantic-only tools: present ONLY in the semantic condition, appended
# after the baseline tools - never replacing or altering them. Exactly
# the operations the real kaicc CLI/M2 broker support today (see
# docs/CLI.md and isolation/broker.py) - no hypothetical future commands.
SEMANTIC_TOOL_SCHEMAS = [
    {
        "name": "inspect",
        "description": "Return the compiler's resolved symbol table for the current source.",
        "parameters": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "definition",
        "description": "Resolve the symbol definition at a given 1-indexed source position.",
        "parameters": {
            "type": "object",
            "properties": {"line": {"type": "integer"}, "column": {"type": "integer"}},
            "required": ["line", "column"],
            "additionalProperties": False,
        },
    },
    {
        "name": "references",
        "description": "Find all references to the symbol at a given 1-indexed source position.",
        "parameters": {
            "type": "object",
            "properties": {"line": {"type": "integer"}, "column": {"type": "integer"}},
            "required": ["line", "column"],
            "additionalProperties": False,
        },
    },
    {
        "name": "callers",
        "description": "Find the direct callers of the function at a given 1-indexed source position.",
        "parameters": {
            "type": "object",
            "properties": {"line": {"type": "integer"}, "column": {"type": "integer"}},
            "required": ["line", "column"],
            "additionalProperties": False,
        },
    },
    {
        "name": "callees",
        "description": "Find the direct callees of the function at a given 1-indexed source position.",
        "parameters": {
            "type": "object",
            "properties": {"line": {"type": "integer"}, "column": {"type": "integer"}},
            "required": ["line", "column"],
            "additionalProperties": False,
        },
    },
    {
        "name": "call-graph",
        "description": "Return the whole file's direct call graph.",
        "parameters": {"type": "object", "properties": {}, "additionalProperties": False},
    },
]


def build_tool_schemas(condition):
    """Returns the exact list of tool schemas advertised to the model
    for the given condition. Textual gets BASELINE_TOOL_SCHEMAS only;
    semantic gets BASELINE_TOOL_SCHEMAS followed by
    SEMANTIC_TOOL_SCHEMAS - never a different/nicer baseline."""
    if condition == "semantic":
        return list(BASELINE_TOOL_SCHEMAS) + list(SEMANTIC_TOOL_SCHEMAS)
    return list(BASELINE_TOOL_SCHEMAS)


def sha256_of_text(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def sha256_of_json(obj):
    """Canonical (sorted-key, whitespace-free) JSON serialization before
    hashing, so the result does not depend on dict insertion order."""
    canonical = json.dumps(obj, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def common_baseline_tools_sha256(condition):
    """Hashes ONLY the leading baseline-tool slice of this condition's
    advertised schema (never the semantic-only tail) - used to prove the
    baseline tool surface is identical between conditions without
    assuming the underlying constant is shared (a future refactor could
    otherwise silently duplicate it and drift)."""
    tools = build_tool_schemas(condition)
    baseline_slice = tools[: len(BASELINE_TOOL_SCHEMAS)]
    return sha256_of_json(baseline_slice)
