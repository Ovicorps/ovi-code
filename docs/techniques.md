# Agentic techniques

Techniques change the control flow around the selected provider client and
workspace tools. Choose one in Settings or press ++t++ from chat Normal mode.

| Technique | Behavior | Suggested use |
| --- | --- | --- |
| **Standard ReAct** | Alternates between model responses and tool calls until the model answers | Most coding tasks |
| **Chain of Thought** | Adds a step-by-step reasoning instruction before the ReAct loop | Multi-step diagnosis and planning |
| **Ralph Loop** | Reissues the same goal for a configured number of passes and stops early on `DONE` | Iterative refactors and repeated improvement |
| **RAG** | Searches the workspace for query keywords and prepends matching excerpts | Large or unfamiliar projects |
| **Deliberate [plugin]** | States and updates hypotheses while explicitly verifying changes | Demonstrating the bundled plugin system and verification-focused work |

Deliberate is bundled, loaded automatically, and selected for fresh
configurations. Existing users retain their saved technique. Configure its
behavior under **Settings → Plugins**.

## Execution limits

The **Agent** Settings tab controls safeguards around technique execution:

- **Tool-call limit** caps tool iterations in one standard ReAct pass.
- **Ralph passes** caps goal repetitions in Ralph Loop.
- **Plugin passes** caps host ReAct passes requested by a runtime plugin.
- **Temperature** controls sampling when the model supports it. A negative
  value omits the provider temperature parameter.

## Models and reasoning effort

The curated model picker records supported reasoning levels for each known
model. Settings only shows valid levels for the current selection.

**Auto** follows model capabilities. If a saved effort is unavailable after a
model change, OVI-CODE uses that model's automatic behavior rather than sending
an invalid effort.

Unknown custom model IDs expose only Auto because their capabilities are not in
the local curated list.

## Choosing a technique

- Start with the bundled **Deliberate** plugin to see plugin configuration and
  explicit verification in action, or use **Standard ReAct** for a plain loop.
- Use **Chain of Thought** when a task benefits from explicit decomposition.
- Use **Ralph Loop** when the model should inspect and improve its result over
  several passes.
- Use **RAG** when likely-relevant code needs to be surfaced before the first
  model response.
- Use a [technique plugin](plugins.md) for custom orchestration that must run
  without changing the OVI-CODE binary.
