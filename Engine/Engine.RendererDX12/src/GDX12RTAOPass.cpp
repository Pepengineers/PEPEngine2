#include "Engine.RendererDX12/RenderPasses/GDX12RTAOPass.h"
#include <cmath>

GDX12RTAOPass::GDX12RTAOPass()
{ _flags = RENDER_PASS_FLAG_USE_RAYTRACING | RENDER_PASS_FLAG_USE_CAMERAS; }

void GDX12RTAOPass::Describe(PassDescription& description) const
{
    description.AddParameter("Radius", 50.0f);
    description.AddParameter("Intensity", 1.0f);
    description.AddParameter("Bias", 0.5f);
    description.AddParameter("Samples", 2);
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

void GDX12RTAOPass::Initialize(PassInitContext& context)
{
    _radius = context.GetParameter<float>("Radius");
    _intensity = context.GetParameter<float>("Intensity");
    _bias = context.GetParameter<float>("Bias");
    _fadeStart = context.GetParameter<float>("FadeStart");
    _fadeEnd = context.GetParameter<float>("FadeEnd");
    const auto samples = context.GetParameter<int>("Samples");
    if (!std::isfinite(_radius) || _radius <= 0 || !std::isfinite(_intensity) || _intensity < 0
        || !std::isfinite(_bias) || _bias < 0 || samples < 1 || samples > 64
        || !std::isfinite(_fadeStart) || _fadeStart < 0 || !std::isfinite(_fadeEnd) || _fadeEnd <= _fadeStart)
    { throw std::invalid_argument("ERROR: RTAO requires positive Radius, nonnegative Intensity/Bias, Samples in [1,64], and 0 <= FadeStart < FadeEnd. Distances are in scene centimetres."); }
    _samples = static_cast<UINT>(samples);

    auto* resources = context.GetDeviceResources();
    GDX12RootSignatureDesc root;
    root.Constants = {6};
    root.NumSingleCBVSlots = 1;
    root.NumSingleSRVSlots = 9;
    root.NumSingleUAVSlots = 1;
    root.SRVRanges.emplace_back(Texture2D_RangeLength);
    root.StaticSamplers = GetStaticSamplers();
    _rootSignature = std::make_unique<GDX12RootSignature>(resources->Device, root);
    auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
    const auto shader = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "RTAOPass.hlsl", nullptr, "CS", "cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = _rootSignature->GetRootSignature().Get();
    pipeline.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    ThrowIfFailed(resources->Device->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&_occlusionPipeline)));
}

void GDX12RTAOPass::Execute(PassExecutionContext& context)
{
    auto* commands = context.GetCommandList();
    auto* resources = context.GetDeviceResources();
    auto& frame = *resources->FrameConstants[context.GetFrameSlotResourcesIndex()];
    const auto size = context.GetOutputResourceDescription("OutAmbientOcclusion").GetSize();
    const struct { float Radius, Intensity, Bias; UINT Samples; float FadeStart, FadeEnd; }
        parameters = {_radius, _intensity, _bias, _samples, _fadeStart, _fadeEnd};

    commands->BeginPixEvent("Ray Traced Ambient Occlusion", Colors::DarkSlateBlue);
    commands->SetComputeRootSignature(_rootSignature.get());
    commands->SetPipelineState(_occlusionPipeline);
    commands->SetDescriptorHeaps({resources->SRV_UAV_Heap.get()});
    commands->SetComputeRootConstants(0, parameters);
    commands->SetComputeRootConstantBufferView(1, frame.CameraCB->GetElementAddress(context.GetFrameData()->ActiveCameraCBufferIndex));
    commands->SetComputeSRV(0, context.GetInputTexture("InDepthBuffer")->GetSRV()->GPUHandle);
    commands->SetComputeSRV(1, context.GetInputTexture("InNormalRoughness")->GetSRV()->GPUHandle);
    commands->SetComputeSRV(2, resources->RayTracingScene->GetTLASSRV(context.GetFrameSlotResourcesIndex())->GPUHandle);
    commands->SetComputeSRV(3, resources->GeometryBuffer->GetVertexSRV()->GPUHandle);
    commands->SetComputeSRV(4, resources->GeometryBuffer->GetIndexSRV()->GPUHandle);
    commands->SetComputeSRV(5, resources->IndirectCommandsCache->GetSRV()->GPUHandle);
    commands->SetComputeSRV(6, frame.InstanceCache->GetSRV()->GPUHandle);
    commands->SetComputeSRV(7, frame.TransformCache->GetSRV()->GPUHandle);
    commands->SetComputeSRV(8, frame.MaterialCache->GetSRV()->GPUHandle);
    commands->SetComputeSRV(9, resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));
    commands->SetComputeUAV(0, context.GetOutputTexture("OutAmbientOcclusion").GetUAV()->GPUHandle);
    commands->Dispatch((size.x + 7) / 8, (size.y + 7) / 8, 1);
    commands->EndPixEvent();
}
