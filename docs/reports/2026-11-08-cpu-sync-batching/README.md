# Evidence and reproduction

Diagnostic artifacts from `Build/CpuSyncOptimize-20261108` and
`Build/CpuSyncProfile-20261108`; executables/PDBs are intentionally not tracked.
Raw ordinary frame samples, physics clocks, input hashes, sensor series and
failed-run states are retained. `moving-summary.json` summarizes `moving-probe.csv`.
`profile/cpu-summary.json` uses frames with frame_index >= 121, matching the
120 warmup + 1200 measured convention in `profile/analyze.py`.

To reproduce on this checkout, first fully rebuild Debug App.Base and App with
MSBuild, including dependent libraries when needed. System's new virtual method
changes its ABI; do not link stale App.Base/App.obj. Keep normal evaluated Debug
flags and the existing optimized-Debug Jolt library. Keep the runtime Engine.ini;
its measured SHA256 is in each meta file. Asset files are not archived here.

Copy diagnostic scripts/headers back into the two original Build directories.
They deliberately resolve the repository from those directories and retain the
original absolute output paths. `run.py` uses the repository peengine-perf runner.
The generators consume the current MSBuild CL/link tlogs and assert replacement
sites; they are experiment tools, not build-system support.

```powershell
python Build/CpuSyncOptimize-20261108/build_probe.py
python Build/CpuSyncOptimize-20261108/run.py CpuSyncProbe moving-repeat 1200 ordinary
python Build/CpuSyncOptimize-20261108/build_matched.py
python Build/CpuSyncOptimize-20261108/run.py CpuMatched64 repeat-A 1200 ordinary
python Build/CpuSyncOptimize-20261108/run.py CpuMatched256 repeat-B 1200 ordinary
python Build/CpuSyncOptimize-20261108/test_core.py
python Build/CpuSyncProfile-20261108/prepare.py
python Build/CpuSyncProfile-20261108/run.py
python Build/CpuSyncProfile-20261108/analyze.py
```

Serialize runs/builds. For new FPS claims, cool down between runs, alternate A/B
ordering, control power settings and inspect sensor series. The recorded matched
A/B pair is thermally confounded and must not be used as a causal speedup/regression.
The moving probe compares restored component state with rotating variant order;
its extra work must not be interpreted as ordinary FPS.
