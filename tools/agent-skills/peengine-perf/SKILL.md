---
name: peengine-perf
description: Run PEPEngine2 performance experiments, paired benchmarks, and long Bistro soaks with compact logs and frozen inputs.
---

Use this for measurement mechanics. Keep the user's optimization objective and quality constraints; this skill does not authorize changing them.

## Before a run

Reuse a current repository harness when available. Past scripts under `Build/` are experiment artifacts: check their source assumptions before reuse. Prefer a normal build over recreating an instrumented build. Read [Bistro notes](references/bistro.md) only for physics-clock measurements, diagnostic linking, or Nsight capture.

Record the executable, config, camera, seed, counts, and whether instrumentation is present. Use unique executable names for baseline/candidate. In a shared checkout, freeze the agreed inputs; attribute other agents' changes separately. Serialize builds, benchmarks, captures, and stress tests.

## Run and wait

The bundled runner redirects child output to files, reports a short heartbeat, enforces timeout on its own process tree, and checks selected input hashes before/after:

```powershell
python <skill>/scripts/run_job.py --cwd <runtime> --state <artifacts>/run.json --timeout 800 --fingerprint <exe> --fingerprint <ini> --unset-env PERF_JOLT --unset-env PERF_CONTROLS --set-env PERF_SOAK=1 -- <exe>
```

`--unset-env` removes an inherited variable; an empty value remains a defined variable on Windows. Set only the controls intended for this run. Use a separate state path for each job. The runner preserves logs and returns a failing status for timeout, changed inputs, or nonzero child exit. It does not decide whether a performance change wins.

Launch through `exec_command` with a short initial yield. While a fixed-duration run is healthy, each model turn does only this:

1. Read the existing session's new output with a bounded wait up to 50 seconds and a small output budget.
2. Give a brief progress update when due, then wait again.
3. Diagnose only a new failure, changed input, timeout, or completed result.

Keep settled design decisions and unchanged risks settled during these waits. Analyze the completed measurements once. Do not repeatedly reopen scripts, rederive numerical proofs, or build another harness while polling. Continue independent authorized work only when it will not contaminate the measurement.

## Read results

Persist full output on disk; bring summaries into context. For MCP results, project the needed fields from `structuredContent` rather than printing both it and `content`. Discover only the needed tool schema; inspect failures with a bounded log tail.

For timing comparisons, retain raw samples, use the declared repeat/ordering rule, and report scatter. Keep ordinary executable timings separate from instrumented scopes. Report simulated seconds, fixed ticks, and debt alongside FPS when physics is involved. A timeout or changed fingerprint is censored evidence, not a successful sample.

Finish with accepted/rejected changes, tests, measured limits, and artifact paths. Apply the repository's normal commit policy only to task-owned changes.
