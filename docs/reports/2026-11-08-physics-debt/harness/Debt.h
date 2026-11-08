#pragma once
#include <Windows.h>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <array>
#include <vector>
#include <atomic>
#include <cstdlib>
#include <stdexcept>
namespace Debt {
using Clock = std::chrono::steady_clock;
inline double Now() { return std::chrono::duration<double,std::milli>(Clock::now().time_since_epoch()).count(); }
inline double ProcessCPU() { FILETIME a,b,c,d; GetProcessTimes(GetCurrentProcess(),&a,&b,&c,&d); ULARGE_INTEGER k{},u{}; k.LowPart=c.dwLowDateTime;k.HighPart=c.dwHighDateTime;u.LowPart=d.dwLowDateTime;u.HighPart=d.dwHighDateTime;return double(k.QuadPart+u.QuadPart)*.0001; }
inline bool Detail = GetEnvironmentVariableA("DEBT_DETAIL",nullptr,0) != 0;
inline bool SyncProbe = GetEnvironmentVariableA("DEBT_SYNC_PROBE",nullptr,0) != 0;
inline bool Relaxed = GetEnvironmentVariableA("DEBT_RELAXED",nullptr,0) != 0;
inline unsigned Frame=0, Tick=0;
inline std::atomic<bool> Sample{false};
inline double Pre=0,Callback=0,Backend=0,Post=0,CPU=0,Actual=0,Requested=0,Physics=0;
inline unsigned Active=0, Bodies=0;
inline bool PoseProbe=false;
inline double PoseLock=0,PoseTotal=0;
inline std::ofstream Frames("D:/Project/PEPEngine2/Build/PhysicsDebt-20261108/frames.csv");
inline std::ofstream Steps("D:/Project/PEPEngine2/Build/PhysicsDebt-20261108/steps.csv");
struct Init { Init(){Frames<<"frame,dt_ms,physics_ms,accumulator_ms,gpu_ms\n";Steps<<"frame,tick,pre_ms,callback_ms,backend_ms,post_ms,process_cpu_ms,active,bodies,debt_ms,wall_limit\n";Frames<<std::setprecision(12);Steps<<std::setprecision(12);} };
inline Init InitOnce;
struct Scope { double& Value;bool Enabled;double Start;explicit Scope(double& v,bool enabled=true):Value(v),Enabled(enabled),Start(enabled?Now():0){}~Scope(){if(Enabled)Value+=Now()-Start;} };
struct StepRow { unsigned frame,tick; double pre,callback,backend,post,cpu;unsigned active,bodies;double debt;bool limit; };
inline std::vector<StepRow> Rows;
inline void BeginStep(){++Tick;Sample.store(Detail && Tick%60==0);Pre=Callback=Backend=Post=0;CPU=ProcessCPU();}
inline void EndStep(double debt,bool limit){Rows.push_back({Frame,Tick,Pre,Callback,Backend,Post,ProcessCPU()-CPU,Active,Bodies,debt,limit});Actual=debt;}
inline void FlushFrame(double dt,double gpu){Frames<<Frame++<<','<<dt*1000<<','<<Physics<<','<<Actual*1000<<','<<gpu<<'\n';for(auto&r:Rows)Steps<<r.frame<<','<<r.tick<<','<<r.pre<<','<<r.callback<<','<<r.backend<<','<<r.post<<','<<r.cpu<<','<<r.active<<','<<r.bodies<<','<<r.debt*1000<<','<<r.limit<<'\n';Rows.clear();Physics=0;}
struct alignas(64) ThreadStats { double create=0,queue=0,free=0,drain=0,barrier=0;unsigned creates=0,queues=0; };
inline std::array<ThreadStats,64> Stats;
inline std::atomic<unsigned> ThreadCount{0};
inline unsigned ThreadIndex(){thread_local unsigned index=ThreadCount.fetch_add(1);if(index>=Stats.size())throw std::runtime_error("Too many threads");return index;}
inline ThreadStats& Local(){return Stats[ThreadIndex()];}
inline void DumpStats(){std::ofstream f("D:/Project/PEPEngine2/Build/PhysicsDebt-20261108/jobs.csv");f<<"thread,create_ms,queue_ms,free_ms,drain_ms,barrier_ms,creates,queues\n";for(unsigned i=0;i<ThreadCount;++i){auto&s=Stats[i];f<<i<<','<<s.create<<','<<s.queue<<','<<s.free<<','<<s.drain<<','<<s.barrier<<','<<s.creates<<','<<s.queues<<'\n';}}
}
