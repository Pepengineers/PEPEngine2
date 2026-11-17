#pragma once
#include "Engine.Core/Jobs/JobSystem.h"
#include <atomic>
#include <functional>
#include <memory>
#include <vector>
namespace Meta {
// Frozen trees: a group launches children and returns; its continuation is a
// separate task queued by the last child. No worker waits for descendants.
struct RecordingGroup {
    std::vector<RecordingGroup> Children;
    std::function<void()> Continue;
};
class Recorder {
public:
    Recorder(JobSystem&, std::vector<RecordingGroup>,
        const std::atomic<bool>* cancellation = nullptr);
    ~Recorder();
    void Wait(size_t group); // owner only, throws on error/cancellation
    void Finish(); // joins native task callbacks as well as logical results
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
