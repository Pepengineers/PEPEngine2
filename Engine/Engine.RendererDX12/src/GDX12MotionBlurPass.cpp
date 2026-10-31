#include "Engine.RendererDX12/RenderPasses/GDX12MotionBlurPass.h"
#include "Engine.RendererDX12/RenderSettings.h"
#include <array>
#include <cmath>
#include <stdexcept>

namespace
{
    struct BlurParameters
    {
        XMFLOAT4X4 InverseViewProjection, PreviousViewProjection;
        XMUINT2 Size, TileCount;
        UINT TileSize, Samples;
        float HalfShutter, MaxRadius;
        XMFLOAT2 DepthUnpack, Padding;
        XMFLOAT4 CameraLocation;
    };
    static_assert(sizeof(BlurParameters) == 192);
}

struct GDX12MotionBlurPass::Data
{
    GDX12Texture Color, TileMax, NeighborMax;
    std::unique_ptr<GDX12RootSignature> Root;
    std::array<ComPtr<ID3D12PipelineState>, 3> Pipelines;
    std::unique_ptr<GDX12UploadBuffer<BlurParameters>> Parameters;
    UINT Width = 0, Height = 0, TileSize = 0, TilesX = 0, TilesY = 0;
    uint64_t Revision = 0;
    bool History = false, WasEnabled = false;

    void CreateTexture(GDX12DeviceResources& resources, GDX12Texture& texture,
        UINT width, UINT height, DXGI_FORMAT format)
    {
        auto* heap = resources.SRV_UAV_Heap.get();
        GDX12TextureDesc desc;
        desc.Width = width; desc.Height = height;
        desc.Format = desc.SRVDesc.Format = desc.UAVDesc.Format = format;
        desc.CreateUAV = true;
        desc.Semantic = ETextureSemantic::Data;
        desc.SRV_UAV_Heap = heap;
        desc.SRVHeapIndex = texture.IsInitialized() ? texture.GetSRV()->HeapIndex :
            heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.UAVHeapIndex = texture.IsInitialized() ? texture.GetUAV()->HeapIndex :
            heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.SRVDesc.Texture2D.MipLevels = 1;
        desc.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        texture.Initialize(desc);
    }

    void CreateTiles(GDX12DeviceResources& resources, UINT tileSize)
    {
        TileSize = tileSize;
        TilesX = (Width + tileSize - 1) / tileSize;
        TilesY = (Height + tileSize - 1) / tileSize;
        CreateTexture(resources, TileMax, TilesX, TilesY, DXGI_FORMAT_R32G32_FLOAT);
        CreateTexture(resources, NeighborMax, TilesX, TilesY, DXGI_FORMAT_R32G32_FLOAT);
    }
};

GDX12MotionBlurPass::GDX12MotionBlurPass() : _data(std::make_unique<Data>())
{
    _flags = RENDER_PASS_FLAG_USE_CAMERAS;
    _numInputs = 3;
    _numOutputs = 1;
    _outputs.push_back(&_data->Color);
}
GDX12MotionBlurPass::~GDX12MotionBlurPass() = default;

void GDX12MotionBlurPass::Initialize()
{
    if (_inputs.size() != 3)
        throw std::invalid_argument("Motion blur requires color, depth and motion");
    for (auto* input : _inputs)
        if (!input || !input->IsInitialized()) throw std::invalid_argument("Uninitialized motion blur input");
    auto* color = _inputs[0]->GetTexture();
    if (color->GetFormat() != DXGI_FORMAT_R16G16B16A16_FLOAT)
        throw std::invalid_argument("Motion blur requires linear HDR RGBA16F color");
    auto& d = *_data;
    d.Width = color->GetWidth(); d.Height = color->GetHeight();
    for (UINT i = 1; i < 3; ++i)
        if (_inputs[i]->GetTexture()->GetWidth() != d.Width || _inputs[i]->GetTexture()->GetHeight() != d.Height)
            throw std::invalid_argument("Motion blur color, depth and velocity grids must match");
    d.CreateTexture(*_resources, d.Color, d.Width, d.Height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    d.CreateTiles(*_resources, static_cast<UINT>(std::ceil(RenderSettings::GetMotionBlur().MaxRadius)));
    d.Parameters = std::make_unique<GDX12UploadBuffer<BlurParameters>>(_resources->Device,
        static_cast<UINT>(_resources->FrameConstants.size()));
    GDX12RootSignatureDesc root;
    root.NumSingleCBVSlots = 1;
    root.NumSingleSRVSlots = 5;
    root.NumSingleUAVSlots = 3;
    root.StaticSamplers.emplace_back(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    d.Root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
    const char* stages[] = { "TileMaxCS", "NeighborMaxCS", "GatherCS" };
    for (UINT i = 0; i < 3; ++i)
    {
        auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(_resources->Device,
            SHADERS_FOLDER "MotionBlur.hlsl", nullptr, stages[i], "cs");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
        pipeline.pRootSignature = d.Root->GetRootSignature().Get();
        pipeline.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
        ThrowIfFailed(_resources->Device->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&d.Pipelines[i])));
    }
}

void GDX12MotionBlurPass::PrepareFrame()
{
    const auto settings = RenderSettings::GetMotionBlur();
    const UINT tileSize = static_cast<UINT>(std::ceil(settings.MaxRadius));
    if (settings.Enabled && tileSize != _data->TileSize)
    {
        _resources->Device->GetCommandQueue()->Flush();
        _data->CreateTiles(*_resources, tileSize);
    }
}

bool GDX12MotionBlurPass::IsEnabled() const { return RenderSettings::GetMotionBlur().Enabled; }

void GDX12MotionBlurPass::Execute(GDX12CommandList* commands)
{
    auto& d = *_data;
    const auto settings = RenderSettings::GetMotionBlur();
    const bool cut = !d.History || d.Revision != _commonData->HistoryRevision || !d.WasEnabled;
    d.History = true;
    d.Revision = _commonData->HistoryRevision;
    d.WasEnabled = settings.Enabled;
    auto* color = _inputs[0]->GetTexture();
    commands->BeginPixEvent("Feature-aware motion blur", Colors::DarkSlateBlue);
    if (!settings.Enabled || cut)
    {
        commands->ResourceBarrier({ color->GetResource()->GetCopySourceBarrier(), d.Color.GetResource()->GetCopyDestBarrier() });
        commands->GetCommandList()->CopyResource(d.Color.GetResource()->D3DResource.Get(), color->GetResource()->D3DResource.Get());
        commands->ResourceBarrier({ color->GetResource()->GetSRVBarrier(), d.Color.GetResource()->GetSRVBarrier() });
        commands->EndPixEvent();
        return;
    }
    BlurParameters parameters = {};
    parameters.InverseViewProjection = (_commonData->ActiveCameraView * _commonData->CameraViewToClip).Invert().Transpose();
    parameters.PreviousViewProjection = _commonData->ActiveCameraPreviousViewProjection.Transpose();
    parameters.Size = { d.Width, d.Height }; parameters.TileCount = { d.TilesX, d.TilesY };
    parameters.TileSize = d.TileSize; parameters.Samples = settings.Samples;
    parameters.HalfShutter = 0.5f * settings.ShutterFraction;
    parameters.MaxRadius = settings.MaxRadius;
    parameters.DepthUnpack = { _commonData->CameraViewToClip._33, _commonData->CameraViewToClip._43 };
    const Matrix inverseView = _commonData->ActiveCameraView.Invert();
    parameters.CameraLocation = { inverseView._41, inverseView._42, inverseView._43, 1 };
    const UINT frame = _resources->CurrFrameConstantsIndex;
    d.Parameters->CopyData(frame, parameters);
    auto* depth = _inputs[1]->GetTexture();
    auto* motion = _inputs[2]->GetTexture();
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    commands->SetComputeRootSignature(d.Root.get());
    commands->SetComputeRootConstantBufferView(0, d.Parameters->GetElementAddress(frame));
    commands->ResourceBarrier({ color->GetResource()->GetSRVBarrier(), depth->GetResource()->GetSRVBarrier(),
        motion->GetResource()->GetSRVBarrier(), d.TileMax.GetResource()->GetUnorderedAccessBarrier() });
    commands->SetComputeSRV(0, color->GetSRV()->GPUHandle);
    commands->SetComputeSRV(1, depth->GetSRV()->GPUHandle);
    commands->SetComputeSRV(2, motion->GetSRV()->GPUHandle);
    commands->SetPipelineState(d.Pipelines[0]);
    commands->SetComputeUAV(1, d.TileMax.GetUAV()->GPUHandle);
    commands->Dispatch(d.TilesX, d.TilesY, 1);
    commands->ResourceBarrier({ d.TileMax.GetResource()->GetSRVBarrier(), d.NeighborMax.GetResource()->GetUnorderedAccessBarrier() });
    commands->SetPipelineState(d.Pipelines[1]);
    commands->SetComputeSRV(3, d.TileMax.GetSRV()->GPUHandle);
    commands->SetComputeUAV(2, d.NeighborMax.GetUAV()->GPUHandle);
    commands->Dispatch((d.TilesX + 7) / 8, (d.TilesY + 7) / 8, 1);
    commands->ResourceBarrier({ d.NeighborMax.GetResource()->GetSRVBarrier(), d.Color.GetResource()->GetUnorderedAccessBarrier() });
    commands->SetPipelineState(d.Pipelines[2]);
    commands->SetComputeSRV(4, d.NeighborMax.GetSRV()->GPUHandle);
    commands->SetComputeUAV(0, d.Color.GetUAV()->GPUHandle);
    commands->Dispatch((d.Width + 7) / 8, (d.Height + 7) / 8, 1);
    commands->ResourceBarrier({ d.Color.GetResource()->GetSRVBarrier() });
    commands->EndPixEvent();
}

void GDX12MotionBlurPass::Resize()
{
    auto& d = *_data;
    d.Width = _inputs[0]->GetTexture()->GetWidth(); d.Height = _inputs[0]->GetTexture()->GetHeight();
    d.CreateTexture(*_resources, d.Color, d.Width, d.Height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    d.CreateTiles(*_resources, static_cast<UINT>(std::ceil(RenderSettings::GetMotionBlur().MaxRadius)));
    d.History = false;
}
void GDX12MotionBlurPass::ClearDenendencies()
{
    GDX12RenderPass::ClearDenendencies();
    _data.reset();
}
