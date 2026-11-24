#pragma once
#include <cstdint>
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12AmbientOcclusion.h"

namespace XeGTAO { struct GTAOConstants; }
namespace BistroRender { class SceneAO; }
struct FFX_CACAO_D3D12Context;

class GDX12AmbientOcclusionPass final : public GDX12RenderPass
{
public:

    GDX12AmbientOcclusionPass();
    ~GDX12AmbientOcclusionPass() override;
    void Initialize() override;
    void Resize() override;
    void ClearDenendencies() override;

private:
    friend class BistroRender::SceneAO;
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
        PrepareNormals, SSAO, HBAO, Blur, Resolve, XePrefilter, XeMain, XeDenoise, Neutral, PrepareHalf, Upsample,
        PadCACAO, Reproject, PackFull, ReprojectXe, ResolveXe, PipelineCount
    };
public:
    struct Pipelines
    {
        explicit Pipelines(GDX12Device* device);
        std::unique_ptr<GDX12RootSignature> Root;
        std::array<ComPtr<ID3D12PipelineState>, PipelineCount> States;
    };

private:
    enum GridResource : UINT
    {
        HalfDepth = 1u << 0, HalfVisibility = 1u << 1, VisibilityScratch = 1u << 2,
        FilterScratch = 1u << 3, XePyramid = 1u << 4, XeTerm = 1u << 5, XeFilter = 1u << 6,
        XeEdge = 1u << 7, PaddedInputs = 1u << 8, PaddedVisibility = 1u << 9
    };
    struct WorkGrid
    {
        UINT Width, Height, Resources;
        GDX12Texture Depth, Normals, Scratch[2], XeDepth, XeTerms[2], XeEdges, Visibility;
        std::array<GDX12Descriptor, 5> DepthUAVs;
        GDX12Texture CacaoDepth, CacaoNormals, CacaoVisibility;
        std::vector<uint8_t> CacaoMemory;
        FFX_CACAO_D3D12Context* Cacao = nullptr;
        GDX12ResourceHandle CacaoScreenDepth;
    };
    bool SetOutputSize(UINT width, UINT height);
    void PrepareGrid(UINT width, UINT height, UINT resources);
    void ResetGrid();
    void CreateTexture(GDX12Texture& texture, DXGI_FORMAT format, UINT width, UINT height, UINT mips = 1);
    void DestroyCACAO(WorkGrid& grid);

    GDX12Texture _output;
    std::unique_ptr<WorkGrid> _grid;
    std::unique_ptr<GDX12UploadBuffer<ShaderConstants>> _constants;
    std::unique_ptr<GDX12UploadBuffer<XeGTAO::GTAOConstants>> _xeConstants;
    GDX12AOSettings _previous = {};
    bool _hasPrevious = false;
};
