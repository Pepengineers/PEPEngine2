#pragma once
#include "Debt.h"
#include <Jolt/Jolt.h>
#include <Jolt/Core/Profiler.h>
#include <Jolt/Core/TickCounter.h>
namespace Debt {
inline std::array<JPH::ProfileThread*,64> Profiles{};
struct Event { unsigned tick,thread;const char*name;uint64_t begin,end; };
inline std::vector<Event> Events;
inline void Attach(){unsigned i=ThreadIndex();if(!Profiles[i])Profiles[i]=new JPH::ProfileThread("debt");JPH::ProfileThread::sSetInstance(Profiles[i]);}
inline void ThreadProfile(){if(Sample.load())Attach();else JPH::ProfileThread::sSetInstance(nullptr);}
inline void BeginBackend(){if(!Detail)return;if(!JPH::Profiler::sInstance)JPH::Profiler::sInstance=new JPH::Profiler;for(auto*p:Profiles)if(p)p->mCurrentSample=0;ThreadProfile();}
inline void EndBackend(){if(Sample.load()){for(unsigned t=0;t<ThreadCount;++t){auto*p=Profiles[t];if(!p)continue;if(p->mCurrentSample==p->cMaxSamples)throw std::runtime_error("Jolt sample overflow");for(unsigned i=0;i<p->mCurrentSample;++i){auto&s=p->mSamples[i];Events.push_back({Tick,t,s.mName,s.mStartCycle,s.mEndCycle});}}}JPH::ProfileThread::sSetInstance(nullptr);}
inline void DumpJolt(){std::ofstream f("D:/Project/PEPEngine2/Build/PhysicsDebt-20261108/jolt-events.csv");f<<"tick,thread,name,start_cycle,end_cycle\n";for(auto&e:Events)f<<e.tick<<','<<e.thread<<','<<e.name<<','<<e.begin<<','<<e.end<<'\n';std::ofstream clock("D:/Project/PEPEngine2/Build/PhysicsDebt-20261108/clock.json");clock<<"{\"ticks_per_second\":"<<std::setprecision(15)<<(JPH::Profiler::sInstance?JPH::Profiler::sInstance->GetProcessorTicksPerSecond():0)<<"}";DumpStats();}
}
