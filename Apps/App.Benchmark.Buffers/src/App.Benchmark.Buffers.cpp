#include "../../CrossGPUBenchmark/CrossGPUBenchmark.h"
#include "../SyntheticBufferPasses.h"

namespace
{
    NodeHandle Configure(FrameGraph& graph, const CrossGPUStage& stage, bool& verified)
    {
        const auto write = AddBenchmarkPass<SyntheticBufferWritePass>(graph, stage.Producer, "Write", stage.Size);
        const auto read = AddBenchmarkPass<SyntheticBufferReadPass>(graph, ConsumerRole(stage), "Read",
            stage.Size, stage.Reads, &verified);
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
    AddCrossGPUStages(stages, DEVICE_ROLE_PRIMARY, 128, 1, "128MiB_x1");
    AddCrossGPUStages(stages, DEVICE_ROLE_SECONDARY, 32, 4, "32MiB_x4");
    AddCrossGPUStages(stages, DEVICE_ROLE_PRIMARY, 32, 1, "32MiB_x1");
    return RunCrossGPUBenchmark(instance, "buffer-benchmark", false, std::move(stages), Configure, stagePrefix);
}
