#define FFX_CACAO_ENABLE_D3D12
#include "Engine.RendererDX12/RenderPasses/GDX12FFXCACAOPass.h"
#include <cmath>
#include <cstring>

GDX12FFXCACAOPass::GDX12FFXCACAOPass()
{ _flags = RENDER_PASS_FLAG_USE_CAMERAS; }

GDX12FFXCACAOPass::~GDX12FFXCACAOPass()
{
    if (_screenResourcesReady) { FFX_CACAO_D3D12DestroyScreenSizeDependentResources(_context); }
    if (_context) { FFX_CACAO_D3D12DestroyContext(_context); }
    if (_descriptorsStart != UINT_MAX)
    { _resources->SRV_UAV_Heap->ReleaseRange(_descriptorsStart, FFX_CACAO_D3D12_DESCRIPTOR_COUNT); }
}

void GDX12FFXCACAOPass::Describe(PassDescription& description) const
{
    description.AddParameter("Radius", 50.0f);
    description.AddParameter("Intensity", 1.0f);
    description.AddParameter("Quality", FFX_CACAO_QUALITY_HIGH);
    description.AddParameter("BlurPasses", 2);
    description.AddParameter("Sharpness", 0.98f);
    description.AddParameter("HorizonBias", 0.06f);
    description.AddParameter("ShadowPower", 1.5f);
    description.AddParameter("ShadowClamp", 0.98f);
    description.AddParameter("DetailShadowStrength", 0.5f);
    description.AddParameter("AdaptiveQualityLimit", 0.45f);
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

void GDX12FFXCACAOPass::Initialize(PassInitContext& context)
{
    auto settings = FFX_CACAO_DEFAULT_SETTINGS;
    const float radius = context.GetParameter<float>("Radius");
    const float fadeStart = context.GetParameter<float>("FadeStart");
    const float fadeEnd = context.GetParameter<float>("FadeEnd");
    const int blurPasses = context.GetParameter<int>("BlurPasses");
    settings.shadowMultiplier = context.GetParameter<float>("Intensity");
    settings.qualityLevel = context.GetParameter<FFX_CACAO_Quality>("Quality");
    settings.sharpness = context.GetParameter<float>("Sharpness");
    settings.horizonAngleThreshold = context.GetParameter<float>("HorizonBias");
    settings.shadowPower = context.GetParameter<float>("ShadowPower");
    settings.shadowClamp = context.GetParameter<float>("ShadowClamp");
    settings.detailShadowStrength = context.GetParameter<float>("DetailShadowStrength");
    settings.adaptiveQualityLimit = context.GetParameter<float>("AdaptiveQualityLimit");
    const auto inRange = [](float value, float minimum, float maximum)
    { return std::isfinite(value) && value >= minimum && value <= maximum; };
    if (!std::isfinite(radius) || radius <= 0 || !std::isfinite(fadeStart) || fadeStart < 0 ||
        !std::isfinite(fadeEnd) || fadeEnd <= fadeStart || !inRange(settings.shadowMultiplier, 0, 5) ||
        !inRange(settings.sharpness, 0, 1) || !inRange(settings.horizonAngleThreshold, 0, 0.2f) ||
        !inRange(settings.shadowPower, 0.5f, 5) || !inRange(settings.shadowClamp, 0, 1) ||
        !inRange(settings.detailShadowStrength, 0, 5) || !inRange(settings.adaptiveQualityLimit, 0, 1) ||
        settings.qualityLevel < FFX_CACAO_QUALITY_LOWEST || settings.qualityLevel > FFX_CACAO_QUALITY_HIGHEST ||
        blurPasses < 0 || blurPasses > 8)
    { throw std::invalid_argument("ERROR: CACAO requires positive Radius, 0 <= FadeStart < FadeEnd, Intensity/DetailShadowStrength in [0,5], Sharpness/ShadowClamp/AdaptiveQualityLimit in [0,1], HorizonBias in [0,0.2], ShadowPower in [0.5,5], Quality Lowest..Highest and BlurPasses in [0,8]. Distances use scene centimetres."); }

    // As in Bistro, use metres for CACAO's FP16 internal view depths.
    settings.radius = radius * 0.01f;
    settings.fadeOutFrom = fadeStart * 0.01f;
    settings.fadeOutTo = fadeEnd * 0.01f;
    settings.blurPassCount = static_cast<UINT>(blurPasses);
    _resources = context.GetDeviceResources();
    _descriptorsStart = _resources->SRV_UAV_Heap->AllocateRange(FFX_CACAO_D3D12_DESCRIPTOR_COUNT, RendererResources_StartIndex);
    _contextMemory.resize(FFX_CACAO_D3D12GetContextSize());
    auto* sdk = reinterpret_cast<FFX_CACAO_D3D12Context*>(_contextMemory.data());
    Check(FFX_CACAO_D3D12InitContext(sdk, context.GetDevice()->GetDevice().Get(),
        static_cast<UINT>(_resources->FrameConstants.size()), _resources->SRV_UAV_Heap->GetHeap().Get(), _descriptorsStart), "create context");
    _context = sdk;
    Check(FFX_CACAO_D3D12UpdateSettings(_context, &settings), "set parameters");
    CreateScreenResources(context);
}

void GDX12FFXCACAOPass::CreateScreenResources(PassInitContext& context)
{
    const auto size = context.GetOutputResourceDescription("OutAmbientOcclusion").GetSize();
    // CACAO's deinterleaved depth needs four mip levels.
    if ((std::max)(size.x, size.y) < 16)
    { throw std::invalid_argument("ERROR: CACAO requires at least one input dimension to be 16 pixels or greater."); }
    if (_screenResourcesReady)
    {
        Check(FFX_CACAO_D3D12DestroyScreenSizeDependentResources(_context), "release screen resources");
        _screenResourcesReady = false;
    }
    FFX_CACAO_D3D12ScreenSizeInfo info = {};
    info.width = size.x;
    info.height = size.y;
    Check(FFX_CACAO_D3D12InitScreenSizeDependentResources(_context, &info), "create screen resources");
    _screenResourcesReady = true;
}

void GDX12FFXCACAOPass::OnResourcesRecreated(PassInitContext& context)
{ CreateScreenResources(context); }

void GDX12FFXCACAOPass::Execute(PassExecutionContext& context)
{
    auto* cmd = context.GetCommandList();
    const auto* frame = context.GetFrameData();
    const auto size = context.GetOutputResourceDescription("OutAmbientOcclusion").GetSize();
    Matrix projection = frame->CameraViewToClip;
    projection._31 += frame->ActiveCameraJitterOffsetX * 2.0f / size.x;
    projection._32 -= frame->ActiveCameraJitterOffsetY * 2.0f / size.y;
    projection._43 *= 0.01f;
    // The SDK reconstructs positive-Z positions. Its HLSL matrix is column-major.
    const Matrix worldToView = (frame->WorldToCameraView * Matrix::CreateScale(1, 1, -1)).Transpose();
    FFX_CACAO_Matrix4x4 proj, normalsToView;
    std::memcpy(&proj, &projection, sizeof(proj));
    std::memcpy(&normalsToView, &worldToView, sizeof(normalsToView));
    const FFX_CACAO_D3D12Views views = {
        context.GetInputTexture("InDepthBuffer").GetSRV()->GPUHandle,
        context.GetInputTexture("InNormalRoughness").GetSRV()->GPUHandle,
        context.GetOutputTexture("OutAmbientOcclusion").GetUAV()->GPUHandle
    };
    cmd->BeginPixEvent("FidelityFX CACAO", Colors::DarkSlateBlue);
    cmd->SetDescriptorHeaps({_resources->SRV_UAV_Heap.get()});
    Check(FFX_CACAO_D3D12Draw(_context, cmd->GetCommandList().Get(), &proj, &normalsToView, &views), "render AO");
    cmd->InvalidateCachedState();
    cmd->EndPixEvent();
}

void GDX12FFXCACAOPass::Check(FFX_CACAO_Status result, const char* operation)
{
    if (result != FFX_CACAO_STATUS_OK)
    { throw std::runtime_error(std::string("ERROR: CACAO failed to ") + operation + " (SDK status " + std::to_string(result) + ")."); }
}
