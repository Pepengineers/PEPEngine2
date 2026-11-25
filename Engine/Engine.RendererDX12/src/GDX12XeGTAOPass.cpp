#include "Engine.RendererDX12/RenderPasses/GDX12XeGTAOPass.h"
#include <cmath>

GDX12XeGTAOPass::GDX12XeGTAOPass()
{ _flags = RENDER_PASS_FLAG_USE_CAMERAS; }

void GDX12XeGTAOPass::Describe(PassDescription& description) const
{
    description.AddParameter("Radius", 50.0f);
    description.AddParameter("Intensity", 1.0f);
    description.AddParameter("Slices", 3);
    description.AddParameter("Steps", 3);
    description.AddParameter("DenoisePasses", 1);
    description.AddParameter("Sharpness", 0.98f);
    description.AddParameter("FalloffRange", XE_GTAO_DEFAULT_FALLOFF_RANGE);
    description.AddParameter("ThinOccluderCompensation", XE_GTAO_DEFAULT_THIN_OCCLUDER_COMPENSATION);
    description.AddParameter("TemporalNoise", true);
    description.AddParameter("FadeStart", 5000.0f);
    description.AddParameter("FadeEnd", 15000.0f);

    auto& depth = description.AddInputTexture("InDepthBuffer");
    depth.Formats = {DXGI_FORMAT_D32_FLOAT};
    depth.Usages = ResourceUsage::NonPixelShaderRead;

    auto& normals = description.AddInputTexture("InNormalRoughness");
    normals.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    normals.Usages = ResourceUsage::NonPixelShaderRead;
    normals.AllowShared = true;

    auto& occlusion = description.AddOutputTexture("OutAmbientOcclusion");
    occlusion.Formats = {DXGI_FORMAT_R32_FLOAT};
    occlusion.Size = Extent::SameAs("InDepthBuffer");
    occlusion.Usages = ResourceUsage::UnorderedAccess;
    occlusion.AllowShared = true;
    description.RequireSameTextureSize({"InDepthBuffer", "InNormalRoughness"});
}

void GDX12XeGTAOPass::Initialize(PassInitContext& context)
{
    _settings.Radius = context.GetParameter<float>("Radius");
    _intensity = context.GetParameter<float>("Intensity");
    _settings.DenoisePasses = context.GetParameter<int>("DenoisePasses");
    _settings.FalloffRange = context.GetParameter<float>("FalloffRange");
    _settings.ThinOccluderCompensation = context.GetParameter<float>("ThinOccluderCompensation");
    _sharpness = context.GetParameter<float>("Sharpness");
    _fadeStart = context.GetParameter<float>("FadeStart");
    _fadeEnd = context.GetParameter<float>("FadeEnd");
    _temporalNoise = context.GetParameter<bool>("TemporalNoise");
    const int slices = context.GetParameter<int>("Slices");
    const int steps = context.GetParameter<int>("Steps");
    if (!std::isfinite(_settings.Radius) || _settings.Radius <= 0 || !std::isfinite(_intensity) || _intensity < 0 ||
        !std::isfinite(_settings.FalloffRange) || _settings.FalloffRange <= 0 || _settings.FalloffRange > 1 ||
        !std::isfinite(_settings.ThinOccluderCompensation) || _settings.ThinOccluderCompensation < 0 || _settings.ThinOccluderCompensation > 1 ||
        !std::isfinite(_sharpness) || _sharpness < 0 || _sharpness > 1 ||
        !std::isfinite(_fadeStart) || _fadeStart < 0 || !std::isfinite(_fadeEnd) || _fadeEnd <= _fadeStart ||
        slices < 1 || slices > 9 || steps < 1 || steps > 16 || _settings.DenoisePasses < 0 || _settings.DenoisePasses > 8)
    { throw std::invalid_argument("ERROR: XeGTAO requires positive Radius, nonnegative Intensity, Slices in [1,9], Steps in [1,16], DenoisePasses in [0,8], Sharpness/ThinOccluderCompensation in [0,1], FalloffRange in (0,1], and 0 <= FadeStart < FadeEnd. Distances are in scene centimetres."); }

    auto* device = context.GetDevice();
    GDX12RootSignatureDesc root;
    root.Constants = {5, sizeof(XeGTAO::GTAOConstants) / sizeof(UINT)};
    root.NumSingleCBVSlots = 1;
    root.NumSingleSRVSlots = 5;
    root.NumSingleUAVSlots = 8;
    root.StaticSamplers = GetStaticSamplers();
    _root = std::make_unique<GDX12RootSignature>(device, root);
    auto& compiler = GDX12ShaderCompiler::GetInstance();
    const auto file = SHADERS_FOLDER "XeGTAOPass.hlsl";
    const std::string sliceCount = std::to_string(slices), stepCount = std::to_string(steps);
    const D3D_SHADER_MACRO defines[] = {{"XE_GTAO_SLICES", sliceCount.c_str()}, {"XE_GTAO_STEPS", stepCount.c_str()}, {nullptr, nullptr}};
    auto prefilter = compiler.CompileShader(device, file, nullptr, "PrefilterCS", "cs");
    auto occlusion = compiler.CompileShader(device, file, defines, "MainCS", "cs");
    auto denoise = compiler.CompileShader(device, file, nullptr, "DenoiseCS", "cs");
    auto resolve = compiler.CompileShader(device, file, nullptr, "ResolveCS", "cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
    pipeline.pRootSignature = _root->GetRootSignature().Get();
    pipeline.CS = {prefilter->GetBufferPointer(), prefilter->GetBufferSize()};
    ThrowIfFailed(device->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&_prefilterPipeline)));
    pipeline.CS = {occlusion->GetBufferPointer(), occlusion->GetBufferSize()};
    ThrowIfFailed(device->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&_occlusionPipeline)));
    pipeline.CS = {denoise->GetBufferPointer(), denoise->GetBufferSize()};
    ThrowIfFailed(device->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&_denoisePipeline)));
    pipeline.CS = {resolve->GetBufferPointer(), resolve->GetBufferSize()};
    ThrowIfFailed(device->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&_resolvePipeline)));
    CreateWorkingTextures(context);
}

void GDX12XeGTAOPass::CreateWorkingTextures(PassInitContext& context)
{
    const auto size = context.GetOutputResourceDescription("OutAmbientOcclusion").GetSize();
    const UINT width = (size.x + 15) & ~15u, height = (size.y + 15) & ~15u;
    auto* resources = context.GetDeviceResources();
    auto* heap = resources->SRV_UAV_Heap.get();
    const auto create = [&](std::unique_ptr<GDX12Texture>& texture, DXGI_FORMAT format, UINT mips = 1)
    {
        if (texture) { texture->Resize(width, height); return; }
        GDX12TextureDesc desc;
        desc.Device = context.GetDevice();
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = mips;
        desc.Format = format;
        desc.CreateUAV = true;
        desc.SRV_UAV_Heap = heap;
        desc.SRVHeapIndex = heap->GetAvailableIndex(RendererResources_StartIndex);
        desc.UAVHeapIndex = heap->GetAvailableIndex(RendererResources_StartIndex);
        texture = std::make_unique<GDX12Texture>();
        texture->Initialize(desc);
    };
    create(_depth, DXGI_FORMAT_R32_FLOAT, XE_GTAO_DEPTH_MIP_LEVELS);
    create(_aoTerms[0], DXGI_FORMAT_R8_UINT);
    create(_aoTerms[1], DXGI_FORMAT_R8_UINT);
    create(_edges, DXGI_FORMAT_R8_UNORM);
    for (UINT mip = 1; mip < XE_GTAO_DEPTH_MIP_LEVELS; ++mip)
    {
        auto& descriptor = _depthMipUAVs[mip - 1];
        const auto index = descriptor.DescType == DESC_TYPE_NONE ? heap->GetAvailableIndex(RendererResources_StartIndex) : descriptor.HeapIndex;
        D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
        view.Format = DXGI_FORMAT_R32_FLOAT;
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = mip;
        descriptor.InitAsUAV(_depth->GetResource()->D3DResource.Get(), nullptr, &view, heap, index);
    }
    _noiseIndex = 0;
}

void GDX12XeGTAOPass::OnResourcesRecreated(PassInitContext& context)
{ CreateWorkingTextures(context); }

std::vector<ExternalResourceUse> GDX12XeGTAOPass::GetExternalResources(PassExecutionContext& context)
{
    auto uses = GDX12RenderPass::GetExternalResources(context);
    uses.push_back({_depth->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    uses.push_back({_aoTerms[0]->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    uses.push_back({_aoTerms[1]->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    uses.push_back({_edges->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    return uses;
}

void GDX12XeGTAOPass::Execute(PassExecutionContext& context)
{
    auto* cmd = context.GetCommandList();
    auto* resources = context.GetDeviceResources();
    const auto* frame = context.GetFrameData();
    const auto size = context.GetOutputResourceDescription("OutAmbientOcclusion").GetSize();
    Matrix projection = frame->CameraViewToClip;
    projection._31 += frame->ActiveCameraJitterOffsetX * 2.0f / size.x;
    projection._32 -= frame->ActiveCameraJitterOffsetY * 2.0f / size.y;
    const bool jittered = frame->ActiveCameraJitterOffsetX != 0 || frame->ActiveCameraJitterOffsetY != 0;
    XeGTAO::GTAOConstants constants = {};
    XeGTAO::GTAOUpdateConstants(constants, size.x, size.y, _settings, &projection._11, true,
        _temporalNoise && jittered ? _noiseIndex++ : 0);
    // XeGTAO's helper assumes a centered projection
    constants.NDCToViewAdd.x += projection._31 / projection._11;
    constants.NDCToViewAdd.y += projection._32 / projection._22;
    constants.DenoiseBlurBeta = _settings.DenoisePasses ? 0.2f + _sharpness : 1e4f;
    const struct { float Intensity, FadeStart, FadeEnd; UINT FinalDenoise; float InputScale; }
        parameters = {_intensity, _fadeStart, _fadeEnd, 0, _settings.DenoisePasses ? 1.0f : XE_GTAO_OCCLUSION_TERM_SCALE};

    cmd->BeginPixEvent("XeGTAO", Colors::DarkSlateBlue);
    cmd->SetComputeRootSignature(_root.get());
    cmd->SetDescriptorHeaps({resources->SRV_UAV_Heap.get()});
    cmd->GetCommandList()->SetComputeRoot32BitConstants(0, 5, &parameters, 0);
    cmd->GetCommandList()->SetComputeRoot32BitConstants(1, sizeof(constants) / sizeof(UINT), &constants, 0);
    cmd->SetComputeRootConstantBufferView(2, resources->FrameConstants[context.GetFrameSlotResourcesIndex()]->CameraCB->GetElementAddress(frame->ActiveCameraCBufferIndex));
    cmd->SetComputeSRV(0, context.GetInputTexture("InDepthBuffer").GetSRV()->GPUHandle);
    cmd->SetComputeSRV(1, context.GetInputTexture("InNormalRoughness").GetSRV()->GPUHandle);
    cmd->SetComputeUAV(0, context.GetOutputTexture("OutAmbientOcclusion").GetUAV()->GPUHandle);
    cmd->SetComputeUAV(1, _depth->GetUAV()->GPUHandle);
    for (UINT mip = 1; mip < XE_GTAO_DEPTH_MIP_LEVELS; ++mip)
        cmd->SetComputeUAV(mip + 1, _depthMipUAVs[mip - 1].GPUHandle);
    cmd->SetPipelineState(_prefilterPipeline);
    cmd->Dispatch(_depth->GetWidth() / 16, _depth->GetHeight() / 16, 1);

    cmd->ResourceBarrier({_depth->GetResource()->GetNonPixelShaderResourceBarrier()});
    cmd->SetComputeSRV(2, _depth->GetSRV()->GPUHandle);
    cmd->SetComputeUAV(6, _aoTerms[0]->GetUAV()->GPUHandle);
    cmd->SetComputeUAV(7, _edges->GetUAV()->GPUHandle);
    cmd->SetPipelineState(_occlusionPipeline);
    cmd->Dispatch(_depth->GetWidth() / 8, _depth->GetHeight() / 8, 1);

    cmd->ResourceBarrier({_aoTerms[0]->GetResource()->GetNonPixelShaderResourceBarrier(), _edges->GetResource()->GetNonPixelShaderResourceBarrier()});
    cmd->SetComputeSRV(4, _edges->GetSRV()->GPUHandle);
    UINT source = 0;
    cmd->SetPipelineState(_denoisePipeline);
    for (int pass = 0; pass < _settings.DenoisePasses; ++pass)
    {
        const UINT target = 1 - source;
        cmd->ResourceBarrier({_aoTerms[target]->GetResource()->GetUnorderedAccessBarrier()});
        cmd->SetComputeSRV(3, _aoTerms[source]->GetSRV()->GPUHandle);
        cmd->SetComputeUAV(6, _aoTerms[target]->GetUAV()->GPUHandle);
        cmd->GetCommandList()->SetComputeRoot32BitConstant(0, pass + 1 == _settings.DenoisePasses, 3);
        cmd->Dispatch(_depth->GetWidth() / 16, _depth->GetHeight() / 8, 1);
        cmd->ResourceBarrier({_aoTerms[target]->GetResource()->GetNonPixelShaderResourceBarrier()});
        source = target;
    }
    cmd->SetComputeSRV(3, _aoTerms[source]->GetSRV()->GPUHandle);
    cmd->SetPipelineState(_resolvePipeline);
    cmd->Dispatch((size.x + 7) / 8, (size.y + 7) / 8, 1);
    cmd->ResourceBarrier({_depth->GetResource()->GetUnorderedAccessBarrier(), _edges->GetResource()->GetUnorderedAccessBarrier(),
        _aoTerms[0]->GetResource()->GetUnorderedAccessBarrier(), _aoTerms[1]->GetResource()->GetUnorderedAccessBarrier()});
    cmd->EndPixEvent();
}
