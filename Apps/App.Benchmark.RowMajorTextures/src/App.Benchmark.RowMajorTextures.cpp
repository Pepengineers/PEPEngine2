#include "../../CrossGPUBenchmark/CrossGPUBenchmark.h"
#include "../SyntheticTexturePasses.h"

namespace
{
    NodeHandle Configure(FrameGraph& graph, const CrossGPUStage& stage, bool& verified)
    {
        const auto write = AddBenchmarkPass<SyntheticTextureWritePass>(graph, stage.Producer, "Write", stage.Size);
        const auto read = AddBenchmarkPass<SyntheticTextureReadPass>(graph, ConsumerRole(stage), "Read",
            stage.Size, &verified);
        const auto output = write.GetOutput("Data");
        const auto input = read.GetInput("Data");
        output.SetStorageType(stage.Storage);
        input.SetAccessType(stage.Access);
        graph.ConnectNodes(output, input);
        return read;
    }
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, PSTR stagePrefix, int)
{
    std::vector<CrossGPUStage> stages;
    for (const auto extent : {2048u, 4096u})
    {
        const auto workload = std::to_string(extent) + "x" + std::to_string(extent);
        AddCrossGPUStages(stages, DEVICE_ROLE_PRIMARY, extent, 1, workload);
        AddCrossGPUStages(stages, DEVICE_ROLE_SECONDARY, extent, 1, workload);
    }
    return RunCrossGPUBenchmark(instance, "row-major-texture-benchmark", true, std::move(stages), Configure, stagePrefix);
}
