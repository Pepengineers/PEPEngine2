from pathlib import Path
import subprocess,os,re
out=Path(__file__).resolve().parent;repo=out.parents[1];runtime=repo/'Build/bin/x64/Debug/App'
source=repo/'Apps/App.Base/src/App.cpp';text=source.read_text(encoding='utf-8-sig')
old='dynamic.Schedule(_jobs, world, step).Complete();';assert text.count(old)==1
target=out/'ProbeAppBase.cpp';target.write_text('#include "SyncProbe.h"\n'+text.replace(old,'SyncProbe::Run(dynamic,_jobs,world,step);'),encoding='utf-8')
lines=(repo/'Build/obj/x64/Debug/App.Base/App.Base.tlog/CL.command.1.tlog').read_text(encoding='utf-16').splitlines();key='^'+str(source).upper();assert lines.count(key)==1
command=lines[lines.index(key)+1]
for flag,ext in [('Fo','.obj'),('Fd','.pdb')]:command=re.sub('/'+flag+r'"[^"\n]*"','/'+flag+'"'+(out/('ProbeAppBase'+ext)).as_posix()+'"',command)
command,n=re.subn(r'\s+D:\\PROJECT\\PEPENGINE2\\[^\r\n]+\.CPP$',' "'+target.as_posix()+'" /I"'+source.parent.as_posix()+'"',command);assert n==1
(out/'probe-compile.rsp').write_text(command)
link='\n'.join((repo/'Build/obj/x64/Debug/App/App.tlog/link.command.1.tlog').read_text(encoding='utf-16').splitlines()[1:])
for flag,ext in [('OUT','.exe'),('PDB','.pdb'),('IMPLIB','.lib'),('ILK','.ilk')]:link=re.sub('/'+flag+r':"[^"\n]+"','/'+flag+':"'+(runtime/('CpuSyncProbe'+ext)).as_posix()+'"',link)
link+='\n"'+(out/'ProbeAppBase.obj').as_posix()+'"'
(out/'probe-link.rsp').write_text(link)
dev=r'C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\Tools\VsDevCmd.bat'
r=subprocess.check_output(f'cmd.exe /d /s /c "call "{dev}" -no_logo -arch=x64 >nul && set"',text=True);env=os.environ.copy()
for line in r.splitlines():
 if '=' in line and not line.startswith('='):k,v=line.split('=',1);env[k]=v
cl=Path(env['VCToolsInstallDir'])/'bin/Hostx64/x64/cl.exe'
with (out/'probe-build.log').open('w') as log:
 subprocess.run([str(cl),'@'+str(out/'probe-compile.rsp')],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
 subprocess.run([str(cl.with_name('link.exe')),'@'+str(out/'probe-link.rsp')],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
print('Built matched moving-state sync probe')
