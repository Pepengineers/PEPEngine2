#include "Engine.RendererDX12/RenderPasses/GDX12FidelityFXPasses.h"
#include "Engine.RendererDX12/RenderSettings.h"
#include "Common/ConsoleVariables.h"
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <FidelityFX/host/ffx_sssr.h>
#include <FidelityFX/host/ffx_dof.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace
{
    using RenderSettings::Quality;
    constexpr auto Runtime = EConsoleVariableFlags::Default;
    bool ValidQuality(Quality value) { return value >= Quality::Off && value <= Quality::High; }
    bool Positive(float value) { return value > 0 && value <= 1000000; }
    bool Unit(float value) { return value >= 0 && value <= 1; }
    bool Temporal(float value) { return value >= 0 && value < 1; }
    bool Traversal(int value) { return value == -1 || (value >= 8 && value <= 1024); }
    bool QuadSamples(int value) { return value == -1 || value == 1 || value == 2 || value == 4; }
    bool Aperture(float value) { return value >= 0.5f && value <= 64; }
    bool CocLimit(float value) { return value == -1 || (value >= 1 && value <= 256); }

    static AutoConsoleVariable<bool> ssrEnabled(L"Render.SSR.Enabled", true, L"Enable FidelityFX SSSR");
    static AutoConsoleVariable<Quality> ssrQuality(L"Render.SSR.Quality", Quality::Mid,
        L"0 Off, 1 Low, 2 Mid, 3 High", Runtime, ValidQuality);
    static AutoConsoleVariable<float> ssrThickness(L"Render.SSR.Thickness", 0.1f,
        L"View-space ray hit thickness in world units", Runtime, Positive);
    static AutoConsoleVariable<float> ssrRoughness(L"Render.SSR.RoughnessThreshold", 0.8f,
        L"Maximum perceptual roughness for traced rays", Runtime, Unit);
    static AutoConsoleVariable<int> ssrSteps(L"Render.SSR.MaxTraversalSteps", -1,
        L"-1 preset, otherwise 8..1024", Runtime, Traversal);
    static AutoConsoleVariable<float> ssrTemporal(L"Render.SSR.TemporalStability", 0.9f,
        L"History weight, [0,1)", Runtime, Temporal);
    static AutoConsoleVariable<int> ssrSamples(L"Render.SSR.SamplesPerQuad", -1,
        L"-1 preset, otherwise 1, 2 or 4", Runtime, QuadSamples);
    static AutoConsoleVariable<bool> dofEnabled(L"Render.DOF.Enabled", false, L"Enable FidelityFX depth of field");
    static AutoConsoleVariable<Quality> dofQuality(L"Render.DOF.Quality", Quality::Mid,
        L"0 Off, 1 Low, 2 Mid, 3 High", Runtime, ValidQuality);
    static AutoConsoleVariable<float> dofFocus(L"Render.DOF.FocusDistance", 1000.0f,
        L"Positive focus distance in world units (centimetres)", Runtime, Positive);
    static AutoConsoleVariable<float> dofAperture(L"Render.DOF.Aperture", 2.8f,
        L"Lens f-number, 0.5..64", Runtime, Aperture);
    static AutoConsoleVariable<float> dofCoc(L"Render.DOF.MaxCoC", -1,
        L"-1 preset, otherwise maximum full-resolution blur radius in pixels, 1..256", Runtime, CocLimit);

    struct SsrSettings
    {
        bool Enabled;
        float Thickness, Roughness, Temporal;
        uint32_t Steps, Samples;
    };
    SsrSettings GetSsr()
    {
        const auto quality = ssrQuality.GetValue();
        const size_t index = static_cast<size_t>(quality);
        constexpr uint32_t steps[] = { 0, 32, 64, 96 }, samples[] = { 0, 1, 2, 4 };
        const int manualSteps = ssrSteps.GetValue(), manualSamples = ssrSamples.GetValue();
        return { ssrEnabled.GetValue() && quality != Quality::Off, ssrThickness.GetValue(),
            ssrRoughness.GetValue(), ssrTemporal.GetValue(),
            manualSteps < 0 ? steps[index] : static_cast<uint32_t>(manualSteps),
            manualSamples < 0 ? samples[index] : static_cast<uint32_t>(manualSamples) };
    }
    struct DofSettings
    {
        bool Enabled;
        float Focus, Aperture, MaxCoC;
        uint32_t Rings;
    };
    DofSettings GetDof()
    {
        const auto quality = dofQuality.GetValue();
        const size_t index = static_cast<size_t>(quality);
        constexpr uint32_t rings[] = { 0, 3, 5, 8 };
        constexpr float limits[] = { 0, 8, 16, 32 };
        const float manual = dofCoc.GetValue();
        return { dofEnabled.GetValue() && quality != Quality::Off, dofFocus.GetValue(),
            dofAperture.GetValue(), manual < 0 ? limits[index] : manual, rings[index] };
    }
    void Check(FfxErrorCode result, const char* operation)
    {
        if (result != FFX_OK)
            throw std::runtime_error(std::string(operation) + " failed: FidelityFX error " + std::to_string(result));
    }
    void CreateOutput(GDX12DeviceResources& resources, GDX12Texture& output, GDX12Texture& input)
    {
        if (input.GetFormat() != DXGI_FORMAT_R16G16B16A16_FLOAT)
            throw std::invalid_argument("FidelityFX color input must be linear HDR RGBA16F");
        auto* heap = resources.SRV_UAV_Heap.get();
        GDX12TextureDesc desc;
        desc.Width = input.GetWidth(); desc.Height = input.GetHeight();
        desc.Format = desc.SRVDesc.Format = desc.UAVDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.Semantic = ETextureSemantic::Color;
        desc.CreateUAV = true;
        desc.SRV_UAV_Heap = heap;
        desc.SRVHeapIndex = output.IsInitialized() ? output.GetSRV()->HeapIndex :
            heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.UAVHeapIndex = output.IsInitialized() ? output.GetUAV()->HeapIndex :
            heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.SRVDesc.Texture2D.MipLevels = 1;
        desc.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        output.Initialize(desc);
    }
    void ValidateFfxInputs(const std::vector<IRenderPassLink*>& inputs, size_t count)
    {
        if (inputs.size() != count)
            throw std::invalid_argument("Incorrect FidelityFX pass input count");
        for (auto* input : inputs)
            if (!input || !input->IsInitialized()) throw std::invalid_argument("Uninitialized FidelityFX input");
        for (size_t i = 1; i < (count == 7 ? 5u : 2u); ++i)
            if (inputs[i]->GetTexture()->GetWidth() != inputs[0]->GetTexture()->GetWidth() ||
                inputs[i]->GetTexture()->GetHeight() != inputs[0]->GetTexture()->GetHeight())
                throw std::invalid_argument("FidelityFX input resolutions must match");
    }
    struct Backend
    {
        std::vector<uint8_t> Scratch;
        FfxInterface Interface = {};
        void Initialize(GDX12Device& device, size_t contexts)
        {
            Scratch.assign(ffxGetScratchMemorySizeDX12(contexts), 0);
            Interface = {};
            Check(ffxGetInterfaceDX12(&Interface, ffxGetDeviceDX12(device.GetDevice().Get()),
                Scratch.data(), Scratch.size(), contexts), "Create DX12 backend");
        }
    };
    FfxResource Resource(GDX12CommandList* commands, GDX12Texture& texture)
    {
        commands->ResourceBarrier({ texture.GetResource()->GetSRVBarrier() });
        auto* native = texture.GetResource()->D3DResource.Get();
        return ffxGetResourceDX12(native, ffxGetResourceDescriptionDX12(native),
            L"PEPEngine2", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    }
    void MatrixForFfx(float (&destination)[16], const Matrix& rowVectorTransform)
    {
        // Column-major storage of the equivalent column-vector transform equals our row-major bytes.
        std::memcpy(destination, &rowVectorTransform._11, sizeof(destination));
    }
    Matrix Projection(const RenderPipelineCommonData& common, UINT width, UINT height)
    {
        Matrix result = common.CameraViewToClip;
        result._31 += common.ActiveCameraJitterOffsetX * 2.0f / width;
        result._32 -= common.ActiveCameraJitterOffsetY * 2.0f / height;
        return result;
    }
}

struct GDX12SssrPass::Data
{
    GDX12Texture Output;
    Backend BackendData;
    std::unique_ptr<FfxSssrContext> Context;
    std::unique_ptr<GDX12RootSignature> ClearRoot;
    ComPtr<ID3D12PipelineState> ClearPipeline;
    SsrSettings Settings = {};
    uint64_t Revision = UINT64_MAX;
    uint32_t DispatchCount = 0;
    bool WasEnabled = false, Prepared = false;
    void Destroy()
    {
        if (Context) { Check(ffxSssrContextDestroy(Context.get()), "Destroy SSSR"); Context.reset(); }
        DispatchCount = 0;
    }
};

GDX12SssrPass::GDX12SssrPass() : _data(std::make_unique<Data>())
{
    _flags = RENDER_PASS_FLAG_USE_CAMERAS;
    _numInputs = 7; _numOutputs = 1;
    _outputs.push_back(&_data->Output);
}
GDX12SssrPass::~GDX12SssrPass()
{
    if (_data && _data->Context)
    {
        _resources->Device->GetCommandQueue()->Flush();
        ffxSssrContextDestroy(_data->Context.get());
    }
}
void GDX12SssrPass::Initialize()
{
    ValidateFfxInputs(_inputs, 7);
    auto& d = *_data;
    CreateOutput(*_resources, d.Output, *_inputs[0]->GetTexture());
    GDX12RootSignatureDesc root;
    root.NumSingleUAVSlots = 1;
    d.ClearRoot = std::make_unique<GDX12RootSignature>(_resources->Device, root);
    auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(_resources->Device,
        SHADERS_FOLDER "FidelityFXUtility.hlsl", nullptr, "ClearReflectionsCS", "cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
    pipeline.pRootSignature = d.ClearRoot->GetRootSignature().Get();
    pipeline.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
    ThrowIfFailed(_resources->Device->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&d.ClearPipeline)));
}
void GDX12SssrPass::PrepareFrame()
{
    auto& d = *_data;
    d.Settings = GetSsr();
    if (!d.Settings.Enabled) d.DispatchCount = 0;
    if (d.Settings.Enabled && (!d.Context || !d.WasEnabled || d.Revision != _commonData->HistoryRevision))
    {
        if (d.Context) { _resources->Device->GetCommandQueue()->Flush(); d.Destroy(); }
        d.BackendData.Initialize(*_resources->Device, FFX_SSSR_CONTEXT_COUNT);
        auto context = std::make_unique<FfxSssrContext>();
        FfxSssrContextDescription description = {};
        description.flags = FFX_SSSR_ENABLE_DEPTH_INVERTED;
        description.renderSize = { d.Output.GetWidth(), d.Output.GetHeight() };
        description.normalsHistoryBufferFormat = ffxGetSurfaceFormatDX12(_inputs[3]->GetTexture()->GetFormat());
        description.backendInterface = d.BackendData.Interface;
        Check(ffxSssrContextCreate(context.get(), &description), "Create SSSR");
        d.Context = std::move(context);
        d.Revision = _commonData->HistoryRevision;
    }
    d.WasEnabled = d.Settings.Enabled;
    d.Prepared = true;
}
bool GDX12SssrPass::IsEnabled() const { return _data && _data->Prepared && _data->Settings.Enabled; }
bool GDX12SssrPass::HasValidHistory() const
{
    // Pinned SDK 1.1.4 / SSSR 1.5 emits a cold denoiser frame after context creation.
    // The resolve uses its environment/probe fallback until the second dispatch completes.
    return IsEnabled() && _data->DispatchCount >= 2;
}
void GDX12SssrPass::Execute(GDX12CommandList* commands)
{
    auto& d = *_data;
    if (!d.Prepared) throw std::logic_error("SSSR PrepareFrame must run before command recording");
    commands->BeginPixEvent("FidelityFX SSSR", Colors::SteelBlue);
    if (!d.Settings.Enabled)
    {
        commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
        commands->SetComputeRootSignature(d.ClearRoot.get());
        commands->SetPipelineState(d.ClearPipeline);
        commands->ResourceBarrier({ d.Output.GetResource()->GetUnorderedAccessBarrier() });
        commands->SetComputeUAV(0, d.Output.GetUAV()->GPUHandle);
        commands->Dispatch((d.Output.GetWidth() + 7) / 8, (d.Output.GetHeight() + 7) / 8, 1);
        commands->ResourceBarrier({ d.Output.GetResource()->GetSRVBarrier() });
    }
    else
    {
        FfxSssrDispatchDescription dispatch = {};
        dispatch.commandList = ffxGetCommandListDX12(commands->GetCommandList().Get());
        dispatch.color = Resource(commands, *_inputs[0]->GetTexture());
        dispatch.depth = Resource(commands, *_inputs[1]->GetTexture());
        dispatch.motionVectors = Resource(commands, *_inputs[2]->GetTexture());
        dispatch.normal = Resource(commands, *_inputs[3]->GetTexture());
        dispatch.materialParameters = Resource(commands, *_inputs[4]->GetTexture());
        dispatch.environmentMap = Resource(commands, *_inputs[5]->GetTexture());
        dispatch.brdfTexture = Resource(commands, *_inputs[6]->GetTexture());
        dispatch.output = Resource(commands, d.Output);
        const Matrix projection = Projection(*_commonData, d.Output.GetWidth(), d.Output.GetHeight());
        MatrixForFfx(dispatch.projection, projection);
        MatrixForFfx(dispatch.invProjection, projection.Invert());
        MatrixForFfx(dispatch.view, _commonData->ActiveCameraView);
        MatrixForFfx(dispatch.invView, _commonData->ActiveCameraView.Invert());
        MatrixForFfx(dispatch.invViewProjection, _commonData->ActiveCameraViewProjection.Invert());
        MatrixForFfx(dispatch.prevViewProjection, _commonData->ActiveCameraPreviousViewProjection);
        dispatch.renderSize = { d.Output.GetWidth(), d.Output.GetHeight() };
        dispatch.motionVectorScale = { 0.5f, -0.5f };
        dispatch.iblFactor = 1;
        dispatch.normalUnPackMul = 1;
        dispatch.normalUnPackAdd = 0;
        dispatch.roughnessChannel = 3;
        dispatch.isRoughnessPerceptual = true;
        dispatch.temporalStabilityFactor = d.Settings.Temporal;
        dispatch.depthBufferThickness = d.Settings.Thickness;
        dispatch.roughnessThreshold = d.Settings.Roughness;
        dispatch.varianceThreshold = 0.01f;
        dispatch.maxTraversalIntersections = d.Settings.Steps;
        dispatch.minTraversalOccupancy = 4;
        dispatch.mostDetailedMip = 0;
        dispatch.samplesPerQuad = d.Settings.Samples;
        dispatch.temporalVarianceGuidedTracingEnabled = true;
        const auto result = ffxSssrContextDispatch(d.Context.get(), &dispatch);
        commands->InvalidateCachedState();
        Check(result, "Dispatch SSSR");
        d.DispatchCount = (std::min)(d.DispatchCount + 1, 2u);
    }
    commands->EndPixEvent();
}
void GDX12SssrPass::Resize()
{
    auto& d = *_data;
    _resources->Device->GetCommandQueue()->Flush();
    d.Destroy();
    CreateOutput(*_resources, d.Output, *_inputs[0]->GetTexture());
    d.Prepared = false;
}
void GDX12SssrPass::ClearDenendencies()
{
    if (_data && _data->Context) { _resources->Device->GetCommandQueue()->Flush(); _data->Destroy(); }
    GDX12RenderPass::ClearDenendencies();
    _data.reset();
}

struct GDX12DepthOfFieldPass::Data
{
    GDX12Texture Output;
    Backend BackendData;
    std::unique_ptr<FfxDofContext> Context;
    DofSettings Settings = {};
    uint32_t Rings = 0;
    float MaxCoC = 0;
    bool Prepared = false;
    void Destroy()
    {
        if (Context) { Check(ffxDofContextDestroy(Context.get()), "Destroy DOF"); Context.reset(); }
    }
};
GDX12DepthOfFieldPass::GDX12DepthOfFieldPass() : _data(std::make_unique<Data>())
{
    _flags = RENDER_PASS_FLAG_USE_CAMERAS;
    _numInputs = 2; _numOutputs = 1;
    _outputs.push_back(&_data->Output);
}
GDX12DepthOfFieldPass::~GDX12DepthOfFieldPass()
{
    if (_data && _data->Context)
    {
        _resources->Device->GetCommandQueue()->Flush();
        ffxDofContextDestroy(_data->Context.get());
    }
}
void GDX12DepthOfFieldPass::Initialize()
{
    ValidateFfxInputs(_inputs, 2);
    CreateOutput(*_resources, _data->Output, *_inputs[0]->GetTexture());
}
bool GDX12DepthOfFieldPass::IsEnabled() const { return _data && _data->Prepared && _data->Settings.Enabled; }
void GDX12DepthOfFieldPass::PrepareFrame()
{
    auto& d = *_data;
    d.Settings = GetDof();
    if (d.Settings.Enabled && (!d.Context || d.Rings != d.Settings.Rings || d.MaxCoC != d.Settings.MaxCoC))
    {
        if (d.Context) { _resources->Device->GetCommandQueue()->Flush(); d.Destroy(); }
        d.BackendData.Initialize(*_resources->Device, FFX_DOF_CONTEXT_COUNT);
        auto context = std::make_unique<FfxDofContext>();
        FfxDofContextDescription description = {};
        description.flags = FFX_DOF_REVERSE_DEPTH;
        description.resolution = { d.Output.GetWidth(), d.Output.GetHeight() };
        description.quality = d.Settings.Rings;
        description.cocLimitFactor = d.Settings.MaxCoC / d.Output.GetHeight();
        description.backendInterface = d.BackendData.Interface;
        Check(ffxDofContextCreate(context.get(), &description), "Create DOF");
        d.Context = std::move(context);
        d.Rings = d.Settings.Rings; d.MaxCoC = d.Settings.MaxCoC;
    }
    d.Prepared = true;
}
void GDX12DepthOfFieldPass::Execute(GDX12CommandList* commands)
{
    auto& d = *_data;
    if (!d.Prepared) throw std::logic_error("DOF PrepareFrame must run before command recording");
    commands->BeginPixEvent("FidelityFX depth of field", Colors::MediumPurple);
    if (!d.Settings.Enabled)
    {
        auto* input = _inputs[0]->GetTexture();
        commands->ResourceBarrier({ input->GetResource()->GetCopySourceBarrier(), d.Output.GetResource()->GetCopyDestBarrier() });
        commands->CopyResource(d.Output.GetResource()->D3DResource.Get(), input->GetResource()->D3DResource.Get());
        commands->ResourceBarrier({ input->GetResource()->GetSRVBarrier(), d.Output.GetResource()->GetSRVBarrier() });
    }
    else
    {
        const Matrix projection = _commonData->CameraViewToClip;
        if (!(projection._11 > 0) || !std::isfinite(projection._11) || std::abs(projection._43) < 1e-8f)
            throw std::invalid_argument("DOF requires a finite perspective camera projection");
        constexpr float sensorWidthCentimeters = 3.6f;
        const float focalLength = sensorWidthCentimeters * projection._11 * 0.5f;
        if (d.Settings.Focus <= focalLength)
            throw std::invalid_argument("DOF focus distance must exceed the camera focal length");
        const float apertureRadius = focalLength / (2 * d.Settings.Aperture);
        const float conversion = 0.5f * d.Output.GetWidth() / sensorWidthCentimeters;
        FfxDofDispatchDescription dispatch = {};
        dispatch.commandList = ffxGetCommandListDX12(commands->GetCommandList().Get());
        dispatch.color = Resource(commands, *_inputs[0]->GetTexture());
        dispatch.depth = Resource(commands, *_inputs[1]->GetTexture());
        dispatch.output = Resource(commands, d.Output);
        dispatch.cocScale = ffxDofCalculateCocScale(apertureRadius, -d.Settings.Focus, focalLength,
            conversion, projection._33, projection._43, projection._34);
        dispatch.cocBias = ffxDofCalculateCocBias(apertureRadius, -d.Settings.Focus, focalLength,
            conversion, projection._33, projection._43, projection._34);
        const auto result = ffxDofContextDispatch(d.Context.get(), &dispatch);
        commands->InvalidateCachedState();
        Check(result, "Dispatch DOF");
    }
    commands->EndPixEvent();
}
void GDX12DepthOfFieldPass::Resize()
{
    auto& d = *_data;
    _resources->Device->GetCommandQueue()->Flush();
    d.Destroy();
    CreateOutput(*_resources, d.Output, *_inputs[0]->GetTexture());
    d.Prepared = false;
}
void GDX12DepthOfFieldPass::ClearDenendencies()
{
    if (_data && _data->Context) { _resources->Device->GetCommandQueue()->Flush(); _data->Destroy(); }
    GDX12RenderPass::ClearDenendencies();
    _data.reset();
}
