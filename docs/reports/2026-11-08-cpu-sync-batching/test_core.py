from pathlib import Path
import os,subprocess
repo=Path.cwd(); out=repo/'Build/CpuSyncOptimize-20261108'
dev=r'C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\Tools\VsDevCmd.bat'
s=subprocess.check_output(f'cmd.exe /d /s /c "call "{dev}" -no_logo -arch=x64 >nul && set"',text=True)
env=os.environ.copy()
for line in s.splitlines():
 if '=' in line and not line.startswith('='):
  k,v=line.split('=',1);env[k]=v
cl=Path(env['VCToolsInstallDir'])/'bin/Hostx64/x64/cl.exe'
with (out/'core-tests-build.log').open('w') as log:
 for test in ['ECSJobTests','JobSystemTests']:
  args=[str(cl),'/nologo','/std:c++17','/EHsc','/Od','/RTC1','/MDd','/Z7','/D_DEBUG','/I'+str(repo/'Engine/Engine.Core/Include'),'/I'+str(repo/'External/External.Libraries/enkiTS'),str(repo/f'Engine/Engine.Core/tests/{test}.cpp'),str(repo/'Engine/Engine.Core/src/Jobs/JobSystem.cpp'),str(repo/'External/External.Libraries/enkiTS/TaskScheduler.cpp'),'/Fe:'+str(out/(test+'.exe')),'/Fo:'+str(out)+'\\']
  subprocess.run(args,cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
  subprocess.run([str(out/(test+'.exe'))],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
print('ECSJobTests and JobSystemTests passed')
