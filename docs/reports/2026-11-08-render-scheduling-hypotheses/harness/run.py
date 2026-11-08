from pathlib import Path
import hashlib,json,subprocess,threading,time,sys
out=Path(__file__).resolve().parent;repo=out.parents[1];runtime=repo/'Build/bin/x64/Debug/App'
exe=runtime/'FrameScheduleProbe.exe';ini=runtime/'Config/Engine.ini';ordinary=runtime/'App.exe'
inputs=[exe,ini,ordinary,*[repo/p for p in json.loads((out/'source-hashes.json').read_text())],repo/'Apps/App.Base/Include/App.Base/Systems/GPUDataUpdateSystem.h']
def hashes():return {str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
meta={'before':hashes(),'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip()}
samples=[];stop=threading.Event()
def monitor():
    while not stop.is_set():
        r=subprocess.run(['nvidia-smi','--query-gpu=timestamp,temperature.gpu,clocks.gr,power.draw,utilization.gpu,clocks_event_reasons.active,clocks_event_reasons.sw_thermal_slowdown,clocks_event_reasons.sw_power_cap','--format=csv,noheader'],capture_output=True,text=True)
        samples.append({'utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'gpu':r.stdout.strip(),'exit_code':r.returncode})
        stop.wait(5)
t=threading.Thread(target=monitor);t.start()
command=[sys.executable,str(repo/'tools/agent-skills/peengine-perf/scripts/run_job.py'),'--cwd',str(runtime),'--state',str(out/'wait-profile.state.json'),'--timeout','600','--fingerprint',str(exe),'--fingerprint',str(ini),'--unset-env','PERF_JOLT','--unset-env','PERF_CONTROLS','--unset-env','PERF_SOAK','--unset-env','DEBT_DETAIL','--unset-env','DEBT_RELAXED','--',str(exe),'-Loading.BenchmarkWarmupFrames=120','-Loading.BenchmarkFrames=1801','-Loading.BenchmarkOutput='+str(out/'wait-profile.json')]
try:subprocess.run(command,check=True)
finally:
    stop.set();t.join();meta['after']=hashes();(out/'manifest.json').write_text(json.dumps(meta,indent=2));(out/'sensors.json').write_text(json.dumps(samples,indent=2))
assert meta['before']==meta['after'],'Inputs changed'
d=json.loads((out/'wait-profile.json').read_text());assert d['sampled_frames']==len(d['frame_samples_ms'])==1800
assert d['physics_count_scale']==1 and d['character_seed']==12345
assert abs(d['physics_requested_seconds']-d['physics_simulated_seconds']-d['physics_debt_end']+d['physics_debt_start'])<.002
print(json.dumps({k:d[k] for k in ['frame_ms','physics_ticks','physics_requested_seconds','physics_simulated_seconds','physics_debt_start','physics_debt_end']}),flush=True)
