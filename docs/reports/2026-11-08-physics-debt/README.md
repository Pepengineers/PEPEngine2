# Physics debt evidence

See [the analysis](../2026-11-08-physics-debt.md). These are diagnostic results,
not an implemented fix or performance acceptance.

- `ordinary-before.json`: unmodified App.exe, 120 warm-up + 1200 measured frames.
- `detailed.json`: nested Jolt profiler and per-step attribution; 120 + 1200.
- `coarse-before.json`, `relaxed.json`, `coarse-after.json`: one binary,
  default / doubled / default wall-time budget; 120 + 1200 each. Later GPU drift
  invalidates attributing the entire between-run difference to policy.
- `monitored.json`: last diagnostic version, default budget; 120 + 1800.
- `all-results.json`: per-step/window aggregates, profiler scopes/cycles and
  probe summaries. Overlapping thread/scoped times are not additive frame time.
- `*-sync-probe.csv`: 200 frozen-state samples after 20 probe warm-ups.
- `*-frames.csv.gz`, `*-steps.csv.gz`: lossless compressed raw default-budget
  frame/step data, including actual accumulator and completed steps.
- `monitored.sensors.json`, `sensors-parsed.json`: raw/parsed GPU clock event
  reasons and AC power. CPU PDH counters were unavailable; no CPU thermal claim.
- `manifest.json`: source/input/binary fingerprints, process states and evidence
  hashes. The ordinary executable is unchanged. The three policy runs share
  one diagnostic binary; the detailed and monitored versions differ.
- `physics-tests*`: newly executed existing PhysicsTests binary, exit 0.
- `harness/`: last version of temporary instrumentation/build/run/analysis tools.

The working artifacts remain at `Build/PhysicsDebt-20261108/`. In a checkout
with ordinary Debug App libraries/objects built, materialize the six harness
files into that exact directory, then run from the repository root:

```powershell
python Build/PhysicsDebt-20261108/prepare.py
python Build/PhysicsDebt-20261108/run.py App ordinary-new 1200 ordinary
python Build/PhysicsDebt-20261108/run.py PhysicsDebtDiag coarse-new 1200 coarse
python Build/PhysicsDebt-20261108/analyze.py coarse-new
python Build/PhysicsDebt-20261108/check_target.py Build/PhysicsDebt-20261108/ordinary-new.json
```

Run everything sequentially. `detail` enables sampled Jolt scopes; `relaxed`
doubles only the catch-up wall-time budget of the diagnostic copy. Both are
experimental modes. The original source files and Engine.ini are not edited.
The absolute output paths inside the headers intentionally refer to the original
workspace; adjust them explicitly for a different checkout. Never overwrite
unrelated existing artifacts when materializing the harness.

`check_target.py` returns 1 when average FPS is below 20 or endpoint debt grows
by more than one fixed step. Its failure reproduces the current target miss;
a pass is only a short-run screen, not proof of sustained bounded debt.

The full 245 MB nested Jolt event CSV is retained under Build and is not committed.
The exported 78-scope summary retains inclusive/exclusive cycle totals and counts.
The last harness reproduces the measurement method, not the exact source bytes
of every earlier diagnostic build. Intermediate diagnostic exes were overwritten;
their recorded hashes remain available. All tests/profiles retained the ordinary
Debug flags, checks, 60 Hz, CCD and four collision steps.
