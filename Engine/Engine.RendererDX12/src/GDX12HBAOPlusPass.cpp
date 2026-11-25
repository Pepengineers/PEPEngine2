#include "Engine.RendererDX12/RenderPasses/GDX12HBAOPlusPass.h"
#include <cmath>

GDX12HBAOPlusPass::GDX12HBAOPlusPass()
{ _flags = RENDER_PASS_FLAG_USE_CAMERAS; }

GDX12HBAOPlusPass::ContextResources::~ContextResources()
{ if (Context) { Context->Release(); } }

void GDX12HBAOPlusPass::Describe(PassDescription& description) const
{
    description.AddParameter("Radius", 50.0f);
    description.AddParameter("Intensity", 2.0f);
    description.AddParameter("Bias", 0.1f);
    description.AddParameter("Steps", 4);
    description.AddParameter("Sharpness", 0.16f);
    description.AddParameter("EnableBlur", true);
    description.AddParameter("MaxDepth", 15000.0f);

    auto& depth = description.AddInputTexture("InDepthBuffer");
    depth.Formats = {DXGI_FORMAT_D32_FLOAT};
    depth.Usages = ResourceUsage::PixelShaderRead;

    auto& normals = description.AddInputTexture("InNormalRoughness");
    normals.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    normals.Usages = ResourceUsage::PixelShaderRead;
    normals.AllowShared = true;

    auto& occlusion = description.AddOutputTexture("OutAmbientOcclusion");
    occlusion.Formats = {DXGI_FORMAT_R32_FLOAT};
    occlusion.Size = Extent::SameAs("InDepthBuffer");
    occlusion.Usages = ResourceUsage::RenderTarget;
    occlusion.AllowShared = true;
    description.RequireSameTextureSize({"InDepthBuffer", "InNormalRoughness"});
}

void GDX12HBAOPlusPass::Initialize(PassInitContext& context)
{
    const float radius = context.GetParameter<float>("Radius");
    const float intensity = context.GetParameter<float>("Intensity");
    const float bias = context.GetParameter<float>("Bias");
    const float sharpness = context.GetParameter<float>("Sharpness");
    const float maxDepth = context.GetParameter<float>("MaxDepth");
    const int steps = context.GetParameter<int>("Steps");
    if (!std::isfinite(radius) || radius <= 0 || !std::isfinite(intensity) ||
        (intensity != 0 && (intensity < 1 || intensity > 4)) ||
        !std::isfinite(bias) || bias < 0 || bias > 0.5f ||
        !std::isfinite(sharpness) || sharpness < 0 || !std::isfinite(maxDepth) || maxDepth < 0 ||
        (steps != 4 && steps != 8))
    { throw std::invalid_argument("ERROR: HBAO+ requires positive Radius, Intensity 0 or [1,4], Bias in [0,0.5], nonnegative Sharpness/MaxDepth, and Steps 4 or 8. Distances are in scene centimetres."); }

    _parameters.Radius = radius * 0.01f;
    _parameters.PowerExponent = intensity;
    _parameters.Bias = bias;
    _parameters.StepCount = steps == 4 ? GFSDK_SSAO_STEP_COUNT_4 : GFSDK_SSAO_STEP_COUNT_8;
    _parameters.Blur.Enable = context.GetParameter<bool>("EnableBlur");
    _parameters.Blur.Sharpness = sharpness * 100.0f;
    _parameters.DepthThreshold.Enable = maxDepth > 0;
    _parameters.DepthThreshold.MaxViewDepth = maxDepth;
}

GDX12HBAOPlusPass::ContextResources& GDX12HBAOPlusPass::GetContext(PassExecutionContext& context)
{
    auto* device = context.GetDevice();
    const auto resourceSlot = context.GetFrameSlotResourcesIndex();
    if (_contexts.size() <= resourceSlot) { _contexts.resize(resourceSlot + 1); }
    auto& slot = _contexts[resourceSlot];
    if (!slot)
    {
        auto resources = std::make_unique<ContextResources>();
        resources->ShaderHeap = std::make_unique<GDX12DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            GFSDK_SSAO_NUM_DESCRIPTORS_CBV_SRV_UAV_HEAP_D3D12 + 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
        resources->RTVHeap = std::make_unique<GDX12DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
            GFSDK_SSAO_NUM_DESCRIPTORS_RTV_HEAP_D3D12, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
        GFSDK_SSAO_DescriptorHeaps_D3D12 heaps;
        heaps.CBV_SRV_UAV.pDescHeap = resources->ShaderHeap->GetHeap().Get();
        heaps.CBV_SRV_UAV.BaseIndex = 2;
        heaps.RTV.pDescHeap = resources->RTVHeap->GetHeap().Get();
        Check(GFSDK_SSAO_CreateContext_D3D12(device->GetDevice().Get(), 1, heaps, &resources->Context), "create context");
        slot = std::move(resources);
    }
    return *slot;
}

void GDX12HBAOPlusPass::UpdateInputViews(PassExecutionContext& context, ContextResources& slot)
{
    auto* device = context.GetDevice()->GetDevice().Get();
    auto* depth = context.GetInputTexture("InDepthBuffer").GetTexture()->GetResource()->D3DResource.Get();
    auto* normals = context.GetInputTexture("InNormalRoughness").GetTexture()->GetResource()->D3DResource.Get();
    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    if (slot.Depth.Get() != depth)
    {
        view.Format = DXGI_FORMAT_R32_FLOAT;
        device->CreateShaderResourceView(depth, &view, slot.ShaderHeap->GetCPUHandle(0));
        slot.Depth = depth;
    }
    if (slot.Normals.Get() != normals)
    {
        view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateShaderResourceView(normals, &view, slot.ShaderHeap->GetCPUHandle(1));
        slot.Normals = normals;
    }
}

void GDX12HBAOPlusPass::Execute(PassExecutionContext& context)
{
    auto* cmd = context.GetCommandList();
    auto* device = context.GetDevice();
    auto* output = context.GetOutputTexture("OutAmbientOcclusion").GetTexture();
    cmd->BeginPixEvent("HBAO+", Colors::DarkSlateBlue);
    if (_parameters.PowerExponent == 0)
    {
        const float white[] = {1, 1, 1, 1};
        cmd->GetCommandList()->ClearRenderTargetView(output->GetRTV()->CPUHandle, white, 0, nullptr);
        cmd->EndPixEvent();
        return;
    }

    auto& slot = GetContext(context);
    UpdateInputViews(context, slot);

    const auto* frame = context.GetFrameData();
    const auto size = context.GetOutputResourceDescription("OutAmbientOcclusion").GetSize();
    Matrix projection = frame->CameraViewToClip;
    projection._31 += frame->ActiveCameraJitterOffsetX * 2.0f / size.x;
    projection._32 -= frame->ActiveCameraJitterOffsetY * 2.0f / size.y;
    // The SDK reconstructs positive forward view depth even for an RH projection.
    const Matrix worldToView = frame->WorldToCameraView * Matrix::CreateScale(1, 1, -1);
    GFSDK_SSAO_InputData_D3D12 input;
    input.DepthData.DepthTextureType = GFSDK_SSAO_HARDWARE_DEPTHS;
    input.DepthData.ProjectionMatrix.Data = GFSDK_SSAO_Float4x4(&projection._11);
    input.DepthData.MetersToViewSpaceUnits = 100.0f;
    input.DepthData.FullResDepthTextureSRV.pResource = slot.Depth.Get();
    input.DepthData.FullResDepthTextureSRV.GpuHandle = slot.ShaderHeap->GetGPUHandle(0).ptr;
    input.NormalData.Enable = true;
    input.NormalData.WorldToViewMatrix.Data = GFSDK_SSAO_Float4x4(&worldToView._11);
    input.NormalData.FullResNormalTextureSRV.pResource = slot.Normals.Get();
    input.NormalData.FullResNormalTextureSRV.GpuHandle = slot.ShaderHeap->GetGPUHandle(1).ptr;

    GFSDK_SSAO_RenderTargetView_D3D12 target;
    target.pResource = output->GetResource()->D3DResource.Get();
    target.CpuHandle = output->GetRTV()->CPUHandle.ptr;
    GFSDK_SSAO_Output_D3D12 destination;
    destination.pRenderTargetView = &target;
    cmd->SetDescriptorHeaps({slot.ShaderHeap.get()});
    Check(slot.Context->RenderAO(device->GetCommandQueue()->GetCommandQueue().Get(), cmd->GetCommandList().Get(),
        input, _parameters, destination), "render AO");
    cmd->EndPixEvent();
}

void GDX12HBAOPlusPass::OnResourcesRecreated(PassInitContext&)
{ _contexts.clear(); }

void GDX12HBAOPlusPass::Check(GFSDK_SSAO_Status result, const char* operation)
{
    if (result != GFSDK_SSAO_OK)
    { throw std::runtime_error(std::string("ERROR: HBAO+ failed to ") + operation + " (SDK status " + std::to_string(result) + ")."); }
}
