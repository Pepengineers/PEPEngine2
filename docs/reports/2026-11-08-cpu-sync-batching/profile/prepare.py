from pathlib import Path
import hashlib,json,re,subprocess,os
out=Path(__file__).resolve().parent;repo=out.parents[1]
sources={}
def read(p):return (repo/p).read_text(encoding='utf-8-sig')
def replace(s,a,b):
    assert s.count(a)==1,(a,s.count(a))
    return s.replace(a,b)
def wrap(s,statement,metric):return replace(s,statement,'{FrameProbe::Scope probe(FrameProbe::'+metric+');'+statement+'}')
app=read('Apps/App/src/App.cpp')
app=replace(app,'            Update(Timer);\n            Render(Timer);', '            FrameProbe::Begin(BenchmarkTicks);\n            const double frameStart=FrameProbe::Now();\n            {FrameProbe::Scope probe(FrameProbe::UpdateTotal);Update(Timer);}\n            {FrameProbe::Scope probe(FrameProbe::Render);Render(Timer);}\n            FrameProbe::Times[FrameProbe::FrameTotal]=FrameProbe::Now()-frameStart;\n            FrameProbe::Dump(Timer.DeltaTime(),Locator.GetModule<RenderModule>()->GetGPUFrameTimeMS(),BenchmarkTicks,BenchmarkRequested);')
for statement,metric in [('UpdatePhysics(world, dt);','Physics'),('work = _gpuData.Schedule(_jobs, world, dt);\n                work.Complete();','ECSGPU'),('UpdateSceneAdditions(world, timer.DeltaTime());','SceneAdditions')]:app=wrap(app,statement,metric)
app=replace(app,'                work = _movement.Schedule','                {FrameProbe::Scope probe(FrameProbe::Movement);\n                work = _movement.Schedule')
app=replace(app,'                work.Complete();\n                {FrameProbe::Scope probe(FrameProbe::Physics);','                work.Complete();}\n                {FrameProbe::Scope probe(FrameProbe::Physics);')
sources['App']=('App','Apps/App/src/App.cpp',app)
render=read('Apps/App.Base/src/RenderModule.cpp')
for signature,metric in [('void RenderModule::OnUpdate()','ModuleUpdate'),('void RenderModule::CollectReflectionScene()','Snapshot'),('void RenderModule::BeginFrame()','BeginFrame')]:render=replace(render,signature+'\n{',signature+'\n{\n    FrameProbe::Scope probe(FrameProbe::'+metric+');')
render=replace(render,'    auto& frameConsts = _primaryResources.FrameConstants[_primaryResources.CurrFrameConstantsIndex];','    auto& frameConsts = _primaryResources.FrameConstants[_primaryResources.CurrFrameConstantsIndex];\n    FrameProbe::SlotTarget=frameConsts->FenceValue;FrameProbe::SlotCompleted=cmdQueue->GetFence()->GetCompletedValue();FrameProbe::SlotIndex=_primaryResources.CurrFrameConstantsIndex;')
render=render.replace('cmdQueue->CPUWaitForFenceValue(frameConsts->FenceValue);','{FrameProbe::Site site(FrameProbe::SlotWait);cmdQueue->CPUWaitForFenceValue(frameConsts->FenceValue);}')
for statement,metric in [('_primaryResources.UpdateMainCB(width, height, _timer);','MainCB'),('PrepareSkeletalMeshes();','Skeletal'),('if (_uiInitialized) Engine::UI::BeginFrame();','UIBegin'),('if (_backBuffer) _backBuffer->BeginFrame(_RPcommonData);','BackBufferBegin'),('const bool rebuild = _pipeline->PrepareFrame(context, primarycmdList);','PipelinePrep')]:
    if metric=='PipelinePrep':render=replace(render,statement,'bool rebuild;{FrameProbe::Scope p(FrameProbe::PipelinePrep);rebuild=_pipeline->PrepareFrame(context,primarycmdList);}')
    else:render=wrap(render,statement,metric)
render=replace(render,'    primarycmdList = _primaryGraph->Execute(primarycmdList,\n        !_dualGPUMode && _pipeline->GetBindings().UseAsyncCompute ? _primaryDevice->TryGetComputeQueue() : nullptr);','    {FrameProbe::Scope p(FrameProbe::GraphRecord);primarycmdList = _primaryGraph->Execute(primarycmdList,\n        !_dualGPUMode && _pipeline->GetBindings().UseAsyncCompute ? _primaryDevice->TryGetComputeQueue() : nullptr);}')
sources['Render']=('App.Base','Apps/App.Base/src/RenderModule.cpp',render)
queue=read('Engine/Engine.RendererDX12/src/GDX12CommandQueue.cpp')
queue=replace(queue,'    HANDLE event = CreateEventEx','    FrameProbe::Scope all(FrameProbe::WaitTotal);FrameProbe::Scope site(FrameProbe::WaitSite);++FrameProbe::WaitCalls;\n    HANDLE event = CreateEventEx')
queue=replace(queue,'    GDX12_SCOPED_CPU_EVENT("Submit");','    FrameProbe::Scope probe(FrameProbe::Submit);\n    GDX12_SCOPED_CPU_EVENT("Submit");')
queue=replace(queue,'GDX12CommandList* GDX12CommandQueue::GetCommandList()\n{','GDX12CommandList* GDX12CommandQueue::GetCommandList()\n{\n    FrameProbe::Scope probe(FrameProbe::Acquire);')
sources['Queue']=('Engine.RendererDX12','Engine/Engine.RendererDX12/src/GDX12CommandQueue.cpp',queue)
swap=read('Engine/Engine.RendererDX12/src/GDX12SwapChain.cpp')
swap=replace(swap,'void GDX12SwapChain::Present()\n{','void GDX12SwapChain::Present()\n{\n    FrameProbe::Scope probe(FrameProbe::Present);')
sources['Swap']=('Engine.RendererDX12','Engine/Engine.RendererDX12/src/GDX12SwapChain.cpp',swap)
timer=read('Engine/Engine.RendererDX12/src/GDX12GPUTimer.cpp')
timer=replace(timer,'        _commandQueue->CPUWaitForFenceValue(_frameFenceValues[_activeFrameIndex]);','        {FrameProbe::Site site(FrameProbe::TimerWait);_commandQueue->CPUWaitForFenceValue(_frameFenceValues[_activeFrameIndex]);}')
sources['Timer']=('Engine.RendererDX12','Engine/Engine.RendererDX12/src/GDX12GPUTimer.cpp',timer)
base=read('Apps/App.Base/src/App.cpp')
base=wrap(base,'physics->Step(world, step);','Backend')
base=wrap(base,'dynamic.Schedule(_jobs, world, step).Complete();','DynamicSync')
sources['AppBase']=('App.Base','Apps/App.Base/src/App.cpp',base)
link='\n'.join((repo/'Build/obj/x64/Debug/App/App.tlog/link.command.1.tlog').read_text(encoding='utf-16').splitlines()[1:])
runtime=repo/'Build/bin/x64/Debug/App'
for flag,ext in [('OUT','.exe'),('PDB','.pdb'),('IMPLIB','.lib'),('ILK','.ilk')]:link=re.sub('/'+flag+r':"[^"\n]+"','/'+flag+':"'+(runtime/('CpuSyncProfile'+ext)).as_posix()+'"',link)
for name,(project,path,source) in sources.items():
    target=out/(name+'.cpp');target.write_text('#include "FrameProbe.h"\n'+source,encoding='utf-8')
    lines=next((repo/'Build/obj/x64/Debug'/project).glob('*.tlog/CL.command.1.tlog')).read_text(encoding='utf-16').splitlines();key='^'+str(repo/path).upper();assert lines.count(key)==1
    command=lines[lines.index(key)+1]
    for flag,ext in [('Fo','.obj'),('Fd','.pdb')]:command=re.sub('/'+flag+r'"[^"\n]*"','/'+flag+'"'+(out/(name+ext)).as_posix()+'"',command)
    command,n=re.subn(r'\s+D:\\PROJECT\\PEPENGINE2\\[^\r\n]+\.CPP$', ' "'+target.as_posix()+'" /I"'+(repo/path).parent.as_posix()+'"',command);assert n==1
    (out/(name+'.rsp')).write_text(command);obj=(out/(name+'.obj')).as_posix()
    if project=='App':
        original=str(repo/'Build/obj/x64/Debug/App/App.obj').upper();assert link.count(original)==1;link=link.replace(original,'"'+obj+'"')
    else:link+='\n"'+obj+'"'
(out/'link.rsp').write_text(link)
(out/'source-hashes.json').write_text(json.dumps({p:hashlib.sha256((repo/p).read_bytes()).hexdigest() for _,p,_ in sources.values()},indent=2))
dev=r'C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\Tools\VsDevCmd.bat'
result=subprocess.check_output(f'cmd.exe /d /s /c "call "{dev}" -no_logo -arch=x64 >nul && set"',text=True);env=os.environ.copy()
for line in result.splitlines():
    if '=' in line and not line.startswith('='):k,v=line.split('=',1);env[k]=v
compiler=Path(env['VCToolsInstallDir'])/'bin/Hostx64/x64/cl.exe'
with (out/'build.log').open('w') as log:
    for name in sources:
        print('Compiling',name,flush=True);subprocess.run([str(compiler),'@'+str(out/(name+'.rsp'))],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
    subprocess.run([str(compiler.with_name('link.exe')),'@'+str(out/'link.rsp')],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
print('Built current-source wait-attribution diagnostic',flush=True)
