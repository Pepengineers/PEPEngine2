#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <ngx/nvsdk_ngx_defs.h>

class GDX12DLSSUpscalePass final : public GDX12RenderPass
{
public:
    explicit GDX12DLSSUpscalePass(NVSDK_NGX_PerfQuality_Value quality = NVSDK_NGX_PerfQuality_Value_MaxQuality);
    ~GDX12DLSSUpscalePass() override;

    void Setup(GDX12DeviceResources* resources, RenderPipelineCommonData* common) override;
    void QueryRenderTargetResolution() override;
    void Initialize() override;
    void Resize() override;
    void Execute(GDX12CommandList* commands) override;
    void ClearDenendencies() override;

private:
    friend class GDX12StreamlineSDK;
    static void ShutdownStreamlineDevice(ID3D12Device* device);

    void CreateFeature();
    void ReleaseFeature();
    void ReleaseNGX();

    const NVSDK_NGX_PerfQuality_Value _quality;
    GDX12Texture _output;
    ID3D12Device* _device = nullptr;
    NVSDK_NGX_Parameter* _parameters = nullptr;
    NVSDK_NGX_Handle* _feature = nullptr;
    uint64_t _historyRevision = UINT64_MAX;
};
