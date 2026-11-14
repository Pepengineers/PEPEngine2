#include "Graph/MetaGraphRecorder.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace {
void Check(bool value) { if(!value) throw std::runtime_error("recording test failed"); }
void Tree(unsigned threads,size_t count,bool serial) {
    JobSystem jobs(threads);
    std::vector<size_t> result(count,0);
    std::atomic<size_t> completed{0};
    Meta::RecordingGroup nested{"nested",{},[&] { Check(completed==count); }};
    for(size_t i=0;i<count;++i) nested.Children.push_back({"bucket "+std::to_string(i),{},[&,i] {
        if(i%3==0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        result[i]=i*7+1; ++completed;
    }});
    bool joined=false;
    Meta::RecordingGroup root{"parent",{std::move(nested)},[&] { Check(completed==count); joined=true; }};
    Meta::Recorder recorder(jobs,{std::move(root)},serial);
    recorder.Wait(0); recorder.Finish(); Check(joined);
    for(size_t i=0;i<count;++i) Check(result[i]==i*7+1);
}
}
int main() {
    try {
        for(unsigned threads:{1u,4u}) for(size_t count:{0u,1u,37u}) for(bool serial:{false,true}) Tree(threads,count,serial);
        JobSystem jobs(4);
        for(bool cancel:{false,true}) {
            std::atomic<bool> cancellation{cancel}; bool threw=false;
            Meta::Recorder recorder(jobs,{{"failure",{{"child",{},[] { throw std::runtime_error("injected"); }}},[] {}}},false,&cancellation);
            try { recorder.Wait(0); } catch(const std::runtime_error&) { threw=true; }
            recorder.Finish(); Check(threw);
        }
        // Shutdown joins running jobs even if the caller never Waits.
        std::atomic<bool> done{false};
        { Meta::Recorder recorder(jobs,{{"shutdown",{},[&] { done=true; }}}); }
        Check(done);
        std::atomic<bool> stopping{false}; bool continued=false, cancelled=false;
        {
            Meta::Recorder recorder(jobs,{{"shutdown cancellation",{{"running child",{},[&] { stopping=true; }}},[&] { continued=true; }}},false,&stopping);
            try { recorder.Wait(0); } catch(const std::runtime_error&) { cancelled=true; }
        }
        Check(cancelled && !continued);
        std::cout << "PASS RecordingJobsTests: serial/parallel 0/1/37; nested fan-out/fan-in/continuation; 1/4 threads; error/cancellation/shutdown lifetime\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
