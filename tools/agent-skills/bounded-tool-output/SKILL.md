---
name: bounded-tool-output
description: Bound large tool results when searching code, inspecting logs, discovering tools, or reading chat histories.
---

# Bounded tool output

Choose the evidence needed for the next decision before retrieving it. Filter at the source or inside the tool runner, before printing into model context. Start with roughly 2,000 output tokens per call; expand when the decision requires more.

- **Code:** locate files with `rg --files` or `rg -l`, then retrieve the relevant symbol or line range. With a working language server, prefer symbol definitions and references. Scope searches away from generated output and vendored code unless they are the subject. For caller audits, retain the complete match count and inspect every relevant caller; a preview is not the complete result.
- **Tool discovery:** print matching tool names first, then the schema of the selected tool. Descriptions may contain entire schemas. Avoid printing a broad slice of `ALL_TOOLS` with descriptions attached.
- **Structured results:** select fields, aggregate counts, and page before printing. Keep the full result in a tool-side variable or local artifact when further queries are likely. Preserve IDs, paths, units, errors, and continuation cursors.
- **Chats:** request the smallest useful turn window, then select messages or statuses inside the runner. A per-item character limit does not bound a whole turn containing hundreds of items. Use compact status snapshots for progress checks; read full history only to resolve a specific question.
- **Builds and logs:** save stdout/stderr to a file; return the exit code, counts, decisive diagnostics, and log path. Preserve the original exit status when filtering. Reuse an existing job runner for long tasks and wait on its handle instead of repeatedly rereading unchanged logs.
- **Skills and references:** read the selected workflow branch. Reuse instructions already in context; after compaction, recover the relevant portion before loading a whole documentation chain.

When the tool reports truncation, narrow or page the query. Increasing the output cap is appropriate when the missing evidence is required. Label partial results and retain a route to the full data. Never treat an omitted error, caller, or test failure as absent.

For media, render the image/audio through the tool's native media helper; base64 and serialized media objects are not useful text output.

Stop retrieving when the required evidence is complete. Report measured byte/character reductions as such; they do not establish model-token, billing, or task-level savings.
