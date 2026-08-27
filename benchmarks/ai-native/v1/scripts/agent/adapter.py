"""AI-NATIVE BENCHMARK - AGENT ADAPTER M3A: provider-neutral adapter interface.

`AgentAdapter` is the abstraction a real model provider (Claude/OpenAI/
etc.) will implement in a future M3B milestone. M3A ships only
`ScriptedAdapter`, a deterministic, offline, fixture-replaying
implementation used to build and test the orchestration layer BEFORE any
real model is integrated.

A ScriptedAdapter session is NEVER a formal benchmark trial. It is never
scored, never counted toward the textual/semantic trial totals, and is
never presented as an AI result - see agent/orchestrator.py's `dryRun`
marking and ISOLATION.md's "Dry-run vs. formal trial" section.

Response shape (a plain dict, never a vendor-specific type):

    {
      "assistantText": str,
      "toolCalls": [
        {"id": str, "name": str, "arguments": dict},
        ...
      ],
      "stopReason": "tool_calls" | "end_turn" | "error",
      "usage": {"inputTokens": int|None, "outputTokens": int|None,
                "cachedInputTokens": int|None, "cost": float|None} | None
    }

Callers MUST validate this shape before trusting it (see
orchestrator.parse_adapter_response) - a real provider SDK's output could
itself be malformed or unexpected, and ScriptedAdapter's own test
fixtures deliberately include malformed shapes to prove that validation
actually rejects them.
"""


class AgentAdapter:
    """Provider-neutral adapter interface. Implementations receive only
    canonical messages, condition-appropriate tool schemas, and never see
    trial/condition/host-path selection - those remain orchestrator-
    authoritative (see orchestrator.py)."""

    @property
    def adapter_type(self):
        raise NotImplementedError

    def next_response(self, messages, tools):
        """messages: list of {"role": ..., "content": ...} dicts (plus
        "toolCalls"/"toolCallId" on assistant/tool turns).
        tools: the condition-appropriate list of tool schemas (see
        agent/prompts.py's build_tool_schemas()).
        Returns a plain response dict in the shape documented above."""
        raise NotImplementedError


class ScriptedAdapter(AgentAdapter):
    """Deterministic, offline adapter that replays a predefined sequence
    of fixture response dicts. Requires no network, no API key, no
    provider SDK. Ignores `messages`/`tools` entirely - it exists to
    exercise the ORCHESTRATOR's tool dispatch, validation, transcript,
    and limit-enforcement logic against known inputs, not to simulate
    realistic model reasoning.

    Never call this "Claude", "GPT", or any vendor name, and never let
    its output be mistaken for an AI benchmark result - see this module's
    own docstring."""

    def __init__(self, script):
        self._script = list(script)
        self._index = 0

    @property
    def adapter_type(self):
        return "scripted-v1"

    def next_response(self, messages, tools):
        if self._index >= len(self._script):
            # Fixture exhausted: behave like a well-formed model that
            # simply has nothing further to say, rather than crashing -
            # the orchestrator's own "no_tool_calls" termination path
            # handles this cleanly.
            return {"assistantText": "", "toolCalls": [], "stopReason": "end_turn", "usage": None}
        response = self._script[self._index]
        self._index += 1
        return response
