#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12SSRSettings.h"
struct FfxSssrContext;

// Inputs: lit HDR, reverse-Z depth, world normal/roughness, F0/baked,
// unjittered prevNDC-currentNDC motion, prefiltered global environment, DFG LUT.
class GDX12SSSRPass final : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Screen-space reflections");
        graph.Read(SharedResource::Scene);
        for (UINT i = 0; i < _inputs.size(); ++i)
            graph.Read(_inputs[i], i == 1 ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : GDX12RenderGraph::UnmanagedState);
        graph.Write(_outputs[0], "SSR HDR");
    }

    GDX12SSSRPass();
    ~GDX12SSSRPass() override;
    void Initialize() override;
    void Execute(GDX12CommandList* commands) override;
    void Resize() override;
    void ClearDenendencies() override;

private:
    struct Constants
    {
        XMFLOAT2 JitterDeltaUV;
        float RoughnessCutoff;
        float Padding = 0;
    };
    GDX12Texture _color, _radiance, _roughness, _motion, _fallback;
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _prepare, _resolve;
    std::unique_ptr<GDX12UploadBuffer<Constants>> _constants;
    std::unique_ptr<FfxSssrContext> _context;
    std::vector<uint64_t> _scratch;
    Matrix _previousViewProjection = Identity4x4();
    XMFLOAT2 _previousJitter = {};
    uint64_t _historyRevision = UINT64_MAX;
    uint64_t _environmentRevision = UINT64_MAX;
    std::array<ID3D12Resource*, 7> _sources = {};
    GDX12SSRSettings _previousSettings = {};
    void CreateContext();
    void DestroyContext();
};
