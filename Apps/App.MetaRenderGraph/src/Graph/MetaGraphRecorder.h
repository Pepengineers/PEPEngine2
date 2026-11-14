#pragma once
#include "Engine.Core/Jobs/JobSystem.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <ostream>
namespace Meta {
// Frozen trees: a group launches children and returns; its continuation is a
// separate task queued by the last child. No worker waits for descendants.
struct RecordingGroup {
    std::string Name;
    std::vector<RecordingGroup> Children;
    std::function<void()> Continue;
};
class Recorder {
public:
    Recorder(JobSystem&, std::vector<RecordingGroup>, bool serial = false,
        const std::atomic<bool>* cancellation = nullptr);
    ~Recorder();
    void Wait(size_t group); // owner only, throws on error/cancellation
    void Finish(); // joins native task callbacks as well as logical results
    void Mark(const std::string&); // owner submission marker in the same clock
    double RecordingWorkMS() const;
    void WriteTrace(std::ostream&) const; // Chrome trace JSON, microseconds
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
