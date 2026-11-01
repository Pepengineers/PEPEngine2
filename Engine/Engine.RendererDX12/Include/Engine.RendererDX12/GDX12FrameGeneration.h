#pragma once
#include "Engine.RendererDX12/D3DHelpers.h"
#include <cstdint>
#include <string>

class GDX12Device;
class GDX12CommandList;
class GDX12Texture;
class RenderPipelineCommonData;

enum class GDX12FrameGenerationProvider { Off, AMD, NVIDIA, Intel };
GDX12FrameGenerationProvider GetFrameGenerationProvider();

struct GDX12FrameGenerationStatus
{
    GDX12FrameGenerationProvider Requested = GDX12FrameGenerationProvider::Off;
    GDX12FrameGenerationProvider Effective = GDX12FrameGenerationProvider::Off;
    std::string FallbackReason;
    uint64_t Generation = 0;
    uint64_t GeneratedFrames = 0;
    bool Suspended = false;
};

class GDX12FrameGeneration final
{
public:
    explicit GDX12FrameGeneration(GDX12Device*);
    ~GDX12FrameGeneration();
    bool NeedsRecreate() const;
    ComPtr<IDXGISwapChain4> CreateSwapChain(HWND, DXGI_SWAP_CHAIN_DESC1&, bool applySettings);
    void DestroySwapChain(ComPtr<IDXGISwapChain4>& proxy, ComPtr<IDXGISwapChain4>& native);
    void WaitForIdle();
    void SetSuspended(bool);
    void BeginFrame(RenderPipelineCommonData&);
    void BeginRendering(RenderPipelineCommonData&);
    void Prepare(GDX12CommandList*, GDX12Texture* depth, GDX12Texture* motion, RenderPipelineCommonData&);
    void EndRendering();
    void BeforePresent();
    void AfterPresent();
    const GDX12FrameGenerationStatus& GetStatus() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
