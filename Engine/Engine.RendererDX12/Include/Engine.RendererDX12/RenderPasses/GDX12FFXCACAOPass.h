#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <ffx_cacao_impl.h>

struct FFX_CACAO_D3D12Context;

// AMD FidelityFX CACAO ambient occlusion pass
class GDX12FFXCACAOPass final : public GDX12RenderPass
{
public:
    GDX12FFXCACAOPass();
    ~GDX12FFXCACAOPass() override;
    void Describe(PassDescription& description) const override;
    void Initialize(PassInitContext& context) override;
    void Execute(PassExecutionContext& context) override;
    void OnResourcesRecreated(PassInitContext& context) override;

private:
    // Recreates only the SDK's private working textures, after FrameGraph drains
    // their GPU users. Input/output descriptors are taken directly at Execute.
    void CreateScreenResources(PassInitContext& context);
    static void Check(FFX_CACAO_Status result, const char* operation);

    // One context per pass/GPU; its upload ring covers all in-flight frame slots.
    std::vector<BYTE> _contextMemory;
    FFX_CACAO_D3D12Context* _context = nullptr;
    GDX12DeviceResources* _resources = nullptr;
    UINT _descriptorsStart = UINT_MAX;
    bool _screenResourcesReady = false;
};
