#include "Engine.RendererDX12/RenderPasses/GDX12MotionBlurPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12MotionBlurSettings.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

GDX12MotionBlurPass::GDX12MotionBlurPass()
{
    _flags = RENDER_PASS_FLAG_USE_CAMERAS;
    _numInputs = 3;
    _numOutputs = 2;
    _color = std::make_unique<GDX12Texture>();
    _motion = std::make_unique<GDX12Texture>();
    _velocity = std::make_unique<GDX12Texture>();
    _tileMax = std::make_unique<GDX12Texture>();
    _neighborMax = std::make_unique<GDX12Texture>();
    _outputs = { _color.get(), _motion.get() };
}

void GDX12MotionBlurPass::Initialize()
{
    auto* color = _inputs[0]->GetTexture();
    if (color->GetFormat() != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        color->GetWidth() != _inputs[1]->GetTexture()->GetWidth() ||
        color->GetHeight() != _inputs[1]->GetTexture()->GetHeight() ||
        color->GetWidth() != _inputs[2]->GetTexture()->GetWidth() ||
        color->GetHeight() != _inputs[2]->GetTexture()->GetHeight())
        throw std::invalid_argument("Motion blur requires matching HDR/depth/motion grids");

    for (auto* target : { _color.get(), _motion.get(), _velocity.get(), _tileMax.get(), _neighborMax.get() })
    {
        if (_passthroughWhenDisabled) continue;
        const bool tile = target == _tileMax.get() || target == _neighborMax.get();
        GDX12RenderHelpers::InitializeComputeTexture(*target, *_resources,
            tile ? 1 : color->GetWidth(), tile ? 1 : color->GetHeight(),
            target == _color.get() ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R16G16_FLOAT);
    }
    if (!_passthroughWhenDisabled)
    {
        _color->GetResource()->D3DResource->SetName(L"MotionBlur.Color");
        _motion->GetResource()->D3DResource->SetName(L"MotionBlur.CompleteMotion");
        _velocity->GetResource()->D3DResource->SetName(L"MotionBlur.Velocity");
        _tileMax->GetResource()->D3DResource->SetName(L"MotionBlur.TileMax");
        _neighborMax->GetResource()->D3DResource->SetName(L"MotionBlur.NeighborMax");
    }
    GDX12RootSignatureDesc root;
    root.Constants = { 16 };
    root.NumSingleCBVSlots = 1;
    root.NumSingleSRVSlots = 7;
    root.NumSingleUAVSlots = 5;
    _root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
    _prepare = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _root.get(), SHADERS_FOLDER "MotionBlur.hlsl", "Prepare");
    _tiles = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _root.get(), SHADERS_FOLDER "MotionBlur.hlsl", "Tiles");
    _neighbors = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _root.get(), SHADERS_FOLDER "MotionBlur.hlsl", "Neighbors");
    _gather = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _root.get(), SHADERS_FOLDER "MotionBlur.hlsl", "Gather");
}

void GDX12MotionBlurPass::ResizeTiles(const UINT tileSize)
{
    auto* input = _inputs[0]->GetTexture();
    const UINT width = (input->GetWidth() + tileSize - 1) / tileSize;
    const UINT height = (input->GetHeight() + tileSize - 1) / tileSize;
    if (!_tileMax->IsInitialized())
    {
        GDX12RenderHelpers::InitializeComputeTexture(*_tileMax, *_resources, width, height, DXGI_FORMAT_R16G16_FLOAT);
        GDX12RenderHelpers::InitializeComputeTexture(*_neighborMax, *_resources, width, height, DXGI_FORMAT_R16G16_FLOAT);
        _tileMax->GetResource()->D3DResource->SetName(L"MotionBlur.TileMax");
        _neighborMax->GetResource()->D3DResource->SetName(L"MotionBlur.NeighborMax");
    }
    if (_tileMax->GetWidth() != width || _tileMax->GetHeight() != height)
    {
        // Runtime radius changes can replace descriptors still used by earlier frames.
        if (!_passthroughWhenDisabled) _resources->Device->GetCommandQueue()->Flush();
        _tileMax->Resize(width, height);
        _neighborMax->Resize(width, height);
    }
    _tileSize = tileSize;
}

void GDX12MotionBlurPass::Execute(GDX12CommandList* commands)
{
    PrepareRecording();
    RecordCommands(commands);
}

bool GDX12MotionBlurPass::NeedsTileResize() const
{
    if (_inputs.empty() || !_inputs[0] || !_tileMax->IsInitialized()) return false;
    const auto settings = GetMotionBlurSettings();
    if (!settings.Enabled) return false;
    auto* input = _inputs[0]->GetTexture();
    const float scale = (std::max)(float(input->GetWidth()) / _commonData->WindowWidth,
        float(input->GetHeight()) / _commonData->WindowHeight);
    const UINT tileSize = (std::max)(8u, UINT(std::ceil(settings.MaxRadius * scale)));
    return _tileMax->GetWidth() != (input->GetWidth() + tileSize - 1) / tileSize ||
        _tileMax->GetHeight() != (input->GetHeight() + tileSize - 1) / tileSize;
}

void GDX12MotionBlurPass::PrepareRecording(bool completeMotionRequired)
{
    const auto settings = GetMotionBlurSettings();
    auto* input = _inputs[0]->GetTexture();
    const float scale = (std::max)(float(input->GetWidth()) / _commonData->WindowWidth,
                                  float(input->GetHeight()) / _commonData->WindowHeight);
    const UINT tileSize = (std::max)(8u, UINT(std::ceil(settings.MaxRadius * scale)));
    if (_passthroughWhenDisabled)
    {
        _commonData->ResetHistory |= settings.Enabled != _wasEnabled;
        _preparedBlur = settings.Enabled && _wasEnabled && !_commonData->ResetHistory;
        _prepareMotion = settings.Enabled || completeMotionRequired;
        _wasEnabled = settings.Enabled;
        const auto initialize = [&](GDX12Texture& target, DXGI_FORMAT format) {
            if (!target.IsInitialized())
            {
                GDX12RenderHelpers::InitializeComputeTexture(target, *_resources,
                    input->GetWidth(), input->GetHeight(), format);
                target.GetResource()->D3DResource->SetName(&target == _color.get() ? L"MotionBlur.Color" :
                    &target == _motion.get() ? L"MotionBlur.CompleteMotion" : L"MotionBlur.Velocity");
            }
        };
        if (_prepareMotion)
        {
            initialize(*_motion, DXGI_FORMAT_R16G16_FLOAT);
            initialize(*_velocity, DXGI_FORMAT_R16G16_FLOAT);
        }
        if (_preparedBlur)
        {
            initialize(*_color, DXGI_FORMAT_R16G16B16A16_FLOAT);
            if (_tileSize != tileSize) ResizeTiles(tileSize);
        }
        _outputs[0] = _preparedBlur ? static_cast<IRenderPassLink*>(_color.get()) : _inputs[0];
        _outputs[1] = _prepareMotion ? static_cast<IRenderPassLink*>(_motion.get()) : _inputs[2];
        return;
    }
    if (settings.Enabled && _wasEnabled && !_commonData->ResetHistory && _tileSize != tileSize)
        ResizeTiles(tileSize);
}

void GDX12MotionBlurPass::RecordCommands(GDX12CommandList* commands)
{
    RecordCommands(commands, _resources->ParticleMotion);
}

void GDX12MotionBlurPass::RecordCommands(GDX12CommandList* commands, GDX12Texture* featureMotion)
{
    if (_passthroughWhenDisabled && !_prepareMotion) return;
    const auto settings = GetMotionBlurSettings();
    const UINT width = _inputs[0]->GetTexture()->GetWidth();
    const UINT height = _inputs[0]->GetTexture()->GetHeight();
    const float scale = (std::max)(static_cast<float>(width) / _commonData->WindowWidth,
        static_cast<float>(height) / _commonData->WindowHeight);
    const UINT tileSize = (std::max)(8u, static_cast<UINT>(std::ceil(settings.MaxRadius * scale)));
    if (!_passthroughWhenDisabled) _commonData->ResetHistory |= settings.Enabled != _wasEnabled;
    const bool blur = _passthroughWhenDisabled ? _preparedBlur : settings.Enabled && _wasEnabled && !_commonData->ResetHistory;
    if (blur && _tileSize != tileSize) throw std::logic_error("Motion blur recording requires owner tile preparation");
    _wasEnabled = settings.Enabled;
    struct Parameters
    {
        UINT Width, Height, TileSize, Samples;
        float Shutter, Radius;
        UINT Blur, Reset;
        float OutputWidth, OutputHeight, CenterWeight, VelocityThreshold;
        UINT FeatureMotion, Padding[3];
    };
    static_assert(sizeof(Parameters) == 16 * sizeof(UINT));
    if (featureMotion && (featureMotion->GetWidth() != width || featureMotion->GetHeight() != height))
        throw std::invalid_argument("Particle motion must match the motion blur input grid");
    const Parameters parameters = { width, height, tileSize, settings.Samples,
        settings.ShutterFraction, settings.MaxRadius, blur, _commonData->ResetHistory,
        static_cast<float>(_commonData->WindowWidth), static_cast<float>(_commonData->WindowHeight),
        settings.CenterWeight, settings.VelocityThreshold, featureMotion != nullptr, {} };
    GDX12_SCOPED_EVENT(commands, "Feature-aware motion blur", Colors::Coral);
    commands->SetComputeRootSignature(_root.get());
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    commands->GetCommandList()->SetComputeRoot32BitConstants(0, 16, &parameters, 0);
    auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    commands->SetComputeRootConstantBufferView(1, frame->CameraCB->GetElementAddress(_commonData->ActiveCameraCBufferIndex));
    GDX12Texture* inputs[] = { _inputs[0]->GetTexture(), _inputs[1]->GetTexture(), _inputs[2]->GetTexture() };
    for (UINT i = 0; i < 3; ++i) commands->ResourceBarrier({ inputs[i]->GetResource()->GetNonPixelShaderResourceBarrier() });
    if (featureMotion) commands->ResourceBarrier({ featureMotion->GetResource()->GetNonPixelShaderResourceBarrier() });
    commands->SetComputeSRV(6, (featureMotion ? featureMotion : inputs[0])->GetSRV()->GPUHandle);
    commands->ResourceBarrier({ _motion->GetResource()->GetUnorderedAccessBarrier(),
        _velocity->GetResource()->GetUnorderedAccessBarrier() });
    commands->SetComputeSRV(1, inputs[1]->GetSRV()->GPUHandle);
    commands->SetComputeSRV(2, inputs[2]->GetSRV()->GPUHandle);
    commands->SetComputeUAV(1, _motion->GetUAV()->GPUHandle);
    commands->SetComputeUAV(2, _velocity->GetUAV()->GPUHandle);
    commands->SetPipelineState(_prepare.Get());
    commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    commands->ResourceBarrier({ _motion->GetResource()->GetNonPixelShaderResourceBarrier(),
        _velocity->GetResource()->GetNonPixelShaderResourceBarrier() });
    if (blur)
    {
        commands->ResourceBarrier({ _tileMax->GetResource()->GetUnorderedAccessBarrier() });
        commands->SetComputeSRV(3, _velocity->GetSRV()->GPUHandle);
        commands->SetComputeUAV(3, _tileMax->GetUAV()->GPUHandle);
        commands->SetPipelineState(_tiles.Get());
        commands->Dispatch(_tileMax->GetWidth(), _tileMax->GetHeight(), 1);
        commands->ResourceBarrier({ _tileMax->GetResource()->GetNonPixelShaderResourceBarrier(),
            _neighborMax->GetResource()->GetUnorderedAccessBarrier() });
        commands->SetComputeSRV(4, _tileMax->GetSRV()->GPUHandle);
        commands->SetComputeUAV(4, _neighborMax->GetUAV()->GPUHandle);
        commands->SetPipelineState(_neighbors.Get());
        commands->Dispatch((_tileMax->GetWidth() + 7) / 8, (_tileMax->GetHeight() + 7) / 8, 1);
        commands->ResourceBarrier({ _neighborMax->GetResource()->GetNonPixelShaderResourceBarrier(),
            _color->GetResource()->GetUnorderedAccessBarrier() });
        commands->SetComputeSRV(0, inputs[0]->GetSRV()->GPUHandle);
        commands->SetComputeSRV(5, _neighborMax->GetSRV()->GPUHandle);
        commands->SetComputeUAV(0, _color->GetUAV()->GPUHandle);
        commands->SetPipelineState(_gather.Get());
        commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    }
    else if (!_passthroughWhenDisabled)
    {
        commands->ResourceBarrier({ inputs[0]->GetResource()->GetCopySourceBarrier(),
            _color->GetResource()->GetCopyDestBarrier() });
        commands->CopyResource(_color->GetResource()->D3DResource.Get(), inputs[0]->GetResource()->D3DResource.Get());
    }
    if (blur || !_passthroughWhenDisabled) commands->ResourceBarrier({ _color->GetResource()->GetSRVBarrier() });
}

void GDX12MotionBlurPass::Resize()
{
    auto* input = _inputs[0]->GetTexture();
    for (auto* target : { _color.get(), _motion.get(), _velocity.get() })
        if (target->IsInitialized()) target->Resize(input->GetWidth(), input->GetHeight());
    _tileSize = 0;
    _wasEnabled = false;
}

void GDX12MotionBlurPass::ClearDenendencies()
{
    GDX12RenderPass::ClearDenendencies();
    _color.reset();
    _motion.reset();
    _velocity.reset();
    _tileMax.reset();
    _neighborMax.reset();
    _root.reset();
    _prepare.Reset();
    _tiles.Reset();
    _neighbors.Reset();
    _gather.Reset();
}
