#include "Engine.RendererDX12/RenderPasses/GDX12SSAOPass.h"
#include <cmath>

GDX12SSAOPass::GDX12SSAOPass()
{ _flags = RENDER_PASS_FLAG_USE_CAMERAS; }

void GDX12SSAOPass::Describe(PassDescription& description) const
{
    description.AddParameter("Radius", 50.0f);
    description.AddParameter("Intensity", 1.0f);
    description.AddParameter("Bias", 0.5f);
    description.AddParameter("Samples", 16);
    description.AddParameter("Sharpness", 0.98f);
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
    // The horizontal blur reads the raw AO through an SRV inside this pass.
    occlusion.Usages = ResourceUsage::UnorderedAccess | ResourceUsage::NonPixelShaderRead;
    occlusion.AllowShared = true;
    description.RequireSameTextureSize({"InDepthBuffer", "InNormalRoughness"});
}

void GDX12SSAOPass::Initialize(PassInitContext& context)
{
    _radius = context.GetParameter<float>("Radius");
    _intensity = context.GetParameter<float>("Intensity");
    _bias = context.GetParameter<float>("Bias");
    _sharpness = context.GetParameter<float>("Sharpness");
    _fadeStart = context.GetParameter<float>("FadeStart");
    _fadeEnd = context.GetParameter<float>("FadeEnd");
    const int samples = context.GetParameter<int>("Samples");
    if (!std::isfinite(_radius) || _radius <= 0 || !std::isfinite(_intensity) || _intensity < 0 ||
        !std::isfinite(_bias) || _bias < 0 || !std::isfinite(_sharpness) || _sharpness < 0 || _sharpness > 1 ||
        !std::isfinite(_fadeStart) || _fadeStart < 0 || !std::isfinite(_fadeEnd) || _fadeEnd <= _fadeStart ||
        samples < 4 || samples > 64)
    { throw std::invalid_argument("ERROR: SSAO requires positive Radius, nonnegative Intensity/Bias, Samples in [4,64], Sharpness in [0,1], and 0 <= FadeStart < FadeEnd. Distances are in scene centimetres."); }
    _samples = static_cast<UINT>(samples);

    auto* device = context.GetDevice();
    GDX12RootSignatureDesc root;
    root.Constants = {8};
    root.NumSingleCBVSlots = 1;
    root.NumSingleSRVSlots = 3;
    root.NumSingleUAVSlots = 1;
    root.StaticSamplers = GetStaticSamplers();
    _root = std::make_unique<GDX12RootSignature>(device, root);
    auto& compiler = GDX12ShaderCompiler::GetInstance();
    const auto file = SHADERS_FOLDER "SSAOPass.hlsl";
    auto occlusion = compiler.CompileShader(device, file, nullptr, "SSAOCS", "cs");
    auto blur = compiler.CompileShader(device, file, nullptr, "BlurCS", "cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
    pipeline.pRootSignature = _root->GetRootSignature().Get();
    pipeline.CS = {occlusion->GetBufferPointer(), occlusion->GetBufferSize()};
    ThrowIfFailed(device->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&_occlusionPipeline)));
    pipeline.CS = {blur->GetBufferPointer(), blur->GetBufferSize()};
    ThrowIfFailed(device->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&_blurPipeline)));
    CreateBlurTexture(context);
}

void GDX12SSAOPass::CreateBlurTexture(PassInitContext& context)
{
    const auto size = context.GetOutputResourceDescription("OutAmbientOcclusion").GetSize();
    if (!_blurred)
    {
        auto* resources = context.GetDeviceResources();
        GDX12TextureDesc desc;
        desc.Device = context.GetDevice();
        desc.Width = size.x;
        desc.Height = size.y;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.CreateUAV = true;
        desc.SRV_UAV_Heap = resources->SRV_UAV_Heap.get();
        desc.SRVHeapIndex = resources->SRV_UAV_Heap->GetAvailableIndex(RendererResources_StartIndex);
        desc.UAVHeapIndex = resources->SRV_UAV_Heap->GetAvailableIndex(RendererResources_StartIndex);
        _blurred = std::make_unique<GDX12Texture>();
        _blurred->Initialize(desc);
    }
    else { _blurred->Resize(size.x, size.y); }
}

void GDX12SSAOPass::OnResourcesRecreated(PassInitContext& context)
{ CreateBlurTexture(context); }

std::vector<ExternalResourceUse> GDX12SSAOPass::GetExternalResources(PassExecutionContext& context)
{
    auto uses = GDX12RenderPass::GetExternalResources(context);
    uses.push_back({_blurred->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    return uses;
}

void GDX12SSAOPass::Execute(PassExecutionContext& context)
{
    auto* cmd = context.GetCommandList();
    auto* resources = context.GetDeviceResources();
    auto& frame = *resources->FrameConstants[context.GetFrameSlotResourcesIndex()];
    auto* output = context.GetOutputTexture("OutAmbientOcclusion").GetTexture();
    const auto size = context.GetOutputResourceDescription("OutAmbientOcclusion").GetSize();
    const struct { float Radius, Intensity, Bias, Sharpness; UINT Samples, BlurAxis; float FadeStart, FadeEnd; }
        parameters = {_radius, _intensity, _bias, _sharpness, _samples, 0, _fadeStart, _fadeEnd};

    cmd->BeginPixEvent("SSAO", Colors::DarkSlateBlue);
    cmd->SetComputeRootSignature(_root.get());
    cmd->SetDescriptorHeaps({resources->SRV_UAV_Heap.get()});
    cmd->GetCommandList()->SetComputeRoot32BitConstants(0, 8, &parameters, 0);
    cmd->SetComputeRootConstantBufferView(1, frame.CameraCB->GetElementAddress(context.GetFrameData()->ActiveCameraCBufferIndex));
    cmd->SetComputeSRV(0, context.GetInputTexture("InDepthBuffer").GetSRV()->GPUHandle);
    cmd->SetComputeSRV(1, context.GetInputTexture("InNormalRoughness").GetSRV()->GPUHandle);
    cmd->SetComputeUAV(0, output->GetUAV()->GPUHandle);
    cmd->SetPipelineState(_occlusionPipeline);
    cmd->Dispatch((size.x + 7) / 8, (size.y + 7) / 8, 1);

    cmd->ResourceBarrier({output->GetResource()->GetNonPixelShaderResourceBarrier()});
    cmd->SetPipelineState(_blurPipeline);
    cmd->SetComputeSRV(2, output->GetSRV()->GPUHandle);
    cmd->SetComputeUAV(0, _blurred->GetUAV()->GPUHandle);
    cmd->Dispatch((size.x + 7) / 8, (size.y + 7) / 8, 1);

    cmd->ResourceBarrier({_blurred->GetResource()->GetNonPixelShaderResourceBarrier(), output->GetResource()->GetUnorderedAccessBarrier()});
    cmd->GetCommandList()->SetComputeRoot32BitConstant(0, 1, 5);
    cmd->SetComputeSRV(2, _blurred->GetSRV()->GPUHandle);
    cmd->SetComputeUAV(0, output->GetUAV()->GPUHandle);
    cmd->Dispatch((size.x + 7) / 8, (size.y + 7) / 8, 1);
    cmd->ResourceBarrier({_blurred->GetResource()->GetUnorderedAccessBarrier()});
    cmd->EndPixEvent();
}
