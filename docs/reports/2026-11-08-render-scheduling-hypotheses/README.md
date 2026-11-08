# Render scheduling evidence

See [the analysis](../2026-11-08-render-scheduling-hypotheses.md).
This is instrumentation of the existing schedule, not an implementation or A/B
measurement of the two proposed schedules.

- `wait-profile.json`: ordinary benchmark output from the diagnostic executable,
  including all 1800 raw frame samples and physics accounting after 120 warm-up frames.
- `frame-trace.csv.gz`: lossless per-frame trace; parent/child timings overlap.
  `slot_target` and `slot_completed` are captured before the slot condition.
- `summary.json`: aggregates, six windows, wait counts, conditional CPU capacity
  calculation. The calculation holds observed mean costs constant; it is not an
  implemented architecture's measured FPS or a universal performance ceiling.
- `run-manifest.json`: before/after executable, INI and source hashes.
- `wait-profile.state.json`: runner state, child exit 0 and binary fingerprints.
- `sensors.json`: GPU frequency/power/thermal event reasons every ~5 seconds.
- `source-hashes.json`: owning sources used for the five diagnostic TUs.
- `harness/`: diagnostic source generator, headers, runner and checker.
- `evidence-hashes.json`: SHA-256 hashes of these committed evidence files (LF
  normalized text, compressed CSV bytes unchanged).

Full generated TUs, command responses, exe/PDB, logs and uncompressed CSV remain
under `Build/FrameScheduling-20261108/` locally. Ordinary App.exe and INI stayed
unchanged. No scheduling policy, physics setting, scene count or quality changed.

To reproduce in this workspace with the current ordinary Debug libraries/objects,
materialize the four harness files in that exact Build directory without
overwriting unrelated artifacts. Then run these commands sequentially:

```powershell
python Build/FrameScheduling-20261108/prepare.py
python Build/FrameScheduling-20261108/run.py
python Build/FrameScheduling-20261108/analyze.py
```

The header's absolute CSV path intentionally names the original checkout. Adjust
it explicitly for a different workspace. The prepare script asserts each source
replacement and App.obj replacement, keeps the evaluated Debug flags, and links
only to a separately named diagnostic executable.

The analyzer checks frame/tick counts, physics accounting at the warm-up boundary,
wait-category sums and wait nesting. Queue Submit timing and raw slot fences
provide checks that instrumentation is active even when actual fence waits are zero.
No regression suite for a new architecture is claimed: no new architecture was
implemented in this task.
