#include "Engine.RendererDX12/RenderPasses/GDX12SSSRPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12SSRSettings.h"
#include <FidelityFX/host/ffx_sssr.h>
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <algorithm>
#include <stdexcept>

namespace
{
    void CheckFFX(const FfxErrorCode result)
    {
        if (result != FFX_OK)
            throw std::runtime_error("FidelityFX SSSR failed: " + std::to_string(result));
    }

}

GDX12SSSRPass::GDX12SSSRPass()
{
    _flags = RENDER_PASS_FLAG_USE_CAMERAS;
    _numInputs = 7;
    _numOutputs = 1;
    _outputs.push_back(&_color);
}

GDX12SSSRPass::~GDX12SSSRPass()
{
    DestroyContext();
}

void GDX12SSSRPass::Initialize()
{
    for (auto [texture, format] : { std::pair{ &_color, DXGI_FORMAT_R16G16B16A16_FLOAT },
        { &_fallback, DXGI_FORMAT_R16G16B16A16_FLOAT }, { &_radiance, DXGI_FORMAT_R16G16B16A16_FLOAT }, { &_roughness, DXGI_FORMAT_R16_FLOAT },
        { &_motion, DXGI_FORMAT_R16G16_FLOAT } })
    {
        GDX12RenderHelpers::InitializeComputeTexture(*texture, *_resources,
            _inputs[0]->GetTexture()->GetWidth(), _inputs[0]->GetTexture()->GetHeight(), format, texture == &_color);
    }
    GDX12RootSignatureDesc root;
    root.NumSingleCBVSlots = 2;
    root.NumSingleSRVSlots = 11;
    root.NumSingleUAVSlots = 4;
    root.StaticSamplers = GetStaticSamplers();
    _root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
    const D3D_SHADER_MACRO defines[] = {{"SSSR_STATIC_IBL_ONLY", _staticIBLOnly ? "1" : "0"}, {nullptr, nullptr}};
    for (auto [pipeline, entry] : { std::pair{ std::addressof(_prepare), "Prepare" }, { std::addressof(_resolve), "Resolve" } })
    {
        *pipeline = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _root.get(),
            SHADERS_FOLDER "SSSR.hlsl", entry, defines);
    }
    _constants = std::make_unique<GDX12UploadBuffer<Constants>>(_resources->Device,
        static_cast<UINT>(_resources->FrameConstants.size()));
}

void GDX12SSSRPass::DestroyContext()
{
    if (_context)
    {
        _resources->Device->GetCommandQueue()->Flush();
        ffxSssrContextDestroy(_context.get());
        _context.reset();
    }
    _scratch.clear();
}

void GDX12SSSRPass::CreateContext()
{
    DestroyContext();
    const size_t bytes = ffxGetScratchMemorySizeDX12(FFX_SSSR_CONTEXT_COUNT);
    _scratch.assign((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0);
    FfxSssrContextDescription desc = {};
    desc.flags = FFX_SSSR_ENABLE_DEPTH_INVERTED;
    desc.renderSize = { _color.GetWidth(), _color.GetHeight() };
    desc.normalsHistoryBufferFormat = FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    CheckFFX(ffxGetInterfaceDX12(&desc.backendInterface,
        ffxGetDeviceDX12(_resources->Device->GetDevice().Get()), _scratch.data(), bytes, FFX_SSSR_CONTEXT_COUNT));
    auto context = std::make_unique<FfxSssrContext>();
    CheckFFX(ffxSssrContextCreate(context.get(), &desc));
    _context = std::move(context);
}

void GDX12SSSRPass::Resize()
{
    DestroyContext();
    for (auto* texture : { &_color, &_radiance, &_roughness, &_motion, &_fallback })
        texture->Resize(_inputs[0]->GetTexture()->GetWidth(), _inputs[0]->GetTexture()->GetHeight());
}

void GDX12SSSRPass::ClearDenendencies()
{
    DestroyContext();
    GDX12RenderPass::ClearDenendencies();
    _constants.reset();
    _root.reset();
    _prepare.Reset();
    _resolve.Reset();
}
