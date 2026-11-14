#pragma once
#include <cstdint>
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12AmbientOcclusion.h"

namespace XeGTAO { struct GTAOConstants; }
namespace Meta { class SceneAO; }
struct FFX_CACAO_D3D12Context;

class GDX12AmbientOcclusionPass final : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Ambient occlusion");
        graph.AsyncCompute();
        graph.Read(SharedResource::Scene);
        graph.ReadInputs(_inputs, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        graph.Write(_outputs[0], "Ambient occlusion", Lifetime::Dynamic, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    GDX12AmbientOcclusionPass();
    ~GDX12AmbientOcclusionPass() override;
    void Initialize() override;
    void Execute(GDX12CommandList* commands) override;
    bool IsAsyncComputeEnabled() const override { return IsAmbientOcclusionAsyncComputeEnabled(); }
    std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override;
    void Resize() override;
    void ClearDenendencies() override;

private:
    friend class Meta::SceneAO;
    friend struct GDX12AOTestAccess;

    struct ShaderConstants
    {
        float Radius, Intensity, Bias, HorizonBias;
        float FadeStart, FadeEnd, Sharpness;
        UINT Samples;
        UINT Directions, Steps, DenoisePasses, Implementation;
        float Slices, SliceSteps;
        UINT Half, Padding;
    };
    enum Pipeline : std::uint8_t
    {
        PrepareNormals, SSAO, HBAO, Blur, Resolve, XePrefilter, XeMain, XeDenoise, Neutral, PrepareHalf, Upsample, PadCACAO, PipelineCount
    };
    struct WorkGrid
    {
        GDX12Texture Depth, Normals, Scratch[2], XeDepth, XeTerms[2], XeEdges, Visibility;
        std::array<GDX12Descriptor, 5> DepthUAVs;
        GDX12Texture CacaoDepth, CacaoNormals, CacaoVisibility;
        std::vector<uint8_t> CacaoMemory;
        FFX_CACAO_D3D12Context* Cacao = nullptr;
        bool CacaoScreenReady = false;
    };
    void CreateTextures();
    void CreateGrid(WorkGrid& grid, UINT width, UINT height, bool half);
    void CreateTexture(GDX12Texture& texture, DXGI_FORMAT format, UINT width, UINT height, UINT mips = 1);
    void Bind(GDX12CommandList* commands);
    void Dispatch(GDX12CommandList* commands, Pipeline pipeline, UINT width, UINT height);
    GDX12Texture& DrawCACAO(GDX12CommandList* commands, WorkGrid& grid, GDX12Texture& depth,
        const GDX12AOSettings& settings, Matrix projection);
    void DestroyCACAO(WorkGrid& grid);

    GDX12Texture _output;
    std::array<WorkGrid, 2> _grids;
    std::array<ComPtr<ID3D12PipelineState>, PipelineCount> _pipelines;
    std::unique_ptr<GDX12RootSignature> _root;
    std::unique_ptr<GDX12UploadBuffer<ShaderConstants>> _constants;
    std::unique_ptr<GDX12UploadBuffer<XeGTAO::GTAOConstants>> _xeConstants;
    GDX12AOSettings _previous = {};
    bool _hasPrevious = false;
    UINT _frameIndex = 0;
};
