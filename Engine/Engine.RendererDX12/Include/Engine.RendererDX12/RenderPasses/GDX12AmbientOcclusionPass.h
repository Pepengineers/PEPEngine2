#pragma once
#include <cstdint>
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12AmbientOcclusion.h"

namespace XeGTAO { struct GTAOConstants; }
struct FFX_CACAO_D3D12Context;

class GDX12AmbientOcclusionPass final : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Ambient occlusion");
        graph.Read(SharedResource::Scene);
        graph.ReadInputs(_inputs, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        graph.Write(_outputs[0], "Ambient occlusion", Lifetime::Dynamic);
    }

    GDX12AmbientOcclusionPass();
    ~GDX12AmbientOcclusionPass() override;
    void Initialize() override;
    void Execute(GDX12CommandList* commands) override;
    void Resize() override;
    void ClearDenendencies() override;

private:
    struct ShaderConstants
    {
        float Radius, Intensity, Bias, HorizonBias;
        float FadeStart, FadeEnd, Sharpness;
        UINT Samples;
        UINT Directions, Steps, DenoisePasses, Implementation;
        float Slices, SliceSteps, Padding[2];
    };
    enum Pipeline : std::uint8_t { PrepareNormals, SSAO, HBAO, Blur, Resolve, XePrefilter, XeMain, XeDenoise, Neutral, PipelineCount };
    void CreateTextures();
    void CreateTexture(GDX12Texture& texture, DXGI_FORMAT format, UINT width, UINT height, UINT mips = 1);
    void Bind(GDX12CommandList* commands);
    void Dispatch(GDX12CommandList* commands, Pipeline pipeline, UINT width, UINT height);
    void DrawCACAO(GDX12CommandList* commands, const GDX12AOSettings& settings, const Matrix& projection);
    void DestroyCACAO();

    GDX12Texture _output, _normals, _scratch[2], _xeDepth, _xeTerms[2], _xeEdges;
    std::array<GDX12Descriptor, 5> _depthUAVs;
    std::array<ComPtr<ID3D12PipelineState>, PipelineCount> _pipelines;
    std::unique_ptr<GDX12RootSignature> _root;
    std::unique_ptr<GDX12UploadBuffer<ShaderConstants>> _constants;
    std::unique_ptr<GDX12UploadBuffer<XeGTAO::GTAOConstants>> _xeConstants;
    std::vector<uint8_t> _cacaoMemory;
    FFX_CACAO_D3D12Context* _cacao = nullptr;
    bool _cacaoScreenReady = false;
    GDX12AOSettings _previous = {};
    bool _hasPrevious = false;
    UINT _frameIndex = 0;
};
