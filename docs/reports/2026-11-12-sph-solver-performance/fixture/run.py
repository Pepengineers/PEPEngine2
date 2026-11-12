from pathlib import Path
import json
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = Path(__file__).resolve().parent
runtime = root / 'Build/bin/x64/Release/SphSolverPerf'
name = sys.argv[1]
exe = runtime / (name + '.exe')
shutil.copy2(runtime/'SphSolverPerf.exe', exe)
inputs = [exe, *sorted(runtime.glob('*.dll')),
          *(out/p for p in ['FluidSolver.hlsl','FluidSolverSpatial.hlsl','FluidParameters.hlsli','SphSolverPerf.cpp','GDX12Fluid.cpp','source-manifest.json'])]
command = [sys.executable, str(root/'tools/agent-skills/peengine-perf/scripts/run_job.py'),
           '--cwd', str(runtime), '--state', str(out/(name+'.run.json')), '--timeout', '600',
           '--unset-env', 'PERF_JOLT', '--unset-env', 'PERF_CONTROLS']
for path in inputs:
    command += ['--fingerprint', str(path)]
command += ['--', str(exe), '--output', str(out/(name+'.samples.json')), *sys.argv[2:]]
monitor = subprocess.Popen(['powershell','-NoProfile','-File',str(out/'monitor.ps1'),'-OutputPath',str(out/(name+'.processes.jsonl'))],creationflags=subprocess.CREATE_NO_WINDOW)
sensor_file = (out/(name+'.sensors.csv')).open('w')
sensors = subprocess.Popen(['nvidia-smi','--query-gpu=timestamp,utilization.gpu,clocks.gr,clocks.mem,power.draw,temperature.gpu','--format=csv','-l','1'],stdout=sensor_file,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
try:
    raise SystemExit(subprocess.call(command,cwd=root))
finally:
    monitor.terminate()
    monitor.wait()
    sensors.terminate()
    sensors.wait()
    sensor_file.close()
