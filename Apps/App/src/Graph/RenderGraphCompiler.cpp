#include "RenderGraph.h"
#include <algorithm>
#include <stdexcept>
namespace BistroRender {
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
    const auto target = [singleDirect](const Operation& op) { return singleDirect ? Queue::Direct : op.Target; };
    const auto n=g.Operations.size();
    std::vector<Id> dependencies;
    std::vector<size_t> offsets(n + 1);
    std::vector<Id> lastConsumer(n, Invalid);
    struct History { Id Writer=Invalid; std::vector<Id> Readers; Id Last=Invalid; State Current; };
    std::vector<History> history;
    history.reserve(g.Resources.size());
    for(const auto& r:g.Resources) history.push_back({Invalid,{},Invalid,r.Initial});
    Id effect=Invalid;
    for(Id i=0;i<n;++i) {
        offsets[i] = dependencies.size();
        const auto addDependency = [&](Id producer) {
            if (lastConsumer[producer] != i) {
                lastConsumer[producer] = i;
                dependencies.push_back(producer);
            }
        };
        const auto& op=g.Operations[i];
        const auto queue=target(op);
        Name(queue);
        if(!op.Lists) throw std::invalid_argument("packet requires at least one list");
        for(auto d:op.Dependencies) { if(d>=n) throw std::invalid_argument("unknown producer"); addDependency(d); }
        if(op.SideEffect) { if(effect!=Invalid) addDependency(effect); effect=i; }
        for(auto a:op.Accesses) {
            if(a.Resource>=history.size() || history[a.Resource].Last == i) throw std::invalid_argument("unknown or duplicate resource access");
            const auto& r=g.Resources[a.Resource]; auto& h=history[a.Resource];
            if(!Legal(queue,a.Required) && !(FixedUpload(r) && a.Usage==Mode::Read && a.Required==GenericRead))
                throw std::invalid_argument("queue cannot use declared resource state");
            if(r.Memory==Heap::Readback && (a.Required!=CopyDest || a.Usage!=Mode::Write))
                throw std::invalid_argument("readback heap requires write-only COPY_DEST");
            if(FixedUpload(r) && (a.Required!=GenericRead || a.Usage!=Mode::Read)) throw std::invalid_argument("upload heap must remain read-only GENERIC_READ");
            if(h.Writer!=Invalid) addDependency(h.Writer);
            if(a.Usage!=Mode::Read) { for (Id reader : h.Readers) addDependency(reader); h.Readers.clear(); h.Writer=i; }
            else h.Readers.push_back(i);
            // Whole-subresource state ownership crosses queues only after all readers.
            // Immutable upload heaps never transition and may be read concurrently.
            if(h.Last!=Invalid && !FixedUpload(r) && (h.Current!=a.Required || target(g.Operations[h.Last])!=queue)) {
                addDependency(h.Last);
                for(auto reader:h.Readers) if(reader!=i) addDependency(reader);
            }
            h.Last=i; h.Current=a.Required;
        }
    }
    offsets[n] = dependencies.size();
    Schedule out;
    out.Packets.reserve(n + 1);
    out.Device=g.Device; out.FrameBoundary=g.FrameBoundary; out.TerminalQueue=g.TerminalQueue;
    Name(g.TerminalQueue);
    if(g.Device==Invalid || (g.FrameBoundary && g.TerminalQueue!=Queue::Direct))
        throw std::invalid_argument("invalid device/terminal boundary");
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
    std::vector<State> lastRequired(g.Resources.size());
    std::vector<bool> lastWrite(g.Resources.size(),false),accessed(g.Resources.size(),false);
    for(const auto& r:g.Resources) states.push_back(r.Initial);
    Id firstPending = 0;
    for (size_t count = 0; count < n; ++count) {
        while (remap[firstPending] != Invalid) ++firstPending;
        Id next = Invalid;
        for (Id i = firstPending; i < n; ++i) {
            if (remap[i] == Invalid && std::all_of(dependencies.begin() + offsets[i], dependencies.begin() + offsets[i + 1],
                    [&](Id dependency) { return remap[dependency] != Invalid; })) {
                next = i;
                break;
            }
        }
        if(next==Invalid) throw std::invalid_argument("dependency cycle");
        const auto& op=g.Operations[next];
        const auto queue=target(op);
        Packet p{next,{},op.Accesses,{},{},op.Lists,op.OwnerRecording,queue,op.Name,op.TransferBytes};
        p.EarliestSubmitStart = 0;
        for (Id dep : op.Dependencies)
            p.EarliestSubmitStart = (std::max)(p.EarliestSubmitStart, remap[dep] + 1);
        if (op.SideEffect || op.OwnerRecording || op.Accesses.empty()) p.EarliestSubmitStart = Invalid;
        p.Dependencies.reserve(offsets[next + 1] - offsets[next]);
        for (size_t edge = offsets[next]; edge < offsets[next + 1]; ++edge)
            p.Dependencies.push_back(remap[dependencies[edge]]);
        for(auto a:op.Accesses) {
            auto& state=states[a.Resource]; const auto& r=g.Resources[a.Resource]; const Id prev=last[a.Resource];
            if(r.Memory!=Heap::Default) {
                if (prev != Invalid && r.Memory == Heap::Readback)
                    p.EarliestSubmitStart = (std::max)(p.EarliestSubmitStart, prev + 1);
                continue;
            }
            if(prev!=Invalid && out.Packets[prev].Target!=p.Target) {
                Depend(p,prev);
                if(state!=0) { out.Packets[prev].After.push_back({a.Resource,state,0,false}); state=0; }
            }
            // Imported texture states can be illegal on their first COMPUTE/COPY
            // user. A DIRECT release packet owns that transition and publishes a token.
            if(!Legal(p.Target,state)) {
                Packet release{Invalid,{}, {},{{a.Resource,state,0,false}}, {},1,false,Queue::Direct,
                               op.Name + " / queue handoff"};
                Depend(release,prev);
                const Id token=Id(out.Packets.size()); out.Packets.push_back(std::move(release)); Depend(p,token); state=0;
            }
            const bool orderWrites = lastWrite[a.Resource] || a.Usage != Mode::Read;
            const bool useUavBarrier = r.Unordered && a.Required == UnorderedAccess;
            if (prev != Invalid && out.Packets[prev].Target == queue) {
                const auto previous = lastRequired[a.Resource];
                // Preserve ordering where a same-state write has no graph barrier, or a
                // callback may implicitly promote a resource declared in COMMON.
                if ((previous == a.Required && orderWrites && !useUavBarrier) ||
                    previous == 0 || a.Required == 0)
                    p.EarliestSubmitStart = (std::max)(p.EarliestSubmitStart, prev + 1);
            }
            if(state!=a.Required) {
                Barrier transition{a.Resource,state,a.Required,false};
                if (r.Decays && prev != Invalid && out.Packets[prev].Target == queue) {
                    transition.PreviousBufferUse = prev;
                    transition.BeforeWithoutDecay = lastRequired[a.Resource];
                    transition.OrderUavWithoutDecay = useUavBarrier && orderWrites;
                }
                p.Before.push_back(transition);
            }
            else if(r.Unordered && a.Required==UnorderedAccess && accessed[a.Resource] && (lastWrite[a.Resource] || a.Usage!=Mode::Read))
                p.Before.push_back({a.Resource,state,state,true});
            state=a.Required; lastRequired[a.Resource]=a.Required; lastWrite[a.Resource]=a.Usage!=Mode::Read; accessed[a.Resource]=true;
        }
        std::sort(p.Dependencies.begin(),p.Dependencies.end());
        remap[next]=Id(out.Packets.size()); out.Packets.push_back(std::move(p));
        for(auto a:op.Accesses) {
            last[a.Resource]=remap[next];
            // COPY texture accesses also decay. Release explicitly for ordinary
            // textures so subsequent queues always observe a known COMMON boundary.
            if(queue==Queue::Copy && !g.Resources[a.Resource].Decays && g.Resources[a.Resource].Memory==Heap::Default && states[a.Resource]!=0) {
                out.Packets.back().After.push_back({a.Resource,states[a.Resource],0,false}); states[a.Resource]=0;
            }
            if(g.Resources[a.Resource].Decays) states[a.Resource]=0;
        }
    }
    Packet terminal{Invalid,{}, {}, {}, {},1,false,g.TerminalQueue,"frame final states"};
    for(Id i=0;i<out.Packets.size();++i) terminal.Dependencies.push_back(i);
    for(Id r=0;r<states.size();++r) {
        const auto prev=last[r];
        if(prev!=Invalid && out.Packets[prev].Target!=g.TerminalQueue && states[r]!=0 && g.Resources[r].Memory==Heap::Default) {
            out.Packets[prev].After.push_back({r,states[r],0,false}); states[r]=0;
        }
        if(g.Resources[r].Memory==Heap::Default &&
            (!Legal(g.TerminalQueue,states[r]) || !Legal(g.TerminalQueue,g.Resources[r].Final)))
            throw std::invalid_argument("terminal queue cannot export resource state");
        if(states[r]!=g.Resources[r].Final) terminal.Before.push_back({r,states[r],g.Resources[r].Final,false});
        states[r]=g.Resources[r].Final;
    }
    out.Packets.push_back(std::move(terminal)); out.FinalStates=std::move(states); return out;
}
void Schedule::Write(std::ostream& os) const {
    os<<"device "<<Device<<" frame-boundary="<<FrameBoundary<<" terminal="<<Name(TerminalQueue)<<'\n';
    for(Id i=0;i<Decays.size();++i) if(Decays[i]) os<<"resource "<<i<<" decays to COMMON at each native submission\n";
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
