from pathlib import Path
import subprocess,re,os,json
out=Path(__file__).resolve().parent;repo=out.parents[1];runtime=repo/'Build/bin/x64/Debug/App'
def prepare(name,project,path,text):
 target=out/(name+'.cpp');target.write_text(text,encoding='utf-8')
 lines=next((repo/'Build/obj/x64/Debug'/project).glob('*.tlog/CL.command.1.tlog')).read_text(encoding='utf-16').splitlines();key='^'+str(repo/path).upper();assert lines.count(key)==1
 command=lines[lines.index(key)+1]
 for flag,ext in [('Fo','.obj'),('Fd','.pdb')]:command=re.sub('/'+flag+r'"[^"\n]*"','/'+flag+'"'+(out/(name+ext)).as_posix()+'"',command)
 command,n=re.subn(r'\s+D:\\PROJECT\\PEPENGINE2\\[^\r\n]+\.CPP$',' "'+target.as_posix()+'" /I"'+(repo/path).parent.as_posix()+'"',command);assert n==1
 (out/(name+'.rsp')).write_text(command)
app=(repo/'Apps/App/src/App.cpp').read_text(encoding='utf-8-sig')
for call in ['MessageBoxW(nullptr, error.ToString().c_str(), L"Bistro App", MB_OK | MB_ICONERROR);','MessageBoxA(nullptr, error.what(), "Bistro App", MB_OK | MB_ICONERROR);']:
 assert app.count(call)==1;app=app.replace(call,'/* Diagnostic: stderr/log already contains the failure. */')
prepare('MatchedApp','App','Apps/App/src/App.cpp','#include "AssertCapture.h"\n'+app)
base=(repo/'Apps/App.Base/src/App.cpp').read_text(encoding='utf-8-sig');old='dynamic.Schedule(_jobs, world, step).Complete();';assert base.count(old)==1
for batch in [64,256]:
 prepare('MatchedBase'+str(batch),'App.Base','Apps/App.Base/src/App.cpp',base.replace(old,'dynamic.Schedule(_jobs, world, step, '+str(batch)+').Complete();'))
link='\n'.join((repo/'Build/obj/x64/Debug/App/App.tlog/link.command.1.tlog').read_text(encoding='utf-16').splitlines()[1:])
original=str(repo/'Build/obj/x64/Debug/App/App.obj').upper();assert link.count(original)==1;link=link.replace(original,'"'+(out/'MatchedApp.obj').as_posix()+'"')
for batch in [64,256]:
 command=link
 for flag,ext in [('OUT','.exe'),('PDB','.pdb'),('IMPLIB','.lib'),('ILK','.ilk')]:command=re.sub('/'+flag+r':"[^"\n]+"','/'+flag+':"'+(runtime/('CpuMatched'+str(batch)+ext)).as_posix()+'"',command)
 (out/('matched'+str(batch)+'-link.rsp')).write_text(command+'\n"'+(out/('MatchedBase'+str(batch)+'.obj')).as_posix()+'"')
dev=r'C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\Tools\VsDevCmd.bat'
r=subprocess.check_output(f'cmd.exe /d /s /c "call "{dev}" -no_logo -arch=x64 >nul && set"',text=True);env=os.environ.copy()
for line in r.splitlines():
 if '=' in line and not line.startswith('='):k,v=line.split('=',1);env[k]=v
cl=Path(env['VCToolsInstallDir'])/'bin/Hostx64/x64/cl.exe'
with (out/'matched-build.log').open('w') as log:
 for name in ['MatchedApp','MatchedBase64','MatchedBase256']:
  print('Compiling',name,flush=True);subprocess.run([str(cl),'@'+str(out/(name+'.rsp'))],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
 for batch in [64,256]:subprocess.run([str(cl.with_name('link.exe')),'@'+str(out/('matched'+str(batch)+'-link.rsp'))],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
print('Built matched 64/256 binaries; diagnostic failures exit instead of opening modal dialogs')
