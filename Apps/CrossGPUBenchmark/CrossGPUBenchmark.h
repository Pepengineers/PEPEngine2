#pragma once

#include <App.Base/App.h>

#include <string>
#include <vector>

struct CrossGPUStage
{
    std::string Name;
    UINT Size;
    UINT Reads;
    EDeviceRole Producer;
    MemoryType Storage;
    MemoryType Access;
    TransportQueuePolicy Queue;
};

EDeviceRole ConsumerRole(const CrossGPUStage& stage);
void AddCrossGPUStages(std::vector<CrossGPUStage>& stages, EDeviceRole producer, UINT size, UINT reads,
    const std::string& workload);

template<class Pass, class... Args>
NodeHandle AddBenchmarkPass(FrameGraph& graph, EDeviceRole role, const char* name, Args&&... args)
{
    if (role == DEVICE_ROLE_PRIMARY)
    { return graph.AddPrimaryGPUPass<Pass>(name, std::forward<Args>(args)...); }
    return graph.AddSecondaryGPUPass<Pass>(name, std::forward<Args>(args)...);
}

using ConfigureCrossGPUWorkload = NodeHandle (*)(FrameGraph&, const CrossGPUStage&, bool& verified);

int RunCrossGPUBenchmark(HINSTANCE instance, const char* name, bool textures,
    std::vector<CrossGPUStage> stages, ConfigureCrossGPUWorkload configure, const char* stagePrefix);
