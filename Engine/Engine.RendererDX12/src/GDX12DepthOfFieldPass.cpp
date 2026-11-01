#include "Engine.RendererDX12/RenderPasses/GDX12DepthOfFieldPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12DepthOfField.h"
#include <FidelityFX/host/ffx_dof.h>
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <stdexcept>

namespace
{
    void CheckFidelityFX(FfxErrorCode result)
    {
        if (result != FFX_OK)
            throw std::runtime_error("FidelityFX DoF failed: " + std::to_string(result));
    }

    FfxResource SDKResource(GDX12Texture& texture, const wchar_t* name, FfxResourceStates state)
    {
        auto* resource = texture.GetResource()->D3DResource.Get();
        return ffxGetResourceDX12(resource, ffxGetResourceDescriptionDX12(resource), name, state);
    }
}

struct GDX12DepthOfFieldPass::Context
{
    FfxDofContext SDK = {};
    std::vector<uint8_t> Scratch;
    unsigned Rings = 0;
    float CocLimit = 0.0f;
    bool RingMerge = true;
};

GDX12DepthOfFieldPass::GDX12DepthOfFieldPass()
{
    _numInputs = 2;
    _numOutputs = 1;
    _outputs.push_back(&_output);
}

GDX12DepthOfFieldPass::~GDX12DepthOfFieldPass()
{
    DestroyContext();
}

void GDX12DepthOfFieldPass::Initialize()
{
    auto* color = _inputs[0]->GetTexture();
    auto* depth = _inputs[1]->GetTexture();
    if (color->GetFormat() != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        color->GetWidth() != depth->GetWidth() || color->GetHeight() != depth->GetHeight())
        throw std::invalid_argument("DoF requires matching HDR color and depth grids");

    GDX12RenderHelpers::InitializeComputeTexture(_output, *_resources,
        color->GetWidth(), color->GetHeight(), DXGI_FORMAT_R16G16B16A16_FLOAT);
}

void GDX12DepthOfFieldPass::DestroyContext()
{
    if (!_context) return;
    _resources->Device->GetCommandQueue()->Flush();
    ffxDofContextDestroy(&_context->SDK);
    _context.reset();
}

void GDX12DepthOfFieldPass::Execute(GDX12CommandList* commands)
{
    const auto settings = GetDepthOfFieldSettings();
    auto* color = _inputs[0]->GetTexture();
    auto* depth = _inputs[1]->GetTexture();
    // The SDK's four bilateral mip levels require at least 8 half-resolution texels.
    const bool enabled = settings.Enabled && color->GetWidth() >= 16 && color->GetHeight() >= 16;
    if (_wasEnabled != enabled) _commonData->ResetUpscalerHistory = true;
    _wasEnabled = enabled;
    GDX12_SCOPED_EVENT(commands, "FidelityFX Depth of Field", Colors::Purple);
    if (!enabled)
    {
        commands->ResourceBarrier({ color->GetResource()->GetCopySourceBarrier(), _output.GetResource()->GetCopyDestBarrier() });
        commands->CopyResource(_output.GetResource()->D3DResource.Get(), color->GetResource()->D3DResource.Get());
        return;
    }

    if (!_context || _context->Rings != settings.Rings || _context->CocLimit != settings.CocLimit ||
        _context->RingMerge != settings.RingMerge)
    {
        DestroyContext();
        auto context = std::make_unique<Context>();
        FfxDofContextDescription desc = {};
        desc.flags = FFX_DOF_REVERSE_DEPTH;
        if (!settings.RingMerge) desc.flags |= FFX_DOF_DISABLE_RING_MERGE;
        desc.quality = settings.Rings;
        desc.cocLimitFactor = settings.CocLimit;
        desc.resolution = { color->GetWidth(), color->GetHeight() };
        context->Scratch.resize(ffxGetScratchMemorySizeDX12(1));
        CheckFidelityFX(ffxGetInterfaceDX12(&desc.backendInterface, ffxGetDeviceDX12(_resources->Device->GetDevice().Get()),
            context->Scratch.data(), context->Scratch.size(), 1));
        const auto result = ffxDofContextCreate(&context->SDK, &desc);
        if (result != FFX_OK)
        {
            ffxDofContextDestroy(&context->SDK);
            CheckFidelityFX(result);
        }
        context->Rings = settings.Rings;
        context->CocLimit = settings.CocLimit;
        context->RingMerge = settings.RingMerge;
        _context = std::move(context);
        _commonData->ResetUpscalerHistory = true;
    }

    if (_resources->FrameConstants.size() > FFX_MAX_QUEUED_FRAMES)
        _resources->Device->GetCommandQueue()->Flush();

    const auto& projection = _commonData->CameraViewToClip;
    const auto coc = CalculateDepthOfFieldCoc(settings, XMConvertToRadians(_commonData->ActiveCameraFOV),
        color->GetHeight(), projection._33, projection._34, projection._43);
    if (_previousScale != coc.Scale || _previousBias != coc.Bias) _commonData->ResetUpscalerHistory = true;
    _previousScale = coc.Scale;
    _previousBias = coc.Bias;
    commands->ResourceBarrier({ color->GetResource()->GetNonPixelShaderResourceBarrier(),
        depth->GetResource()->GetNonPixelShaderResourceBarrier(), _output.GetResource()->GetUnorderedAccessBarrier() });
    FfxDofDispatchDescription dispatch = {};
    dispatch.commandList = ffxGetCommandListDX12(commands->GetCommandList().Get());
    dispatch.color = SDKResource(*color, L"DoF HDR input", FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatch.depth = SDKResource(*depth, L"DoF reverse-Z depth", FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatch.output = SDKResource(_output, L"DoF HDR output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.cocScale = coc.Scale;
    dispatch.cocBias = coc.Bias;
    const auto result = ffxDofContextDispatch(&_context->SDK, &dispatch);
    // The backend unregisters inputs to their supplied states; it also replaces heaps and PSO.
    commands->InvalidateCachedState();
    CheckFidelityFX(result);
    commands->ResourceBarrier({ _output.GetResource()->GetUAVBarrier() });
}

void GDX12DepthOfFieldPass::Resize()
{
    DestroyContext();
    auto* color = _inputs[0]->GetTexture();
    auto* depth = _inputs[1]->GetTexture();
    if (color->GetWidth() != depth->GetWidth() || color->GetHeight() != depth->GetHeight())
        throw std::invalid_argument("DoF resize requires matching color and depth grids");
    _output.Resize(color->GetWidth(), color->GetHeight());
    _commonData->ResetUpscalerHistory = true;
}

void GDX12DepthOfFieldPass::ClearDenendencies()
{
    DestroyContext();
    GDX12RenderPass::ClearDenendencies();
}
