from pathlib import Path
import hashlib, json, os, shutil, statistics, subprocess, sys, time, threading, ctypes

out = Path(__file__).resolve().parent
repo = out.parents[1]
runtime = repo / 'Build/bin/x64/Debug/App'
name, tag = sys.argv[1:3]
frames = int(sys.argv[3]) if len(sys.argv) > 3 else 1200
mode = sys.argv[4] if len(sys.argv) > 4 else 'ordinary'
assert mode in ['ordinary','coarse','detail','relaxed']
exe = runtime / (name + '.exe')
ini = runtime / 'Config/Engine.ini'
inputs = [exe, ini, *sorted((repo/'Apps/App/src').rglob('*.h')),
          repo/'Apps/App/src/App.cpp', repo/'Apps/App.Base/src/App.cpp',
          repo/'Apps/App.Base/src/Physics/JoltPhysicsBackend.cpp',
          repo/'Apps/App.Base/src/Physics/JoltJobSystem.cpp',
          repo/'Apps/App.Base/Include/App.Base/Systems/DynamicBodyUpdateSystem.h']
def hashes():
    return {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
pdh=ctypes.WinDLL('pdh')
query=ctypes.c_void_p()
pdh.PdhOpenQueryW.argtypes=[ctypes.c_wchar_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_void_p)]
pdh.PdhAddEnglishCounterW.argtypes=[ctypes.c_void_p,ctypes.c_wchar_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_void_p)]
pdh.PdhCollectQueryData.argtypes=[ctypes.c_void_p]
class CounterValue(ctypes.Structure):
    _fields_=[('status',ctypes.c_ulong),('value',ctypes.c_double)]
pdh.PdhGetFormattedCounterValue.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_void_p,ctypes.POINTER(CounterValue)]
counters={}
if pdh.PdhOpenQueryW(None,0,ctypes.byref(query))==0:
    for label,path in [('cpu_performance_percent',r'\Processor Information(_Total)\% Processor Performance'),('cpu_utility_percent',r'\Processor Information(_Total)\% Processor Utility')]:
        handle=ctypes.c_void_p()
        if pdh.PdhAddEnglishCounterW(query,path,0,ctypes.byref(handle))==0:counters[label]=handle
def cpu_telemetry():
    values={}
    if query:
        pdh.PdhCollectQueryData(query)
        for label,handle in counters.items():
            v=CounterValue()
            rc=pdh.PdhGetFormattedCounterValue(handle,0x200,None,ctypes.byref(v))
            values[label]=v.value if rc==0 and v.status in [0,1] else None
    status=(ctypes.c_ubyte*12)()
    values['ac_line_status']=status[0] if ctypes.windll.kernel32.GetSystemPowerStatus(status) else None
    return values
def telemetry():
    r = subprocess.run(['nvidia-smi','--query-gpu=timestamp,temperature.gpu,clocks.gr,power.draw,utilization.gpu,memory.used,clocks_event_reasons.active,clocks_event_reasons.sw_thermal_slowdown,clocks_event_reasons.hw_thermal_slowdown,clocks_event_reasons.sw_power_cap','--format=csv,noheader'], capture_output=True, text=True)
    return {'gpu':r.stdout.strip(), 'gpu_returncode':r.returncode, **cpu_telemetry(),
            'utc':time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}
before = hashes()
meta = {'before':before, 'telemetry_before':telemetry(), 'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip()}
samples=[]
stop=threading.Event()
def sample():
    while not stop.is_set():
        samples.append(telemetry())
        stop.wait(5)
monitor=threading.Thread(target=sample)
monitor.start()
command = [sys.executable, str(repo/'tools/agent-skills/peengine-perf/scripts/run_job.py'),
           '--cwd',str(runtime),'--state',str(out/(tag+'.state.json')),'--timeout','600',
           '--fingerprint',str(exe),'--fingerprint',str(ini),
           '--unset-env','PERF_JOLT','--unset-env','PERF_CONTROLS','--unset-env','PERF_SOAK',
           '--set-env','DEBT_SYNC_PROBE=1',
           *(['--set-env','DEBT_DETAIL=1'] if mode=='detail' else ['--unset-env','DEBT_DETAIL']),
           *(['--set-env','DEBT_RELAXED=1'] if mode=='relaxed' else ['--unset-env','DEBT_RELAXED']),
           '--',str(exe),'-Loading.BenchmarkWarmupFrames=120',
           '-Loading.BenchmarkFrames='+str(frames+1),'-Loading.BenchmarkOutput='+str(out/(tag+'.json'))]
try:
    subprocess.run(command, check=True)
finally:
    stop.set()
    monitor.join()
    (out/(tag+'.sensors.json')).write_text(json.dumps(samples,indent=2))
    meta['after'] = hashes()
    meta['telemetry_after'] = telemetry()
    (out/(tag+'.meta.json')).write_text(json.dumps(meta,indent=2))
assert before == meta['after'], 'Inputs changed during run'
if mode != 'ordinary':
    for filename in ['frames.csv','steps.csv','jobs.csv','jolt-events.csv','sync-probe.csv','clock.json']:
        shutil.copyfile(out/filename,out/(tag+'-'+filename))
d = json.loads((out/(tag+'.json')).read_text())
s = d['frame_samples_ms']
assert len(s) == frames == d['sampled_frames']
assert d['physics_count_scale'] == 1 and d['character_seed'] == 12345
assert abs(d['physics_requested_seconds']-d['physics_simulated_seconds']-d['physics_debt_end']+d['physics_debt_start']) < .002
print(json.dumps({k:d[k] for k in ['frame_ms','physics_requested_seconds','physics_ticks','physics_simulated_seconds','physics_debt_start','physics_debt_end']}),flush=True)
print('fps',1000/d['frame_ms'],'p95',sorted(s)[int(.95*(len(s)-1))],flush=True)
