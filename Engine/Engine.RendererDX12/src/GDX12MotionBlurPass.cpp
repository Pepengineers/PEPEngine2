#include "Engine.RendererDX12/RenderPasses/GDX12MotionBlurPass.h"
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

    GDX12TextureDesc texture;
    texture.Width = color->GetWidth();
    texture.Height = color->GetHeight();
    texture.CreateUAV = true;
    texture.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
    texture.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    texture.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    texture.SRVDesc.Texture2D.MipLevels = 1;
    texture.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    for (auto* target : { _color.get(), _motion.get(), _velocity.get(), _tileMax.get(), _neighborMax.get() })
    {
        texture.Format = texture.SRVDesc.Format = texture.UAVDesc.Format =
            target == _color.get() ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R16G16_FLOAT;
        texture.SRVHeapIndex = texture.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        texture.UAVHeapIndex = texture.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        if (target == _tileMax.get() || target == _neighborMax.get())
        {
            texture.Width = texture.Height = 1;
        }
        target->Initialize(texture);
    }
    _color->GetResource()->D3DResource->SetName(L"MotionBlur.Color");
    _motion->GetResource()->D3DResource->SetName(L"MotionBlur.CompleteMotion");
    _velocity->GetResource()->D3DResource->SetName(L"MotionBlur.Velocity");
    _tileMax->GetResource()->D3DResource->SetName(L"MotionBlur.TileMax");
    _neighborMax->GetResource()->D3DResource->SetName(L"MotionBlur.NeighborMax");
    GDX12RootSignatureDesc root;
    root.Constants = { 16 };
    root.NumSingleCBVSlots = 1;
    root.NumSingleSRVSlots = 7;
    root.NumSingleUAVSlots = 5;
    _root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
    const auto compile = [&](const char* entry, ComPtr<ID3D12PipelineState>& pipeline)
    {
        auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(_resources->Device,
            SHADERS_FOLDER "MotionBlur.hlsl", nullptr, entry, "cs");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = _root->GetRootSignature().Get();
        desc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
        ThrowIfFailed(_resources->Device->GetDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)));
    };
    compile("Prepare", _prepare);
    compile("Tiles", _tiles);
    compile("Neighbors", _neighbors);
    compile("Gather", _gather);
}

void GDX12MotionBlurPass::ResizeTiles(UINT tileSize)
{
    const UINT width = (_color->GetWidth() + tileSize - 1) / tileSize;
    const UINT height = (_color->GetHeight() + tileSize - 1) / tileSize;
    if (_tileMax->GetWidth() != width || _tileMax->GetHeight() != height)
    {
        // Runtime radius changes can replace descriptors still used by earlier frames.
        _resources->Device->GetCommandQueue()->Flush();
        _tileMax->Resize(width, height);
        _neighborMax->Resize(width, height);
    }
    _tileSize = tileSize;
}

void GDX12MotionBlurPass::Execute(GDX12CommandList* commands)
{
    const auto settings = GetMotionBlurSettings();
    const UINT width = _color->GetWidth();
    const UINT height = _color->GetHeight();
    const float scale = (std::max)(static_cast<float>(width) / _commonData->WindowWidth,
        static_cast<float>(height) / _commonData->WindowHeight);
    const UINT tileSize = (std::max)(8u, static_cast<UINT>(std::ceil(settings.MaxRadius * scale)));
    _commonData->ResetHistory |= settings.Enabled != _wasEnabled;
    const bool blur = settings.Enabled && _wasEnabled && !_commonData->ResetHistory;
    if (blur && _tileSize != tileSize) ResizeTiles(tileSize);
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
    auto* featureMotion = _resources->ParticleMotion;
    if (featureMotion && (featureMotion->GetWidth() != width || featureMotion->GetHeight() != height))
        throw std::invalid_argument("Particle motion must match the motion blur input grid");
    const Parameters parameters = { width, height, tileSize, settings.Samples,
        settings.ShutterFraction, settings.MaxRadius, blur, _commonData->ResetHistory,
        static_cast<float>(_commonData->WindowWidth), static_cast<float>(_commonData->WindowHeight),
        settings.CenterWeight, settings.VelocityThreshold, featureMotion != nullptr, {} };
    commands->BeginPixEvent("Feature-aware motion blur", Colors::Coral);
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
    else
    {
        commands->ResourceBarrier({ inputs[0]->GetResource()->GetCopySourceBarrier(),
            _color->GetResource()->GetCopyDestBarrier() });
        commands->CopyResource(_color->GetResource()->D3DResource.Get(), inputs[0]->GetResource()->D3DResource.Get());
    }
    commands->ResourceBarrier({ _color->GetResource()->GetSRVBarrier() });
    commands->EndPixEvent();
}

void GDX12MotionBlurPass::Resize()
{
    auto* input = _inputs[0]->GetTexture();
    _color->Resize(input->GetWidth(), input->GetHeight());
    _motion->Resize(input->GetWidth(), input->GetHeight());
    _velocity->Resize(input->GetWidth(), input->GetHeight());
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
