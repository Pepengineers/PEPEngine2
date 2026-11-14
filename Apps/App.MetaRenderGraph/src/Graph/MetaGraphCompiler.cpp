#include "MetaGraph.h"
#include <algorithm>
#include <set>
#include <stdexcept>
namespace Meta {
namespace {
constexpr State GenericRead = 0xac3;
constexpr State CopyDest = 0x400;
constexpr State CopyStates = 0xc00;
constexpr State ComputeStates = 0x400000 | CopyStates | 0x200 | 0x40 | 0x8 | 0x1;
const char* Name(Queue q) {
    switch(q) { case Queue::Direct:return "DIRECT"; case Queue::Compute:return "COMPUTE"; case Queue::Copy:return "COPY"; }
    throw std::invalid_argument("unknown queue");
}
bool Legal(Queue q, State state) {
    Name(q);
    return q==Queue::Direct || (state & ~(q==Queue::Compute ? ComputeStates : CopyStates))==0;
}
bool FixedUpload(const Resource& r) { return r.Memory==Heap::Upload; }
void Depend(Packet& p, Id id) {
    if(id!=Invalid && std::find(p.Dependencies.begin(),p.Dependencies.end(),id)==p.Dependencies.end()) p.Dependencies.push_back(id);
}
}
Schedule Compile(const Graph& g, bool singleDirect) {
    if(singleDirect) { auto direct=g; for(auto& op:direct.Operations) op.Target=Queue::Direct; return Compile(direct); }
    const auto n=g.Operations.size();
    std::vector<std::set<Id>> deps(n);
    struct History { Id Writer=Invalid; std::set<Id> Readers; Id Last=Invalid; State Current; };
    std::vector<History> history;
    for(const auto& r:g.Resources) history.push_back({Invalid,{},Invalid,r.Initial});
    Id effect=Invalid;
    for(Id i=0;i<n;++i) {
        const auto& op=g.Operations[i]; Name(op.Target);
        if(!op.Lists) throw std::invalid_argument("packet requires at least one list");
        for(auto d:op.Dependencies) { if(d>=n) throw std::invalid_argument("unknown producer"); deps[i].insert(d); }
        if(op.SideEffect) { if(effect!=Invalid) deps[i].insert(effect); effect=i; }
        std::set<Id> seen;
        for(auto a:op.Accesses) {
            if(a.Resource>=history.size() || !seen.insert(a.Resource).second) throw std::invalid_argument("unknown or duplicate resource access");
            const auto& r=g.Resources[a.Resource]; auto& h=history[a.Resource];
            if(!Legal(op.Target,a.Required) && !(FixedUpload(r) && a.Usage==Mode::Read && a.Required==GenericRead))
                throw std::invalid_argument("queue cannot use declared resource state");
            if(r.Memory==Heap::Readback && (a.Required!=CopyDest || a.Usage!=Mode::Write))
                throw std::invalid_argument("readback heap requires write-only COPY_DEST");
            if(FixedUpload(r) && (a.Required!=GenericRead || a.Usage!=Mode::Read)) throw std::invalid_argument("upload heap must remain read-only GENERIC_READ");
            if(h.Writer!=Invalid) deps[i].insert(h.Writer);
            if(a.Usage!=Mode::Read) { deps[i].insert(h.Readers.begin(),h.Readers.end()); h.Readers.clear(); h.Writer=i; }
            else h.Readers.insert(i);
            // Whole-subresource state ownership crosses queues only after all readers.
            // Immutable upload heaps never transition and may be read concurrently.
            if(h.Last!=Invalid && !FixedUpload(r) && (h.Current!=a.Required || g.Operations[h.Last].Target!=op.Target)) {
                deps[i].insert(h.Last);
                for(auto reader:h.Readers) if(reader!=i) deps[i].insert(reader);
            }
            h.Last=i; h.Current=a.Required;
        }
    }
    Schedule out;
    for(const auto& r:g.Resources) {
        if(r.Memory!=Heap::Default && r.Memory!=Heap::Upload && r.Memory!=Heap::Readback)
            throw std::invalid_argument("unknown resource heap");
        if(r.Decays && r.Final!=0) throw std::invalid_argument("decaying buffer must end in COMMON");
        if(r.Memory!=Heap::Default && (r.Decays || r.Initial!=r.Final || r.Initial!=(r.Memory==Heap::Upload ? GenericRead : CopyDest)))
            throw std::invalid_argument("invalid fixed heap state contract");
        out.Decays.push_back(r.Decays); out.Memory.push_back(r.Memory);
    }
    std::vector<Id> remap(n,Invalid),last(g.Resources.size(),Invalid);
    std::vector<State> states;
    std::vector<bool> lastWrite(g.Resources.size(),false),accessed(g.Resources.size(),false);
    for(const auto& r:g.Resources) states.push_back(r.Initial);
    for(size_t count=0;count<n;++count) {
        Id next=Invalid;
        for(Id i=0;i<n;++i) if(remap[i]==Invalid && std::all_of(deps[i].begin(),deps[i].end(),[&](Id d){return remap[d]!=Invalid;})) {next=i;break;}
        if(next==Invalid) throw std::invalid_argument("dependency cycle");
        const auto& op=g.Operations[next];
        Packet p{next,{},op.Accesses,{},{},op.Lists,op.OwnerRecording,op.Target};
        for(auto d:deps[next]) Depend(p,remap[d]);
        for(auto a:op.Accesses) {
            auto& state=states[a.Resource]; const auto& r=g.Resources[a.Resource]; const Id prev=last[a.Resource];
            if(r.Memory!=Heap::Default) continue;
            if(prev!=Invalid && out.Packets[prev].Target!=p.Target) {
                Depend(p,prev);
                if(state!=0) { out.Packets[prev].After.push_back({a.Resource,state,0,false}); state=0; }
            }
            // Imported texture states can be illegal on their first COMPUTE/COPY
            // user. A DIRECT release packet owns that transition and publishes a token.
            if(!Legal(p.Target,state)) {
                Packet release{Invalid,{}, {},{{a.Resource,state,0,false}}, {},1};
                Depend(release,prev);
                const Id token=Id(out.Packets.size()); out.Packets.push_back(std::move(release)); Depend(p,token); state=0;
            }
            if(state!=a.Required) p.Before.push_back({a.Resource,state,a.Required,false});
            else if(r.Unordered && a.Required==UnorderedAccess && accessed[a.Resource] && (lastWrite[a.Resource] || a.Usage!=Mode::Read))
                p.Before.push_back({a.Resource,state,state,true});
            state=a.Required; lastWrite[a.Resource]=a.Usage!=Mode::Read; accessed[a.Resource]=true;
        }
        std::sort(p.Dependencies.begin(),p.Dependencies.end());
        remap[next]=Id(out.Packets.size()); out.Packets.push_back(std::move(p));
        for(auto a:op.Accesses) {
            last[a.Resource]=remap[next];
            // COPY texture accesses also decay. Release explicitly for ordinary
            // textures so subsequent queues always observe a known COMMON boundary.
            if(op.Target==Queue::Copy && !g.Resources[a.Resource].Decays && g.Resources[a.Resource].Memory==Heap::Default && states[a.Resource]!=0) {
                out.Packets.back().After.push_back({a.Resource,states[a.Resource],0,false}); states[a.Resource]=0;
            }
            if(g.Resources[a.Resource].Decays) states[a.Resource]=0;
        }
    }
    Packet terminal{Invalid,{}, {}, {}, {},1};
    for(Id i=0;i<out.Packets.size();++i) terminal.Dependencies.push_back(i);
    for(Id r=0;r<states.size();++r) {
        const auto prev=last[r];
        if(prev!=Invalid && out.Packets[prev].Target!=Queue::Direct && states[r]!=0 && g.Resources[r].Memory==Heap::Default) {
            out.Packets[prev].After.push_back({r,states[r],0,false}); states[r]=0;
        }
        if(states[r]!=g.Resources[r].Final) terminal.Before.push_back({r,states[r],g.Resources[r].Final,false});
        states[r]=g.Resources[r].Final;
    }
    out.Packets.push_back(std::move(terminal)); out.FinalStates=std::move(states); return out;
}
void Schedule::Write(std::ostream& os) const {
    for(Id i=0;i<Decays.size();++i) if(Decays[i]) os<<"resource "<<i<<" decays to COMMON at each packet submission\n";
    for(Id i=0;i<Packets.size();++i) {
        const auto& p=Packets[i];
        os<<"packet "<<i<<' '<<Name(p.Target)<<" op="<<p.Operation<<" lists="<<p.Lists<<" signal="<<i<<" deps=";
        for(auto d:p.Dependencies) os<<d<<',';
        os<<'\n';
        for(auto d:p.Dependencies) if(Packets.at(d).Target!=p.Target) os<<" wait "<<Name(Packets[d].Target)<<':'<<d<<'\n';
        if(p.OwnerRecording) os<<" owner-recorded native section\n";
        for(auto a:p.Accesses) os<<" access "<<a.Resource<<" mode="<<int(a.Usage)<<" state="<<a.Required<<'\n';
        for(auto b:p.Before) os<<" before "<<b.Resource<<' '<<b.Before<<"->"<<b.After<<(b.Uav?" UAV":" transition")<<'\n';
        for(auto b:p.After) os<<" after "<<b.Resource<<' '<<b.Before<<"->"<<b.After<<(b.Uav?" UAV":" transition")<<'\n';
    }
}
}
