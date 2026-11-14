#include "MetaGraphRecorder.h"
#include "../../../../External/External.Libraries/enkiTS/TaskScheduler.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
namespace Meta {
struct Recorder::Impl {
    struct Node;
    struct Task : enki::ITaskSet {
        Task(Node& n, bool join) : Owner(n), Join(join) { m_SetSize=1; }
        void ExecuteRange(enki::TaskSetPartition, uint32_t thread) override;
        Node& Owner; bool Join;
    };
    struct Node {
        Node(Impl& o, RecordingGroup g, Node* p) : Owner(o), Group(std::move(g)), Parent(p), Start(*this,false), Join(*this,true) {}
        Impl& Owner; RecordingGroup Group; Node* Parent;
        std::vector<Node*> Children;
        std::atomic<size_t> Remaining{0};
        std::atomic<bool> Done{false}, Started{false}, Joined{false};
        Task Start, Join;
        void ChildDone() {
            if(Remaining.fetch_sub(1,std::memory_order_acq_rel)==1) {
                Joined.store(true,std::memory_order_release);
                Owner.Jobs.QueueTask(Join);
            }
        }
        void Complete() {
            Done.store(true,std::memory_order_release);
            if(Parent && !Owner.Serial) Parent->ChildDone();
            if(!Parent) Owner.Changed.notify_all();
        }
    };
    struct Event { std::string Name; uint32_t Thread; int64_t Start, Duration; };
    JobSystem& Jobs; bool Serial; const std::atomic<bool>* Cancellation;
    std::vector<std::unique_ptr<Node>> Nodes;
    std::vector<Node*> Roots;
    std::mutex Mutex;
    std::condition_variable Changed;
    std::exception_ptr Error;
    std::atomic<bool> Failed{false};
    std::vector<Event> Events;
    std::chrono::steady_clock::time_point Epoch=std::chrono::steady_clock::now();
    Impl(JobSystem& jobs,bool serial,const std::atomic<bool>* cancel):Jobs(jobs),Serial(serial),Cancellation(cancel) {}
    int64_t Now() const { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-Epoch).count(); }
    bool Cancelled() const { return Failed.load(std::memory_order_acquire) || (Cancellation && Cancellation->load(std::memory_order_acquire)); }
    Node* Build(RecordingGroup group,Node* parent) {
        auto node=std::make_unique<Node>(*this,std::move(group),parent);
        auto* p=node.get(); Nodes.push_back(std::move(node));
        for(auto& child:p->Group.Children) p->Children.push_back(Build(std::move(child),p));
        p->Group.Children.clear(); return p;
    }
    void Run(Node& n,uint32_t thread) {
        const auto start=Now();
        try { if(!Cancelled() && n.Group.Continue) n.Group.Continue(); }
        catch(...) {
            std::lock_guard<std::mutex> lock(Mutex);
            if(!Error) Error=std::current_exception();
            Failed.store(true,std::memory_order_release);
        }
        const auto end=Now();
        { std::lock_guard<std::mutex> lock(Mutex); Events.push_back({n.Group.Name,thread,start,end-start}); }
        n.Complete();
    }
    void SerialRun(Node& n) { for(auto* c:n.Children) SerialRun(*c); Run(n,0); }
    void Check() {
        std::lock_guard<std::mutex> lock(Mutex);
        if(Error) std::rethrow_exception(Error);
        if(Cancelled()) throw std::runtime_error("recording cancelled");
    }
};
void Recorder::Impl::Task::ExecuteRange(enki::TaskSetPartition,uint32_t thread) {
    auto& n=Owner;
    if(Join || n.Children.empty()) { n.Owner.Run(n,thread); return; }
    n.Remaining=n.Children.size()+1; // launch sentinel protects fast children
    for(auto* child:n.Children) {
        child->Started.store(true,std::memory_order_release);
        n.Owner.Jobs.QueueTask(child->Start);
    }
    n.ChildDone();
}
Recorder::Recorder(JobSystem& jobs,std::vector<RecordingGroup> groups,bool serial,const std::atomic<bool>* cancel)
    :_impl(std::make_unique<Impl>(jobs,serial,cancel)) {
    for(auto& g:groups) _impl->Roots.push_back(_impl->Build(std::move(g),nullptr));
    for(auto* root:_impl->Roots) {
        if(serial) _impl->SerialRun(*root);
        else { root->Started.store(true,std::memory_order_release); jobs.QueueTask(root->Start); }
    }
}
void Recorder::Wait(size_t group) {
    auto* root=_impl->Roots.at(group);
    while(!root->Done.load(std::memory_order_acquire)) {
        // With a single scheduler thread the owner must help execute tasks.
        // Multi-thread pools leave the owner available for early submission.
        if(_impl->Jobs.GetThreadCount()==1) _impl->Jobs.WaitForTask();
        else {
            std::unique_lock<std::mutex> lock(_impl->Mutex);
            _impl->Changed.wait_for(lock,std::chrono::milliseconds(1),[&] { return root->Done.load(std::memory_order_acquire); });
        }
    }
    _impl->Check();
}
void Recorder::Finish() {
    for(size_t i=0;i<_impl->Roots.size();++i) {
        auto* r=_impl->Roots[i];
        while(!r->Done.load(std::memory_order_acquire)) {
            _impl->Jobs.WaitForTask(); std::this_thread::yield();
        }
    }
    // Logical completion is published inside ExecuteRange. Wait for enki's
    // epilogue before destroying any task, including ancestor launch tasks.
    if(!_impl->Serial) for(auto& n:_impl->Nodes) {
        if(n->Started.load(std::memory_order_acquire)) _impl->Jobs.WaitForTask(&n->Start);
        if(n->Joined.load(std::memory_order_acquire)) _impl->Jobs.WaitForTask(&n->Join);
    }
}
Recorder::~Recorder() { Finish(); }
void Recorder::Mark(const std::string& name) {
    std::lock_guard<std::mutex> lock(_impl->Mutex);
    _impl->Events.push_back({name,0,_impl->Now(),0});
}
double Recorder::RecordingWorkMS() const {
    std::lock_guard<std::mutex> lock(_impl->Mutex);
    int64_t total=0;
    for(const auto& e:_impl->Events) if(e.Name.rfind("list ",0)==0) total+=e.Duration;
    return double(total)/1000.;
}
void Recorder::WriteTrace(std::ostream& out) const {
    std::lock_guard<std::mutex> lock(_impl->Mutex);
    out << "{\"traceEvents\":[";
    bool first=true;
    for(const auto& e:_impl->Events) {
        if(!first) out << ','; first=false;
        // Names are generated by the recorder caller; JSON-escape quotes/slashes.
        std::string name; for(char c:e.Name) { if(c=='"' || c=='\\') name+='\\'; if(c<32) name+=' '; else name+=c; }
        out << "{\"name\":\"" << name << "\",\"ph\":\"X\",\"pid\":1,\"tid\":" << e.Thread
            << ",\"ts\":" << e.Start << ",\"dur\":" << e.Duration << '}';
    }
    out << "]}";
}
}
