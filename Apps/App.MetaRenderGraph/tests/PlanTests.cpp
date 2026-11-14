#include "Graph/MetaGraph.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
using namespace Meta;
void Check(bool v,const char* text) { if(!v) throw std::runtime_error(text); }
bool Depends(const Schedule& s,Id packet,Id dep) { const auto& d=s.Packets.at(packet).Dependencies; return std::find(d.begin(),d.end(),dep)!=d.end(); }
int main() { try {
    Graph g; g.Resources={{"buffer",0,0,true}};
    g.Operations={{"write",{{0,Mode::Write,8}}},{"read A",{{0,Mode::Read,8}}},{"read B",{{0,Mode::Read,8}}},{"write",{{0,Mode::Write,8}}}};
    auto s=Compile(g);
    Check(Depends(s,3,1)&&Depends(s,3,2),"WAR must join all readers");
    Check(Depends(s,1,0)&&Depends(s,2,0),"diamond RAW");
    Check(s.Packets[1].Before.size()==1 && s.Packets[1].Before[0].Uav,"same-state UAV barrier");
    Check(s.Packets.back().Before.size()==1 && s.FinalStates[0]==0,"boundary state");
    Check(s.Packets[3].Before.size()==1 && s.Packets[3].Before[0].Uav,"UAV WAR barrier after readers");
    Graph waw; waw.Resources=g.Resources;
    waw.Operations={{"first",{{0,Mode::Write,8}}},{"second",{{0,Mode::Write,8}}}};
    Check(Depends(Compile(waw),1,0),"WAW dependency");
    Graph readStates; readStates.Resources={{"read buffer",0,0,false}};
    readStates.Operations={{"A",{{0,Mode::Read,1}}},{"B",{{0,Mode::Read,1}}},{"C",{{0,Mode::Read,2}}}};
    Check(Depends(Compile(readStates),2,0)&&Depends(Compile(readStates),2,1),"state transition must join all readers");
    Graph reverse; reverse.Operations={{"consumer",{},{1}},{"producer",{}}};
    Check(Compile(reverse).Packets[0].Operation==1,"explicit forward producer order");
    std::ostringstream a,b; s.Write(a); Compile(g).Write(b); Check(a.str()==b.str(),"determinism");
    auto rejects=[&](Graph bad) { try { Compile(bad); } catch(const std::invalid_argument&) {return;} throw std::runtime_error("invalid graph accepted"); };
    auto bad=g; bad.Operations[0].Dependencies={99}; rejects(bad);
    bad=g; bad.Operations[0].Dependencies={3}; rejects(bad);
    bad=g; bad.Operations[0].Accesses.push_back({5,Mode::Read,0}); rejects(bad);
    bad=g; bad.Operations[0].Lists=0; rejects(bad);
    Graph independent;
    independent.Operations={{"empty",{}, {},true},{"two lists",{}, {},false,2},{"join",{}, {0,1},true}};
    s=Compile(independent);
    Check(s.Packets[0].Operation==0 && s.Packets[1].Operation==1 && s.Packets[1].Lists==2,"stable independent order");
    Check(Depends(s,2,0)&&Depends(s,2,1),"multi-input packet");
    Check(Compile({}).Packets.size()==1,"empty graph completion");
    Graph decay; decay.Resources={{"DEFAULT buffer",0,0,true,true}};
    decay.Operations={{"upload",{{0,Mode::Write,1024}}},{"UAV",{{0,Mode::ReadWrite,8}}},{"UAV again",{{0,Mode::ReadWrite,8}}}};
    s=Compile(decay);
    Check(s.Decays[0] && s.Packets.back().Before.empty(),"buffer must decay before terminal packet");
    for(size_t i=0;i<3;++i) Check(s.Packets[i].Before.size()==1 && s.Packets[i].Before[0].Before==0 && !s.Packets[i].Before[0].Uav,"per-submission COMMON contract");
    decay.Resources[0].Final=8; rejects(decay);
    Graph queues;
    queues.Resources={{"buffer",0,0,true,true}};
    queues.Operations={{"copy",{{0,Mode::Write,0x400}}},{"compute",{{0,Mode::ReadWrite,8}}},
        {"direct reader",{{0,Mode::Read,0x800}}},{"compute reader",{{0,Mode::Read,0x800}}},
        {"copy reuse",{{0,Mode::Write,0x400}}}};
    queues.Operations[0].Target=queues.Operations[4].Target=Queue::Copy;
    queues.Operations[1].Target=queues.Operations[3].Target=Queue::Compute;
    s=Compile(queues);
    Check(Depends(s,4,2)&&Depends(s,4,3),"COPY reuse must join both queue readers");
    Check(s.Packets[0].Target==Queue::Copy && s.Packets[1].Target==Queue::Compute,"queue placement lost");
    for(const auto& p:Compile(queues,true).Packets) Check(p.Target==Queue::Direct,"single-DIRECT override lost");
    bad=queues; bad.Operations[1].Accesses[0].Required=4; rejects(bad);
    bad=queues; bad.Operations[0].Accesses[0].Required=8; rejects(bad);
    bad=queues; bad.Operations[0].Dependencies={4}; rejects(bad);
    Graph fixed;
    fixed.Resources={{"upload",0xac3,0xac3,false,false,Heap::Upload},
        {"readback",0x400,0x400,false,false,Heap::Readback}};
    fixed.Operations={{"copy",{{0,Mode::Read,0xac3},{1,Mode::Write,0x400}}}};
    fixed.Operations[0].Target=Queue::Copy;
    s=Compile(fixed);
    Check(s.Packets[0].Before.empty()&&s.Packets[0].After.empty(),"fixed heaps must never transition");
    bad=fixed; bad.Operations[0].Accesses[0].Usage=Mode::Write; rejects(bad);
    bad=fixed; bad.Operations[0].Accesses[1].Usage=Mode::Read; rejects(bad);
    bad=fixed; bad.Resources[0].Final=0; rejects(bad);
    bad=fixed; bad.Resources[1].Decays=true; rejects(bad);
    Graph imported;
    imported.Resources={{"SRV texture",0xc0,0xc0}};
    imported.Operations={{"compute",{{0,Mode::Read,0x40}}}};
    imported.Operations[0].Target=Queue::Compute;
    s=Compile(imported);
    Check(s.Packets.size()==3 && s.Packets[0].Target==Queue::Direct && s.Packets[0].Operation==Invalid,
        "illegal imported COMPUTE state needs a DIRECT release");
    Check(Depends(s,1,0)&&s.Packets[1].After.size()==1&&s.Packets[1].After[0].After==0,
        "texture handoff must release and fence before queue acquire");
    std::cout << "PASS PlanTests: WAR/RAW/WAW, diamond, UAV, boundaries, cycle/unknown, empty, multi-list, multi-input, determinism\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
