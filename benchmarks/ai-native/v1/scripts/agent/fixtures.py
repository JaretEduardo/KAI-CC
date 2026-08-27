"""AI-NATIVE BENCHMARK - AGENT ADAPTER M3A: reusable ScriptedAdapter
fixture scripts, shared by run-agent-dry-run.py and test-agent-adapter.py.

None of these are benchmark solutions. `textual_happy_path` and
`semantic_happy_path` make a deliberately trivial, non-solving edit
(a comment appended to the pristine baseline) purely to exercise
read/write/compile/run/finish end to end - a dry-run failing hidden
validation is expected and fine (see ISOLATION.md). Never copy a
reference solution here.
"""


def call(call_id, name, arguments=None):
    return {"id": call_id, "name": name, "arguments": arguments or {}}


def response(text, calls, stop_reason="tool_calls"):
    return {"assistantText": text, "toolCalls": calls, "stopReason": stop_reason, "usage": None}


def textual_happy_path(initial_source):
    edited = initial_source.rstrip("\n") + "\n// scripted dry-run edit (M3A infrastructure test, not a solution)\n"
    return [
        response("Reading the current source.", [call("c1", "read_source")]),
        response("Applying a small, deliberately non-solving edit.", [call("c2", "replace_source", {"content": edited})]),
        response("Compiling.", [call("c3", "compile")]),
        response("Running.", [call("c4", "run")]),
        response("Ending this dry-run.", [call("c5", "finish")]),
    ]


def semantic_happy_path(initial_source, line=14, column=4):
    # line/column point at clamp_i64 in the shared baseline/benchmark.kai.
    return [
        response("Reading the current source.", [call("c1", "read_source")]),
        response("Checking a symbol's definition.", [call("c2", "definition", {"line": line, "column": column})]),
        response("Compiling.", [call("c3", "compile")]),
        response("Running.", [call("c4", "run")]),
        response("Ending this dry-run.", [call("c5", "finish")]),
    ]


def textual_illegal_semantic_call():
    """A textual-condition fixture that deliberately emits a semantic
    tool call it was never advertised - proves the orchestrator denies it
    independent of what the model attempts (see item 26/14)."""
    return [
        response("Attempting a semantic query anyway.", [call("c1", "inspect")]),
        response("Ending after the denial.", [call("c2", "finish")]),
    ]


def malformed_unknown_tool():
    return [response("Calling something that does not exist.", [call("c1", "delete_everything")])]


def malformed_bad_shape():
    # 'toolCalls' has the wrong TYPE entirely (a string, not a list) -
    # unambiguously invalid regardless of which fields are optional; the
    # orchestrator must reject this without crashing.
    return [{"assistantText": "this response is malformed on purpose", "toolCalls": "not-a-list-at-all"}]


def tool_call_limit_probe(count):
    calls = [call(f"c{i}", "read_source") for i in range(count)]
    return [response("Issuing many tool calls in one turn.", calls)]


def turn_limit_probe(turns):
    return [response(f"Turn {i}", [call(f"c{i}", "read_source")]) for i in range(turns)]
