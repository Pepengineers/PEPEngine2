from pathlib import Path
import json,os,subprocess,sys,tempfile,time

runner=Path(__file__).with_name('run_job.py')
with tempfile.TemporaryDirectory(prefix='perf-runner-check-') as directory:
    root=Path(directory)
    def run(name, code, extra=(), timeout=10):
        state=root/(name+'.json')
        command=[sys.executable,str(runner),'--cwd',str(root),'--state',str(state),'--timeout',str(timeout),*extra,'--',sys.executable,'-c',code]
        result=subprocess.run(command,capture_output=True,text=True)
        return result,json.loads(state.read_text()),command

    os.environ['PERF_RUNNER_TEST_FLAG']='inherited'
    result,state,command=run('env',"import os; print(repr(os.environ.get('PERF_RUNNER_TEST_FLAG'))); print(os.environ['PERF_RUNNER_TEST_VALUE'])",['--unset-env','PERF_RUNNER_TEST_FLAG','--set-env','PERF_RUNNER_TEST_VALUE=spaces and = sign'])
    assert result.returncode==0 and state['status']=='passed'
    assert Path(state['stdout']).read_text().splitlines()==['None','spaces and = sign']
    assert len(result.stdout.splitlines())==2
    assert subprocess.run(command,capture_output=True).returncode!=0
    result,state,_=run('exit',"raise SystemExit(7)")
    assert result.returncode!=0 and state['status']=='failed' and state['child_exit_code']==7
    protected=root/'input.txt';protected.write_text('before')
    result,state,_=run('changed',"from pathlib import Path; Path('input.txt').write_text('after')",['--fingerprint',str(protected)])
    assert result.returncode!=0 and state['status']=='censored' and len(state['changed_inputs'])==1
    code="import subprocess,sys,time; subprocess.Popen([sys.executable,'-c',\"import time; from pathlib import Path; time.sleep(3); Path('orphan-marker').write_text('bad')\"]); time.sleep(20)"
    result,state,_=run('timeout',code,timeout=1)
    assert result.returncode!=0 and state['status']=='timeout'
    time.sleep(3.2)
    assert not (root/'orphan-marker').exists(),'Timed-out descendant survived'
print('PASS: environment deletion, compact logs, run-name protection, exit failure, changed-input censoring, owned-tree timeout')
