#include "Engine.RendererDX12/RenderPasses/GDX12TAAPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12Reconstruction.h"
#include "Engine.Core/Logging/Logger.h"
#include <stdexcept>

GDX12TAAPass::GDX12TAAPass()
{
    _flags = RENDER_PASS_FLAG_UPSCALER | RENDER_PASS_FLAG_USE_JITTER | RENDER_PASS_FLAG_USE_CAMERAS;
    _numInputs = 3;
    _numOutputs = 1;
    _outputs = {&_color[0]};
}
void GDX12TAAPass::Setup(GDX12DeviceResources* resources, RenderPipelineCommonData* common)
{
    GDX12RenderPass::Setup(resources, common);
    QueryRenderTargetResolution();
}
void GDX12TAAPass::QueryRenderTargetResolution()
{
    _commonData->DownscaledWidth = _commonData->WindowWidth;
    _commonData->DownscaledHeight = _commonData->WindowHeight;
}
void GDX12TAAPass::Initialize()
{
    for (UINT i = 0; i < 2; ++i)
    {
        GDX12RenderHelpers::InitializeComputeTexture(_color[i], *_resources,
            _commonData->WindowWidth, _commonData->WindowHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
        GDX12RenderHelpers::InitializeComputeTexture(_depth[i], *_resources,
            _commonData->WindowWidth, _commonData->WindowHeight, DXGI_FORMAT_R32_FLOAT);
        _color[i].GetResource()->D3DResource->SetName(i ? L"TAA.Color1" : L"TAA.Color0");
        _depth[i].GetResource()->D3DResource->SetName(i ? L"TAA.Depth1" : L"TAA.Depth0");
    }
    GDX12RootSignatureDesc root;
    static_assert(sizeof(Parameters) == 24 * sizeof(UINT));
    root.Constants = {24};
    root.NumSingleCBVSlots = 1;
    root.NumSingleSRVSlots = 5;
    root.NumSingleUAVSlots = 2;
    root.StaticSamplers = {CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP)};
    _root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
    _resolve = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _root.get(), SHADERS_FOLDER "NativeTAA.hlsl", "Resolve");
    Log::Info("Reconstruction", "Effective Native TAA: render=output=", _commonData->WindowWidth, "x", _commonData->WindowHeight,
        "; DIRECT compute resolve, sequential color/depth history");
    CancelRecording();
}
void GDX12TAAPass::Resize()
{
    for (UINT i = 0; i < 2; ++i)
    {
        _color[i].Resize(_commonData->WindowWidth, _commonData->WindowHeight);
        _depth[i].Resize(_commonData->WindowWidth, _commonData->WindowHeight);
    }
    CancelRecording();
}
void GDX12TAAPass::PrepareRecording()
{
    _pending = false;
    _frame.Constants.Width = _frame.Constants.Height = 0;
    if (!_commonData->WindowWidth || !_commonData->WindowHeight) return;
    const auto width = _commonData->WindowWidth, height = _commonData->WindowHeight;
    for (auto* input : _inputs)
        if (input->GetTexture()->GetWidth() != width || input->GetTexture()->GetHeight() != height)
            throw std::invalid_argument("Native TAA requires matching native-resolution inputs");
    auto& frame = _frame;
    frame.Write = _write;
    frame.ViewProjection = _commonData->CameraView * _commonData->CameraViewToClip;
    frame.Jitter = {_commonData->ActiveCameraJitterOffsetX, _commonData->ActiveCameraJitterOffsetY};
    frame.Revision = _commonData->HistoryRevision;
    XMStoreFloat4x4(&frame.Constants.PreviousViewProjection, XMMatrixTranspose(_previousViewProjection));
    frame.Constants.Width = width;
    frame.Constants.Height = height;
    frame.Constants.Valid = _valid && _revision == frame.Revision &&
        !_commonData->ResetHistory && !_commonData->ResetUpscalerHistory && !_commonData->RequestHistoryReset;
    frame.Constants.HistoryWeight = GetTAAHistoryWeight();
    frame.Constants.VarianceGamma = GetTAAVarianceGamma();
    frame.Constants.DepthJitterDeltaUV = {(_previousJitter.x - frame.Jitter.x) / width,
        (_previousJitter.y - frame.Jitter.y) / height};
    frame.Reads = {_inputs[2]->GetTexture(), _inputs[0]->GetTexture(), _inputs[1]->GetTexture(),
        &_color[1 - _write], &_depth[1 - _write]};
    frame.Color = &_color[_write];
    frame.Depth = &_depth[_write];
    const auto& camera = _resources->FrameConstants.at(_resources->CurrFrameConstantsIndex)->CameraCB;
    frame.Camera = &camera->GetResource();
    frame.CameraAddress = camera->GetElementAddress(_commonData->ActiveCameraCBufferIndex);
    _outputs[0] = frame.Color;
    _pending = true;
}
void GDX12TAAPass::RecordCommands(GDX12CommandList* commands, const Frame& frame) const
{
    if (!frame.Constants.Width || !frame.Constants.Height) return;
    GDX12_SCOPED_EVENT(commands, "Native TAA resolve", Colors::Cyan);
    commands->SetComputeRootSignature(_root.get());
    commands->SetDescriptorHeaps({_resources->SRV_UAV_Heap.get()});
    commands->GetCommandList()->SetComputeRoot32BitConstants(0, 24, &frame.Constants, 0);
    commands->SetComputeRootConstantBufferView(1, frame.CameraAddress);
    for (UINT i = 0; i < frame.Reads.size(); ++i)
    {
        auto* texture = frame.Reads[i];
        commands->ResourceBarrier({texture->GetResource()->GetNonPixelShaderResourceBarrier()});
        commands->SetComputeSRV(i, texture->GetSRV()->GPUHandle);
    }
    commands->ResourceBarrier({frame.Color->GetResource()->GetUnorderedAccessBarrier(),
        frame.Depth->GetResource()->GetUnorderedAccessBarrier()});
    commands->SetComputeUAV(0, frame.Color->GetUAV()->GPUHandle);
    commands->SetComputeUAV(1, frame.Depth->GetUAV()->GPUHandle);
    commands->SetPipelineState(_resolve.Get());
    commands->Dispatch((frame.Constants.Width + 7) / 8, (frame.Constants.Height + 7) / 8, 1);
    commands->ResourceBarrier({frame.Color->GetResource()->GetSRVBarrier(), frame.Depth->GetResource()->GetSRVBarrier()});
}
void GDX12TAAPass::Submitted()
{
    if (!_pending) return;
    _write = 1 - _frame.Write;
    _previousViewProjection = _frame.ViewProjection;
    _previousJitter = _frame.Jitter;
    _revision = _frame.Revision;
    _valid = true;
    _pending = false;
}
