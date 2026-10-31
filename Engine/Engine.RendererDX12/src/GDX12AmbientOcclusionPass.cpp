#include "Engine.RendererDX12/RenderPasses/GDX12AmbientOcclusionPass.h"
#include "Engine.RendererDX12/RenderSettings.h"
#include "../Shaders/XeGTAO/XeGTAO.h"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace
{
    struct AOParameters
    {
        XMFLOAT4X4 Projection, InverseProjection;
        XMUINT2 Size, WorkingSize;
        float Radius, Intensity, Bias;
        UINT Samples;
        UINT Horizontal, RestorePackedScale;
        UINT Padding[2] = {};
    };
    static_assert(sizeof(AOParameters) == 176);
    constexpr UINT Prepare = 0, SSAO = 1, HBAO = 2, Blur = 3, Resolve = 4;
    constexpr UINT XePrefilter = 0, XeMain = 1, XeDenoise = 2, XeDenoiseLast = 3, XeResolve = 4;
}

struct GDX12AmbientOcclusionPass::Data
{
    GDX12Texture Visibility, RawDepth, ViewNormals, WorkingDepth, Edges;
    std::array<GDX12Texture, 2> PackedAO, ClassicAO;
    std::array<GDX12Descriptor, 5> DepthMipUAV;
    std::unique_ptr<GDX12RootSignature> Root;
    std::array<ComPtr<ID3D12PipelineState>, 5> Classic, Xe;
    ComPtr<ID3D12PipelineState> Neutral;
    std::unique_ptr<GDX12UploadBuffer<AOParameters>> Parameters;
    std::unique_ptr<GDX12UploadBuffer<XeGTAO::GTAOConstants>> XeConstants;
    UINT Width = 0, Height = 0, WorkingWidth = 0, WorkingHeight = 0;

    void CreateTexture(GDX12DeviceResources& resources, GDX12Texture& texture,
        UINT width, UINT height, DXGI_FORMAT format, UINT mips = 1)
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
        desc.SRVDesc.Texture2D.MipLevels = mips;
        desc.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        if (mips > 1)
        {
            const auto allocation = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1,
                static_cast<UINT16>(mips), 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            const auto properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            ThrowIfFailed(resources.Device->GetDevice()->CreateCommittedResource(&properties,
                D3D12_HEAP_FLAG_NONE, &allocation, D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(&desc.ExternalResource)));
        }
        texture.Initialize(desc);
    }

    void CreateTextures(GDX12DeviceResources& resources, UINT width, UINT height)
    {
        if (width == 0 || height == 0 || width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
            throw std::invalid_argument("Ambient occlusion viewport is outside DX12 texture limits");
        Width = width; Height = height;
        // Padding gives every XeGTAO 16x16 prefilter/denoise group valid UAV coordinates.
        WorkingWidth = (width + 15u) & ~15u;
        WorkingHeight = (height + 15u) & ~15u;
        CreateTexture(resources, Visibility, width, height, DXGI_FORMAT_R32_FLOAT);
        CreateTexture(resources, RawDepth, WorkingWidth, WorkingHeight, DXGI_FORMAT_R32_FLOAT);
        CreateTexture(resources, ViewNormals, WorkingWidth, WorkingHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
        CreateTexture(resources, WorkingDepth, WorkingWidth, WorkingHeight, DXGI_FORMAT_R32_FLOAT, 5);
        CreateTexture(resources, Edges, WorkingWidth, WorkingHeight, DXGI_FORMAT_R8_UNORM);
        for (auto& texture : PackedAO)
            CreateTexture(resources, texture, WorkingWidth, WorkingHeight, DXGI_FORMAT_R32_UINT);
        for (auto& texture : ClassicAO)
            CreateTexture(resources, texture, WorkingWidth, WorkingHeight, DXGI_FORMAT_R32_FLOAT);
        auto* heap = resources.SRV_UAV_Heap.get();
        for (UINT mip = 0; mip < 5; ++mip)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
            desc.Format = DXGI_FORMAT_R32_FLOAT;
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipSlice = mip;
            const UINT slot = DepthMipUAV[mip].HeapIndex != UINT_MAX ? DepthMipUAV[mip].HeapIndex :
                heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
            DepthMipUAV[mip].InitAsUAV(WorkingDepth.GetResource()->D3DResource.Get(), nullptr, &desc, heap, slot);
        }
    }

    void Bind(GDX12CommandList* commands, const ComPtr<ID3D12PipelineState>& pipeline,
        D3D12_GPU_VIRTUAL_ADDRESS firstCB, D3D12_GPU_VIRTUAL_ADDRESS secondCB)
    {
        commands->SetComputeRootSignature(Root.get());
        commands->SetPipelineState(pipeline);
        commands->SetComputeRootConstantBufferView(0, firstCB);
        commands->SetComputeRootConstantBufferView(1, secondCB);
    }
};

GDX12AmbientOcclusionPass::GDX12AmbientOcclusionPass() : _data(std::make_unique<Data>())
{
    _flags = RENDER_PASS_FLAG_USE_CAMERAS;
    _numInputs = 2;
    _numOutputs = 1;
    _outputs.push_back(&_data->Visibility);
}
GDX12AmbientOcclusionPass::~GDX12AmbientOcclusionPass() = default;

void GDX12AmbientOcclusionPass::Initialize()
{
    if (_inputs.size() != 2 || !_inputs[0] || !_inputs[1])
        throw std::invalid_argument("Ambient occlusion requires depth and world normal textures");
    auto& d = *_data;
    d.CreateTextures(*_resources, _inputs[0]->GetTexture()->GetWidth(), _inputs[0]->GetTexture()->GetHeight());
    const UINT frames = static_cast<UINT>(_resources->FrameConstants.size());
    d.Parameters = std::make_unique<GDX12UploadBuffer<AOParameters>>(_resources->Device, frames * 2);
    d.XeConstants = std::make_unique<GDX12UploadBuffer<XeGTAO::GTAOConstants>>(_resources->Device, frames);
    GDX12RootSignatureDesc root;
    root.NumSingleCBVSlots = 2;
    root.NumSingleSRVSlots = 3;
    root.NumSingleUAVSlots = 5;
    root.StaticSamplers.emplace_back(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    d.Root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
    const char* classic[] = { "PrepareCS", "SSAOCS", "HBAOCS", "BlurCS", "ResolveCS" };
    const char* xe[] = { "PrefilterCS", "MainCS", "DenoiseCS", "FinalDenoiseCS", "ResolveCS" };
    for (UINT i = 0; i < 5; ++i)
    {
        auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(_resources->Device,
            SHADERS_FOLDER "AOClassic.hlsl", nullptr, classic[i], "cs");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = d.Root->GetRootSignature().Get();
        pso.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
        ThrowIfFailed(_resources->Device->GetDevice()->CreateComputePipelineState(&pso, IID_PPV_ARGS(&d.Classic[i])));
        const auto stage = std::to_string(i == XeDenoiseLast ? 2 : (i == XeResolve ? 3 : i));
        const D3D_SHADER_MACRO macros[] = { { "AO_STAGE", stage.c_str() }, { nullptr, nullptr } };
        shader = GDX12ShaderCompiler::GetInstance().CompileShader(_resources->Device,
            SHADERS_FOLDER "AOXeGTAO.hlsl", macros, xe[i], "cs");
        pso.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
        ThrowIfFailed(_resources->Device->GetDevice()->CreateComputePipelineState(&pso, IID_PPV_ARGS(&d.Xe[i])));
    }
    auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(_resources->Device,
        SHADERS_FOLDER "AOClassic.hlsl", nullptr, "NeutralCS", "cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC neutral = {};
    neutral.pRootSignature = d.Root->GetRootSignature().Get();
    neutral.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
    ThrowIfFailed(_resources->Device->GetDevice()->CreateComputePipelineState(&neutral, IID_PPV_ARGS(&d.Neutral)));
}

void GDX12AmbientOcclusionPass::Execute(GDX12CommandList* commands)
{
    auto& d = *_data;
    const auto settings = RenderSettings::GetAO();
    commands->BeginPixEvent("Ambient occlusion", Colors::DarkSlateBlue);
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    if (!settings.Enabled || settings.Intensity == 0)
    {
        commands->ResourceBarrier({ d.Visibility.GetResource()->GetUnorderedAccessBarrier() });
        commands->SetComputeRootSignature(d.Root.get());
        commands->SetPipelineState(d.Neutral);
        commands->SetComputeUAV(0, d.Visibility.GetUAV()->GPUHandle);
        commands->Dispatch((d.Width + 7) / 8, (d.Height + 7) / 8, 1);
        commands->ResourceBarrier({ d.Visibility.GetResource()->GetSRVBarrier() });
        commands->EndPixEvent();
        return;
    }

    const UINT frameIndex = _resources->CurrFrameConstantsIndex;
    auto& frame = _resources->FrameConstants[frameIndex];
    Matrix projection = _commonData->CameraViewToClip;
    projection._31 += _commonData->ActiveCameraJitterOffsetX * 2.0f / d.Width;
    projection._32 -= _commonData->ActiveCameraJitterOffsetY * 2.0f / d.Height;
    AOParameters parameters = {};
    parameters.Projection = projection.Transpose();
    parameters.InverseProjection = projection.Invert().Transpose();
    parameters.Size = { d.Width, d.Height };
    parameters.WorkingSize = { d.WorkingWidth, d.WorkingHeight };
    parameters.Radius = settings.Radius;
    parameters.Intensity = settings.Intensity;
    parameters.Bias = settings.Bias;
    parameters.Samples = settings.Samples;
    parameters.RestorePackedScale = settings.DenoisePasses == 0;
    d.Parameters->CopyData(frameIndex * 2, parameters);
    parameters.Horizontal = 1;
    d.Parameters->CopyData(frameIndex * 2 + 1, parameters);
    const auto parameterCB = d.Parameters->GetElementAddress(frameIndex * 2);
    const auto cameraCB = frame->CameraCB->GetElementAddress(_commonData->ActiveCameraCBufferIndex);
    auto* depth = _inputs[0]->GetTexture();
    auto* normals = _inputs[1]->GetTexture();

    d.Bind(commands, d.Classic[Prepare], parameterCB, cameraCB);
    commands->ResourceBarrier({ depth->GetResource()->GetSRVBarrier(), normals->GetResource()->GetSRVBarrier(),
        d.RawDepth.GetResource()->GetUnorderedAccessBarrier(), d.ViewNormals.GetResource()->GetUnorderedAccessBarrier() });
    commands->SetComputeSRV(0, depth->GetSRV()->GPUHandle);
    commands->SetComputeSRV(1, normals->GetSRV()->GPUHandle);
    commands->SetComputeUAV(1, d.RawDepth.GetUAV()->GPUHandle);
    commands->SetComputeUAV(2, d.ViewNormals.GetUAV()->GPUHandle);
    commands->Dispatch(d.WorkingWidth / 8, d.WorkingHeight / 8, 1);
    commands->ResourceBarrier({ d.RawDepth.GetResource()->GetSRVBarrier(), d.ViewNormals.GetResource()->GetSRVBarrier() });

    if (settings.Implementation == RenderSettings::AOImplementation::XeGTAO)
    {
        XeGTAO::GTAOSettings xeSettings;
        xeSettings.Radius = settings.Radius;
        xeSettings.DenoisePasses = settings.DenoisePasses;
        XeGTAO::GTAOConstants constants = {};
        XeGTAO::GTAOUpdateConstants(constants, d.WorkingWidth, d.WorkingHeight,
            xeSettings, &projection._11, true, 0);
        constants.DepthUnpackConsts = { -projection._43, -projection._33 };
        constants.NDCToViewMul = { 2.0f / projection._11 * d.WorkingWidth / d.Width,
            -2.0f / projection._22 * d.WorkingHeight / d.Height };
        constants.NDCToViewAdd = { (-1.0f + projection._31) / projection._11,
            (1.0f + projection._32) / projection._22 };
        constants.NDCToViewMul_x_PixelSize = { 2.0f / projection._11 / d.Width, -2.0f / projection._22 / d.Height };
        d.XeConstants->CopyData(frameIndex, constants);
        const auto xeCB = d.XeConstants->GetElementAddress(frameIndex);
        d.Bind(commands, d.Xe[XePrefilter], xeCB, parameterCB);
        commands->ResourceBarrier({ d.WorkingDepth.GetResource()->GetUnorderedAccessBarrier() });
        commands->SetComputeSRV(0, d.RawDepth.GetSRV()->GPUHandle);
        for (UINT mip = 0; mip < 5; ++mip) commands->SetComputeUAV(mip, d.DepthMipUAV[mip].GPUHandle);
        commands->Dispatch(d.WorkingWidth / 16, d.WorkingHeight / 16, 1);
        commands->ResourceBarrier({ d.WorkingDepth.GetResource()->GetSRVBarrier(),
            d.PackedAO[0].GetResource()->GetUnorderedAccessBarrier(), d.Edges.GetResource()->GetUnorderedAccessBarrier() });
        d.Bind(commands, d.Xe[XeMain], xeCB, parameterCB);
        commands->SetComputeSRV(0, d.WorkingDepth.GetSRV()->GPUHandle);
        commands->SetComputeSRV(1, d.ViewNormals.GetSRV()->GPUHandle);
        commands->SetComputeUAV(0, d.PackedAO[0].GetUAV()->GPUHandle);
        commands->SetComputeUAV(1, d.Edges.GetUAV()->GPUHandle);
        commands->Dispatch(d.WorkingWidth / 8, d.WorkingHeight / 8, 1);
        commands->ResourceBarrier({ d.Edges.GetResource()->GetSRVBarrier() });
        UINT source = 0;
        for (UINT pass = 0; pass < settings.DenoisePasses; ++pass)
        {
            const UINT destination = 1 - source;
            commands->ResourceBarrier({ d.PackedAO[source].GetResource()->GetSRVBarrier(),
                d.PackedAO[destination].GetResource()->GetUnorderedAccessBarrier() });
            d.Bind(commands, d.Xe[pass + 1 == settings.DenoisePasses ? XeDenoiseLast : XeDenoise], xeCB, parameterCB);
            commands->SetComputeSRV(0, d.PackedAO[source].GetSRV()->GPUHandle);
            commands->SetComputeSRV(1, d.Edges.GetSRV()->GPUHandle);
            commands->SetComputeUAV(0, d.PackedAO[destination].GetUAV()->GPUHandle);
            commands->Dispatch(d.WorkingWidth / 16, d.WorkingHeight / 8, 1);
            source = destination;
        }
        commands->ResourceBarrier({ d.PackedAO[source].GetResource()->GetSRVBarrier(),
            d.Visibility.GetResource()->GetUnorderedAccessBarrier() });
        d.Bind(commands, d.Xe[XeResolve], xeCB, parameterCB);
        commands->SetComputeSRV(0, d.PackedAO[source].GetSRV()->GPUHandle);
        commands->SetComputeSRV(1, d.RawDepth.GetSRV()->GPUHandle);
        commands->SetComputeUAV(0, d.Visibility.GetUAV()->GPUHandle);
        commands->Dispatch((d.Width + 7) / 8, (d.Height + 7) / 8, 1);
    }
    else
    {
        d.Bind(commands, d.Classic[settings.Implementation == RenderSettings::AOImplementation::SSAO ? SSAO : HBAO], parameterCB, cameraCB);
        commands->ResourceBarrier({ d.ClassicAO[0].GetResource()->GetUnorderedAccessBarrier() });
        commands->SetComputeSRV(0, d.RawDepth.GetSRV()->GPUHandle);
        commands->SetComputeSRV(1, d.ViewNormals.GetSRV()->GPUHandle);
        commands->SetComputeUAV(0, d.ClassicAO[0].GetUAV()->GPUHandle);
        commands->Dispatch((d.Width + 7) / 8, (d.Height + 7) / 8, 1);
        for (UINT pass = 0; pass < settings.DenoisePasses; ++pass)
        {
            for (UINT axis = 0; axis < 2; ++axis)
            {
                const UINT source = axis, destination = 1 - axis;
                commands->ResourceBarrier({ d.ClassicAO[source].GetResource()->GetSRVBarrier(),
                    d.ClassicAO[destination].GetResource()->GetUnorderedAccessBarrier() });
                d.Bind(commands, d.Classic[Blur], axis == 0 ? d.Parameters->GetElementAddress(frameIndex * 2 + 1) : parameterCB, cameraCB);
                commands->SetComputeSRV(2, d.ClassicAO[source].GetSRV()->GPUHandle);
                commands->SetComputeUAV(0, d.ClassicAO[destination].GetUAV()->GPUHandle);
                commands->Dispatch((d.Width + 7) / 8, (d.Height + 7) / 8, 1);
            }
        }
        commands->ResourceBarrier({ d.ClassicAO[0].GetResource()->GetSRVBarrier(), d.Visibility.GetResource()->GetUnorderedAccessBarrier() });
        d.Bind(commands, d.Classic[Resolve], parameterCB, cameraCB);
        commands->SetComputeSRV(2, d.ClassicAO[0].GetSRV()->GPUHandle);
        commands->SetComputeUAV(0, d.Visibility.GetUAV()->GPUHandle);
        commands->Dispatch((d.Width + 7) / 8, (d.Height + 7) / 8, 1);
    }
    commands->ResourceBarrier({ d.Visibility.GetResource()->GetSRVBarrier() });
    commands->EndPixEvent();
}

void GDX12AmbientOcclusionPass::Resize()
{
    _data->CreateTextures(*_resources, _inputs[0]->GetTexture()->GetWidth(), _inputs[0]->GetTexture()->GetHeight());
}
void GDX12AmbientOcclusionPass::ClearDenendencies()
{
    GDX12RenderPass::ClearDenendencies();
    _data.reset();
}
