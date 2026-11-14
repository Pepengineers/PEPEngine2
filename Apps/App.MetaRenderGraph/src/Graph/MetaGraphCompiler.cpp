#include "MetaGraph.h"
#include <algorithm>
#include <set>
#include <stdexcept>
namespace Meta {
Schedule Compile(const Graph& g) {
    const auto n = g.Operations.size();
    std::vector<std::set<Id>> deps(n);
    struct History { Id Writer = Invalid; std::set<Id> Readers; Id Last = Invalid; State Current; };
    std::vector<History> history;
    for (const auto& r : g.Resources) history.push_back({Invalid, {}, Invalid, r.Initial});
    Id effect = Invalid;
    for (Id i=0; i<n; ++i) {
        const auto& op=g.Operations[i];
        if (!op.Lists) throw std::invalid_argument("packet requires at least one list");
        for (auto d:op.Dependencies) {
            if (d>=n) throw std::invalid_argument("unknown producer");
            deps[i].insert(d);
        }
        if (op.SideEffect) { if(effect!=Invalid) deps[i].insert(effect); effect=i; }
        std::set<Id> seen;
        for (auto a:op.Accesses) {
            if (a.Resource>=history.size() || !seen.insert(a.Resource).second)
                throw std::invalid_argument("unknown or duplicate resource access");
            auto& h=history[a.Resource];
            if(h.Writer!=Invalid) deps[i].insert(h.Writer);
            if(a.Usage!=Mode::Read) {
                deps[i].insert(h.Readers.begin(),h.Readers.end());
                h.Readers.clear(); h.Writer=i;
            } else h.Readers.insert(i);
            // State changes also order readers. Resource states apply to the
            // whole subresource, even when the logical access is read-only.
            if(h.Current!=a.Required && h.Last!=Invalid) {
                deps[i].insert(h.Last);
                for(auto reader:h.Readers) if(reader!=i) deps[i].insert(reader);
            }
            h.Last=i; h.Current=a.Required;
        }
    }
    Schedule out;
    for(const auto& r:g.Resources) {
        if(r.Decays && r.Final!=0) throw std::invalid_argument("decaying buffer must end in COMMON");
        out.Decays.push_back(r.Decays);
    }
    std::vector<Id> remap(n,Invalid);
    std::vector<State> states;
    std::vector<bool> lastWrite(g.Resources.size(),false), accessed(g.Resources.size(),false);
    for(auto r:g.Resources) states.push_back(r.Initial);
    for(size_t count=0;count<n;++count) {
        Id next=Invalid;
        for(Id i=0;i<n;++i) if(remap[i]==Invalid && std::all_of(deps[i].begin(),deps[i].end(),[&](Id d){return remap[d]!=Invalid;})) {next=i;break;}
        if(next==Invalid) throw std::invalid_argument("dependency cycle");
        const auto& op=g.Operations[next];
        Packet p{next,{},op.Accesses,{},{},op.Lists,op.OwnerRecording};
        for(auto d:deps[next]) p.Dependencies.push_back(remap[d]);
        std::sort(p.Dependencies.begin(),p.Dependencies.end());
        for(auto a:op.Accesses) {
            auto& state=states[a.Resource];
            if(state!=a.Required) p.Before.push_back({a.Resource,state,a.Required,false});
            else if(g.Resources[a.Resource].Unordered && a.Required==UnorderedAccess &&
                accessed[a.Resource] && (lastWrite[a.Resource] || a.Usage!=Mode::Read))
                p.Before.push_back({a.Resource,state,state,true});
            state=a.Required;
            lastWrite[a.Resource]=a.Usage!=Mode::Read;
            accessed[a.Resource]=true;
        }
        remap[next]=static_cast<Id>(out.Packets.size());
        out.Packets.push_back(std::move(p));
        for(Id r=0;r<states.size();++r) if(g.Resources[r].Decays) states[r]=0;
    }
    // Final transitions are a separate terminal packet joined with all work.
    Packet terminal{Invalid,{}, {}, {}, {}, 1};
    for(Id i=0;i<out.Packets.size();++i) terminal.Dependencies.push_back(i);
    for(Id r=0;r<states.size();++r) {
        if(states[r]!=g.Resources[r].Final) terminal.Before.push_back({r,states[r],g.Resources[r].Final,false});
        states[r]=g.Resources[r].Final;
    }
    out.Packets.push_back(std::move(terminal)); out.FinalStates=std::move(states);
    return out;
}
void Schedule::Write(std::ostream& os) const {
    for(Id i=0;i<Decays.size();++i) if(Decays[i]) os << "resource " << i << " decays to COMMON at each packet submission\n";
    for(Id i=0;i<Packets.size();++i) {
        const auto& p=Packets[i];
        os << "packet " << i << " DIRECT op=" << p.Operation << " lists=" << p.Lists << " deps=";
        for(auto d:p.Dependencies) os << d << ',';
        os << '\n';
        if(p.OwnerRecording)os << " owner-recorded cold initialization\n";
        for(auto a:p.Accesses) os << " access " << a.Resource << " mode=" << int(a.Usage) << " state=" << a.Required << '\n';
        for(auto b:p.Before) os << " barrier " << b.Resource << ' ' << b.Before << "->" << b.After << (b.Uav?" UAV":" transition") << '\n';
    }
}
}
