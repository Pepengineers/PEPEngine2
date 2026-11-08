from pathlib import Path
import hashlib,json,re,subprocess,os
out=Path(__file__).resolve().parent
repo=out.parents[1]
runtime=repo/'Build/bin/x64/Debug/App'
sources={}
def read(p): return (repo/p).read_text(encoding='utf-8-sig')
def replace(s,a,b):
    assert s.count(a)==1,(a,s.count(a))
    return s.replace(a,b)
def scope(s,statement,name): return replace(s,statement,'{ Debt::Scope scope(Debt::'+name+'); '+statement+' }')
app=read('Apps/App/src/App.cpp')
app=replace(app,'#include <App.Base/App.h>','#include <App.Base/App.h>\n#include "App.Base/Systems/DynamicBodyUpdateSystem.h"\n#include "JoltDebt.h"')
app=replace(app,'            Render(Timer);','            Render(Timer);\n            Debt::FlushFrame(Timer.DeltaTime(),Locator.GetModule<RenderModule>()->GetGPUFrameTimeMS());')
app=replace(app,'        void ResetBenchmarkClock()',r'''        void ProbeSync()
        {
            if(!Debt::SyncProbe) return;
            auto& world=*Locator.GetModule<SceneManagerModule>()->GetWorld();
            auto physics=Locator.GetModule<PhysicsModule>();
            DynamicBodyUpdateSystem dynamic(*physics);
            std::ofstream f("D:/Project/PEPEngine2/Build/PhysicsDebt-20261108/sync-probe.csv");
            f<<"iteration,scheduled_ms,direct_ms,batch16_ms,batch256_ms,batch1024_ms,ecs_query_ms,poses_ms,body_count,pose_lock_ms,pose_inner_ms\n";
            auto checksum=[&]{double result=0;world.GetECS().ForEach<RigidBodyComponent,TransformComponent>([&](Entity,RigidBodyComponent&,TransformComponent& t){result+=t.Location.x+t.Location.y+t.Location.z+t.Rotation.x+t.Rotation.y+t.Rotation.z;});return result;};
            const double original=checksum();
            for(unsigned i=0;i<220;++i){double scheduled=0,direct=0,b16=0,b256=0,b1024=0,query=0,poses=0;unsigned count=0;
                auto measure=[&](auto&& work){double t=Debt::Now();work();return Debt::Now()-t;};
                auto a=[&]{scheduled=measure([&]{dynamic.Schedule(_jobs,world,1.f/60).Complete();});};
                auto b=[&]{direct=measure([&]{dynamic.Tick(world,1.f/60);});};
                if(i%2){a();b();}else{b();a();}
                auto c=[&]{b16=measure([&]{dynamic.Schedule(_jobs,world,1.f/60,16).Complete();});};
                auto d=[&]{b256=measure([&]{dynamic.Schedule(_jobs,world,1.f/60,256).Complete();});};
                auto e=[&]{b1024=measure([&]{dynamic.Schedule(_jobs,world,1.f/60,1024).Complete();});};
                if(i%2){c();d();e();}else{e();d();c();}
                query=measure([&]{world.GetECS().ForEach<RigidBodyComponent,TransformComponent>([&](Entity,RigidBodyComponent& body,TransformComponent&){if(body.MotionType==PhysicsMotionType::Dynamic&&body.Body.IsValid())++count;});});
                Debt::PoseLock=Debt::PoseTotal=0;Debt::PoseProbe=true;
                poses=measure([&]{for(auto& body:world.GetECS().GetPool<RigidBodyComponent>().GetDense()){if(body.MotionType!=PhysicsMotionType::Dynamic||!body.Body.IsValid())continue;Vector3 p;DirectX::SimpleMath::Quaternion q;if(!physics->TryGetBodyTransform(world,body.Body,p,q))throw std::runtime_error("Probe lost body");}});
                Debt::PoseProbe=false;
                if(checksum()!=original)throw std::runtime_error("Sync probe changed state");
                if(i>=20)f<<i<<','<<scheduled<<','<<direct<<','<<b16<<','<<b256<<','<<b1024<<','<<query<<','<<poses<<','<<count<<','<<Debt::PoseLock<<','<<Debt::PoseTotal<<'\n';
            }
        }
        void ResetBenchmarkClock()''')
app=replace(app,'            return EXIT_SUCCESS;','            app.ProbeSync();\n            Debt::DumpJolt();\n            return EXIT_SUCCESS;')
app=scope(app,'UpdatePhysics(world, dt);','Physics')
sources['App']=('App','Apps/App/src/App.cpp',app)
base=read('Apps/App.Base/src/App.cpp')
base=replace(base,'    world._physicsAccumulator += elapsed;','    world._physicsAccumulator += elapsed;\n    Debt::Requested=elapsed;Debt::Actual=world._physicsAccumulator;')
base=replace(base,'        if (!_AppConfig.StartScene.empty())\n            scenes->UpdateSystems(_jobs, world, step, SystemUpdatePhase::FixedPrePhysics);','        Debt::BeginStep();\n        if (!_AppConfig.StartScene.empty())\n            scenes->UpdateSystems(_jobs, world, step, SystemUpdatePhase::FixedPrePhysics);')
for statement,name in [('movement.Schedule(_jobs, world, step).Complete();','Pre'),('spline.Schedule(_jobs, world, step).Complete();','Pre'),('kinematic.Schedule(_jobs, world, step).Complete();','Pre'),('if (world.BeforePhysicsStep) world.BeforePhysicsStep(step);','Callback'),('physics->Step(world, step);','Backend'),('dynamic.Schedule(_jobs, world, step).Complete();','Post')]:base=scope(base,statement,name)
base=replace(base,'        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() >= step) break;', '        const bool limit=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count() >= step*(Debt::Relaxed?2:1);\n        Debt::EndStep(world._physicsAccumulator,limit);\n        if(limit)break;')
sources['AppBase']=('App.Base','Apps/App.Base/src/App.cpp',base)
backend=read('Apps/App.Base/src/Physics/JoltPhysicsBackend.cpp')
backend=replace(backend,'    const auto error = _physicsSystem->Update','    Debt::Active=_physicsSystem->GetNumActiveBodies(JPH::EBodyType::RigidBody);\n    Debt::Bodies=_physicsSystem->GetNumBodies();\n    Debt::BeginBackend();\n    const auto error = _physicsSystem->Update')
backend=replace(backend,'    if (error != JPH::EPhysicsUpdateError::None)', '    Debt::EndBackend();\n    if (error != JPH::EPhysicsUpdateError::None)')
start=backend.index('bool JoltPhysicsBackend::TryGetBodyTransform(')
end=backend.index('void JoltPhysicsBackend::MoveKinematicBody(',start)
pose=backend[start:end]
pose=replace(pose,'    const JPH::BodyID bodID(handle.Value);','    Debt::Scope total(Debt::PoseTotal,Debt::PoseProbe);\n    const double lockStart=Debt::PoseProbe?Debt::Now():0;\n    const JPH::BodyID bodID(handle.Value);')
pose=replace(pose,'    if (!lock.Succeeded())', '    if(Debt::PoseProbe)Debt::PoseLock+=Debt::Now()-lockStart;\n    if (!lock.Succeeded())')
backend=backend[:start]+pose+backend[end:]
sources['Backend']=('App.Base','Apps/App.Base/src/Physics/JoltPhysicsBackend.cpp',backend)
jobs=read('Apps/App.Base/src/Physics/JoltJobSystem.cpp')
jobs=replace(jobs,'    auto* job = new ScheduledJob(*this, name, color, function, dependencies);','    Debt::Scope measured(Debt::Local().create,Debt::Detail);++Debt::Local().creates;\n    auto wrapped=[function]{Debt::ThreadProfile();function();};\n    auto* job = new ScheduledJob(*this, name, color, wrapped, dependencies);')
jobs=replace(jobs,'    job->AddRef();','    Debt::Scope measured(Debt::Local().queue,Debt::Detail);++Debt::Local().queues;\n    job->AddRef();')
jobs=replace(jobs,'void JoltJobSystem::FreeJob(Job* job) { delete static_cast<ScheduledJob*>(job); }','void JoltJobSystem::FreeJob(Job* job) { Debt::Scope measured(Debt::Local().free,Debt::Detail);delete static_cast<ScheduledJob*>(job); }')
jobs=replace(jobs,'    while (_outstanding.load','    Debt::Scope measured(Debt::Local().drain,Debt::Detail);\n    while (_outstanding.load')
jobs=replace(jobs,'    JobSystemWithBarrier::WaitForJobs(barrier);','    {Debt::Scope measured(Debt::Local().barrier,Debt::Detail);JobSystemWithBarrier::WaitForJobs(barrier);}')
sources['Jobs']=('App.Base','Apps/App.Base/src/Physics/JoltJobSystem.cpp',jobs)
link='\n'.join((repo/'Build/obj/x64/Debug/App/App.tlog/link.command.1.tlog').read_text(encoding='utf-16').splitlines()[1:])
for flag,extension in [('OUT','.exe'),('PDB','.pdb'),('IMPLIB','.lib'),('ILK','.ilk')]:link=re.sub('/'+flag+r':"[^"\n]+"','/'+flag+':"'+(runtime/('PhysicsDebtDiag'+extension)).as_posix()+'"',link)
for name,(project,path,source) in sources.items():
    target=out/(name+'.cpp');target.write_text('#include "'+('JoltDebt.h' if name in ['Backend','Jobs'] else 'Debt.h')+'"\n'+source,encoding='utf-8')
    lines=next((repo/'Build/obj/x64/Debug'/project).glob('*.tlog/CL.command.1.tlog')).read_text(encoding='utf-16').splitlines()
    key='^'+str(repo/path).upper();assert lines.count(key)==1
    command=lines[lines.index(key)+1]
    for flag,ext in [('Fo','.obj'),('Fd','.pdb')]: command=re.sub('/'+flag+r'"[^"\n]*"','/'+flag+'"'+(out/(name+ext)).as_posix()+'"',command)
    command,n=re.subn(r'\s+D:\\PROJECT\\PEPENGINE2\\[^\r\n]+\.CPP$', ' "'+target.as_posix()+'" /I"'+(repo/path).parent.as_posix()+'"',command);assert n==1
    (out/(name+'.rsp')).write_text(command)
    obj=(out/(name+'.obj')).as_posix()
    if project=='App':
        original=str(repo/'Build/obj/x64/Debug/App/App.obj').upper();assert link.count(original)==1
        link=link.replace(original,'"'+obj+'"')
    else:link+='\n"'+obj+'"'
(out/'link.rsp').write_text(link)
(out/'sources.json').write_text(json.dumps(list(sources)))
(out/'source-hashes.json').write_text(json.dumps({p:hashlib.sha256((repo/p).read_bytes()).hexdigest() for _,p,_ in sources.values()},indent=2))
dev=r'C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\Tools\VsDevCmd.bat'
result=subprocess.check_output(f'cmd.exe /d /s /c "call "{dev}" -no_logo -arch=x64 >nul && set"',text=True)
env=os.environ.copy()
for line in result.splitlines():
    if '=' in line and not line.startswith('='):
        k,v=line.split('=',1);env[k]=v
compiler=Path(env['VCToolsInstallDir'])/'bin/Hostx64/x64/cl.exe'
with (out/'build.log').open('w') as log:
    for name in sources:
        print('Compiling',name,flush=True)
        subprocess.run([str(compiler),'@'+str(out/(name+'.rsp'))],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
    subprocess.run([str(compiler.with_name('link.exe')),'@'+str(out/'link.rsp')],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
print('Built current-scene Debug diagnostic without changing production sources',flush=True)
