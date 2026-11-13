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
    std::cout << "PASS PlanTests: WAR/RAW/WAW, diamond, UAV, boundaries, cycle/unknown, empty, multi-list, multi-input, determinism\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
