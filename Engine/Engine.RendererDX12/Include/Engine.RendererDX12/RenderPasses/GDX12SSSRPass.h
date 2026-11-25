#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
struct FfxSssrContext;
namespace BistroRender { class SceneLighting; }

// Inputs: lit HDR, reverse-Z depth, world normal/roughness, F0/baked,
// unjittered prevNDC-currentNDC motion, prefiltered global environment, DFG LUT.
class GDX12SSSRPass final : public GDX12RenderPass
{
public:

    GDX12SSSRPass();
    ~GDX12SSSRPass() override;
    void SetStaticIBLOnly(bool enabled) { _staticIBLOnly = enabled; }
    void Initialize() override;
    void Resize() override;
    void ClearDenendencies() override;

private:
    friend class BistroRender::SceneLighting;
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
    bool _staticIBLOnly = false;
    void CreateContext();
    void DestroyContext();
};
