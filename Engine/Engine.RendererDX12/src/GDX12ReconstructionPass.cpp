#include "Engine.RendererDX12/RenderPasses/GDX12ReconstructionPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12XeSSUpscalePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12FSRUpscalePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12DLSSUpscalePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12NISUpscalePass.h"
#include "Engine.Core/Logging/Logger.h"
#include <stdexcept>

namespace
{
    std::unique_ptr<GDX12RenderPass> CreateProvider(const GDX12ReconstructionSettings& s)
    {
        const auto q = s.Quality;
        switch (s.Provider)
        {
        case GDX12UpscalerProvider::Native: return {};
        case GDX12UpscalerProvider::XeSS:
        {
            const xess_quality_settings_t modes[] = { XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS,
                XESS_QUALITY_SETTING_AA, XESS_QUALITY_SETTING_QUALITY, XESS_QUALITY_SETTING_BALANCED,
                XESS_QUALITY_SETTING_PERFORMANCE, XESS_QUALITY_SETTING_ULTRA_PERFORMANCE,
                XESS_QUALITY_SETTING_ULTRA_QUALITY, XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS };
            return std::make_unique<GDX12XeSSUpscalePass>(modes[static_cast<int>(q)]);
        }
        case GDX12UpscalerProvider::FSR:
        {
            if (q > GDX12UpscalerQuality::UltraPerformance) throw std::invalid_argument("FSR does not expose UltraQuality/UltraQualityPlus");
            const FfxApiUpscaleQualityMode modes[] = { FFX_UPSCALE_QUALITY_MODE_QUALITY,
                FFX_UPSCALE_QUALITY_MODE_NATIVEAA, FFX_UPSCALE_QUALITY_MODE_QUALITY, FFX_UPSCALE_QUALITY_MODE_BALANCED,
                FFX_UPSCALE_QUALITY_MODE_PERFORMANCE, FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE };
            return std::make_unique<GDX12FSRUpscalePass>(modes[static_cast<int>(q)]);
        }
        case GDX12UpscalerProvider::DLSS:
        {
            if (q == GDX12UpscalerQuality::UltraQualityPlus) throw std::invalid_argument("DLSS does not expose UltraQualityPlus");
            const sl::DLSSMode modes[] = { sl::DLSSMode::eMaxQuality, sl::DLSSMode::eDLAA,
                sl::DLSSMode::eMaxQuality, sl::DLSSMode::eBalanced, sl::DLSSMode::eMaxPerformance,
                sl::DLSSMode::eUltraPerformance, sl::DLSSMode::eUltraQuality };
            return std::make_unique<GDX12DLSSUpscalePass>(modes[static_cast<int>(q)]);
        }
        case GDX12UpscalerProvider::NIS:
        {
            if (q == GDX12UpscalerQuality::NativeAA || q >= GDX12UpscalerQuality::UltraPerformance)
                throw std::invalid_argument("NIS supports Default/Quality/Balanced/Performance scaling without temporal AA");
            const float ratios[] = { 1.5f, 1.0f, 1.5f, 1.7f, 2.0f };
            return std::make_unique<GDX12NISUpscalePass>(ratios[static_cast<int>(q)]);
        }
        }
        throw std::invalid_argument("Unknown upscaler provider");
    }
}

GDX12ReconstructionPass::GDX12ReconstructionPass()
{
    _flags = RENDER_PASS_FLAG_UPSCALER;
    _numInputs = 3;
    _numOutputs = 1;
    _outputs.push_back(&_output);
}

GDX12ReconstructionPass::~GDX12ReconstructionPass() { ClearDenendencies(); }

void GDX12ReconstructionPass::Setup(GDX12DeviceResources* resources, RenderPipelineCommonData* common)
{
    GDX12RenderPass::Setup(resources, common);
    Configure();
}

void GDX12ReconstructionPass::Configure()
{
    _output.Target = nullptr;
    if (_provider) _provider->ClearDenendencies();
    _provider.reset();
    _status.Requested = GetReconstructionSettings();
    _status.Effective = GDX12UpscalerProvider::Native;
    _status.FallbackReason.clear();
    try
    {
        _provider = CreateProvider(_status.Requested);
        if (_provider) _provider->Setup(_resources, _commonData);
        if (_provider && (!_commonData->DownscaledWidth || !_commonData->DownscaledHeight))
            throw std::runtime_error("SDK returned an empty render grid");
        _status.Effective = _status.Requested.Provider;
    }
    catch (const std::exception& e)
    {
        if (_provider) _provider->ClearDenendencies();
        _provider.reset();
        _status.FallbackReason = e.what();
        Log::Warning("Reconstruction", "SR fallback to native: ", _status.FallbackReason);
    }
    if (!_provider)
    {
        _commonData->DownscaledWidth = _commonData->WindowWidth;
        _commonData->DownscaledHeight = _commonData->WindowHeight;
    }
    _flags = RENDER_PASS_FLAG_UPSCALER | (_provider ? _provider->GetFlags() : 0);
    _commonData->Upscaler = this;
    _commonData->RequestHistoryReset = _commonData->ResetUpscalerHistory = true;
    _needsInitialize = true;
    ++_status.Generation;
}

bool GDX12ReconstructionPass::ApplySettings()
{
    if (GetReconstructionSettings() == _status.Requested) return false;
    _resources->Device->GetCommandQueue()->Flush();
    Configure();
    return true;
}

void GDX12ReconstructionPass::LinkOutput()
{
    _output.Target = _provider ? _provider->GetOutput(0) : _inputs[2];
    _commonData->Upscaler = this;
}

void GDX12ReconstructionPass::Initialize()
{
    if (_provider)
    {
        _provider->SetInputs(_inputs);
        _provider->Initialize();
    }
    LinkOutput();
    _needsInitialize = false;
}

void GDX12ReconstructionPass::Execute(GDX12CommandList* commands)
{
    if (_provider) _provider->Execute(commands);
    _commonData->ResetUpscalerHistory = false;
    _commonData->ResetHistory = false;
}

void GDX12ReconstructionPass::QueryRenderTargetResolution()
{
    if (_provider) _provider->QueryRenderTargetResolution();
    else
    {
        _commonData->DownscaledWidth = _commonData->WindowWidth;
        _commonData->DownscaledHeight = _commonData->WindowHeight;
    }
}

void GDX12ReconstructionPass::Resize()
{
    _commonData->RequestHistoryReset = _commonData->ResetUpscalerHistory = true;
    if (_needsInitialize) Initialize();
    else
    {
        if (_provider) _provider->Resize();
        LinkOutput();
    }
}

void GDX12ReconstructionPass::ClearDenendencies()
{
    _output.Target = nullptr;
    if (_provider) _provider->ClearDenendencies();
    _provider.reset();
    GDX12RenderPass::ClearDenendencies();
}
