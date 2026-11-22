#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12RenderGeometrySource.h"
#include "Engine.Core/Jobs/JobSystem.h"
#include <utility>
#include <functional>

class RenderModule;
struct DeviceDesc;
class GDX12SwapChain;
class GDX12ReconstructionPass;
struct RenderPipelineContext
{
    RenderModule& Runtime;
    GDX12DeviceResources& Primary;
    RenderPipelineCommonData& Common;
    GDX12SwapChain* BackBuffer;

    void Resize();
    void SetPresentationDevice(GDX12Device&);
};

struct RenderPipelineBindings
{
    std::vector<GDX12RenderPass*> PrimaryPasses;
    GDX12ReconstructionPass* Reconstruction = nullptr;
};

class IRenderPipeline
{
public:
    virtual ~IRenderPipeline() = default;
    virtual void Initialize(RenderPipelineContext&, const std::vector<DeviceDesc>&) = 0;
    virtual void PrepareMaterialBindings(RenderPipelineContext&) {}
    virtual void ResetWorldHistory() {}
    virtual std::pair<size_t, size_t> GetWorldProbeReadiness() const { return {}; }
    // Before frame constants/camera uploads, so a changed render grid applies to the same frame.
    virtual void UpdateFrameSettings(RenderPipelineContext&) {}
    virtual void PrepareFrame(RenderPipelineContext&) = 0;
    virtual void RebuildExecution(RenderPipelineContext&) {}
    virtual void DrainExecution() {}
    virtual uint64_t ExecuteFrame(RenderPipelineContext&, GDX12CommandList*,
        const std::function<void(GDX12CommandList*)>&) = 0;
    virtual void FramePresented() {}
    virtual void WriteExecution(std::ostream&) const {}
    virtual void Resize(RenderPipelineContext&) = 0;
    virtual const RenderPipelineBindings& GetBindings() const = 0;
};
