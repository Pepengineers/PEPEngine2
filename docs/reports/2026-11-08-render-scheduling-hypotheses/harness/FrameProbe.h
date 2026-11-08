#pragma once
#include <chrono>
#include <array>
#include <fstream>
#include <iomanip>
#include <cstdint>
namespace FrameProbe {
enum Metric { FrameTotal,UpdateTotal,BeginFrame,UIBegin,BackBufferBegin,ModuleUpdate,SlotWait,OtherWait,TimerWait,MainCB,Skeletal,Snapshot,PipelinePrep,GraphRecord,Submit,Present,Physics,ECSGPU,SceneAdditions,Movement,Render,WaitTotal,Acquire,Count };
inline const char* Names[]={"frame","update","begin_frame","ui_begin","backbuffer_begin","module_update","slot_wait","other_wait","timer_wait","main_cb","skeletal","snapshot","pipeline_prepare","graph_record","submit","present","physics","ecs_gpu","scene_additions","movement","render","wait_total","command_acquire"};
inline std::array<double,Count> Times{};
inline thread_local Metric WaitSite=OtherWait;
inline unsigned Frame=0, WaitCalls=0;
inline uint64_t StartTicks=0, SlotTarget=0, SlotCompleted=0;
inline unsigned SlotIndex=0;
inline double Now(){return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();}
struct Scope{Metric Id;double Start=Now();explicit Scope(Metric id):Id(id){}~Scope(){Times[Id]+=Now()-Start;}};
struct Site{Metric Previous=WaitSite;explicit Site(Metric id){WaitSite=id;}~Site(){WaitSite=Previous;}};
inline void Begin(uint64_t ticks){Times.fill(0);WaitCalls=0;StartTicks=ticks;}
inline void Dump(double dt,double gpu,uint64_t ticks,double requested){
    static std::ofstream f("D:/Project/PEPEngine2/Build/FrameScheduling-20261108/frame-trace.csv");
    if(Frame==0){f<<"frame_index,dt_ms,gpu_ms,ticks,debt_ms,wait_calls,slot_index,slot_target,slot_completed";for(auto*n:Names)f<<','<<n<<"_ms";f<<'\n';f<<std::setprecision(12);}
    f<<Frame++<<','<<dt*1000<<','<<gpu<<','<<ticks-StartTicks<<','<<(requested-double(ticks)*(1.f/60))*1000<<','<<WaitCalls<<','<<SlotIndex<<','<<SlotTarget<<','<<SlotCompleted;
    for(auto x:Times)f<<','<<x;f<<'\n';
}
}
