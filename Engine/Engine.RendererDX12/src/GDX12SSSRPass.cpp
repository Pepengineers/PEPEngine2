#include "Engine.RendererDX12/RenderPasses/GDX12SSSRPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12SSRSettings.h"
#include <FidelityFX/host/ffx_sssr.h>
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <algorithm>
#include <stdexcept>

namespace
{
    void CheckFFX(FfxErrorCode result)
    {
        if (result != FFX_OK)
            throw std::runtime_error("FidelityFX SSSR failed: " + std::to_string(result));
    }

    FfxResource SDKResource(GDX12Texture* texture)
    {
        auto* resource = texture->GetResource()->D3DResource.Get();
        auto description = ffxGetResourceDescriptionDX12(resource);
        const auto state = texture->GetResource()->GetCurrentState();
        const auto combinedRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE && state != combinedRead)
            throw std::logic_error("SSSR resource must enter SDK in shader-read state");
        return ffxGetResourceDX12(resource, description, L"SSSR",
            state == combinedRead ? FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ : FFX_RESOURCE_STATE_COMPUTE_READ);
    }

    void CopyMatrix(float* output, const Matrix& matrix)
    {
        // Row-vector row-major bytes equal the SDK's column-vector column-major representation.
        memcpy(output, &matrix, sizeof(Matrix));
    }
}

GDX12SSSRPass::GDX12SSSRPass()
{
    _flags = RENDER_PASS_FLAG_USE_CAMERAS;
    _numInputs = 7;
    _numOutputs = 1;
    _outputs.push_back(&_color);
}

GDX12SSSRPass::~GDX12SSSRPass()
{
    DestroyContext();
}

void GDX12SSSRPass::Initialize()
{
    for (auto [texture, format] : { std::pair{ &_color, DXGI_FORMAT_R16G16B16A16_FLOAT },
        { &_fallback, DXGI_FORMAT_R16G16B16A16_FLOAT }, { &_radiance, DXGI_FORMAT_R16G16B16A16_FLOAT }, { &_roughness, DXGI_FORMAT_R16_FLOAT },
        { &_motion, DXGI_FORMAT_R16G16_FLOAT } })
    {
        GDX12RenderHelpers::InitializeComputeTexture(*texture, *_resources,
            _inputs[0]->GetTexture()->GetWidth(), _inputs[0]->GetTexture()->GetHeight(), format, texture == &_color);
    }
    GDX12RootSignatureDesc root;
    root.NumSingleCBVSlots = 2;
    root.NumSingleSRVSlots = 11;
    root.NumSingleUAVSlots = 4;
    root.StaticSamplers = GetStaticSamplers();
    _root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
    for (auto [pipeline, entry] : { std::pair{ std::addressof(_prepare), "Prepare" }, { std::addressof(_resolve), "Resolve" } })
    {
        *pipeline = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _root.get(),
            SHADERS_FOLDER "SSSR.hlsl", entry);
    }
    _constants = std::make_unique<GDX12UploadBuffer<Constants>>(_resources->Device,
        static_cast<UINT>(_resources->FrameConstants.size()));
}

void GDX12SSSRPass::DestroyContext()
{
    if (_context)
    {
        _resources->Device->GetCommandQueue()->Flush();
        ffxSssrContextDestroy(_context.get());
        _context.reset();
    }
    _scratch.clear();
}

void GDX12SSSRPass::CreateContext()
{
    DestroyContext();
    const size_t bytes = ffxGetScratchMemorySizeDX12(FFX_SSSR_CONTEXT_COUNT);
    _scratch.assign((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0);
    FfxSssrContextDescription desc = {};
    desc.flags = FFX_SSSR_ENABLE_DEPTH_INVERTED;
    desc.renderSize = { _color.GetWidth(), _color.GetHeight() };
    desc.normalsHistoryBufferFormat = FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    CheckFFX(ffxGetInterfaceDX12(&desc.backendInterface,
        ffxGetDeviceDX12(_resources->Device->GetDevice().Get()), _scratch.data(), bytes, FFX_SSSR_CONTEXT_COUNT));
    auto context = std::make_unique<FfxSssrContext>();
    CheckFFX(ffxSssrContextCreate(context.get(), &desc));
    _context = std::move(context);
}

void GDX12SSSRPass::Execute(GDX12CommandList* commands)
{
    const auto settings = GetSSRSettings();
    auto* scene = _inputs[0]->GetTexture();
    if (!settings.Enabled)
    {
        commands->ResourceBarrier({ scene->GetResource()->GetCopySourceBarrier(), _color.GetResource()->GetCopyDestBarrier() });
        commands->CopyResource(_color.GetResource()->D3DResource.Get(), scene->GetResource()->D3DResource.Get());
        _wasEnabled = false;
        return;
    }
    std::array<ID3D12Resource*, 7> sources;
    for (size_t i = 0; i < sources.size(); ++i)
        sources[i] = _inputs[i]->GetTexture()->GetResource()->D3DResource.Get();
    const Matrix view = _commonData->CameraView;
    const Matrix projection = _commonData->CameraProjection;
    const Matrix viewProjection = view * projection;
    if (!_context || !_wasEnabled || _historyRevision != _commonData->HistoryRevision || sources != _sources ||
        _environmentRevision != _resources->ReflectionEnvironmentRevision)
    {
        // SDK 1.1.4 has no reset dispatch flag. Recreate only at actual history discontinuities.
        CreateContext();
        _previousViewProjection = viewProjection;
        _previousJitter = { _commonData->ActiveCameraJitterOffsetX, _commonData->ActiveCameraJitterOffsetY };
    }
    _environmentRevision = _resources->ReflectionEnvironmentRevision;
    _sources = sources;
    _historyRevision = _commonData->HistoryRevision;
    _wasEnabled = true;
    GDX12_SCOPED_EVENT(commands, "AMD SSSR + Reflections Denoiser + specular resolve", Colors::Cyan);
    const UINT slot = _resources->CurrFrameConstantsIndex;
    const XMFLOAT2 jitter = { _commonData->ActiveCameraJitterOffsetX, _commonData->ActiveCameraJitterOffsetY };
    Constants constants;
    constants.JitterDeltaUV = { (_previousJitter.x - jitter.x) / _color.GetWidth(),
        (_previousJitter.y - jitter.y) / _color.GetHeight() };
    constants.RoughnessCutoff = settings.RoughnessCutoff;
    _constants->CopyData(slot, constants);
    auto bind = [&]
    {
        commands->SetComputeRootSignature(_root.get());
        commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
        commands->SetComputeRootConstantBufferView(0,
            _resources->FrameConstants[slot]->CameraCB->GetElementAddress(_commonData->ActiveCameraCBufferIndex));
        commands->SetComputeRootConstantBufferView(1, _constants->GetElementAddress(slot));
        commands->SetComputeSRV(9, _resources->ReflectionRadianceSRV.GPUHandle);
        commands->SetComputeSRV(10, _resources->FrameConstants[slot]->ReflectionProbes->GetSRV()->GPUHandle);
        for (UINT i = 0; i < _numInputs; ++i)
        {
            auto* texture = _inputs[i]->GetTexture();
            commands->ResourceBarrier({ texture->GetResource()->GetSRVBarrier() });
            commands->SetComputeSRV(i, texture->GetSRV()->GPUHandle);
        }
    };
    bind();
    commands->SetPipelineState(_prepare);
    commands->ResourceBarrier({ _roughness.GetResource()->GetUnorderedAccessBarrier(), _motion.GetResource()->GetUnorderedAccessBarrier(), _fallback.GetResource()->GetUnorderedAccessBarrier() });
    commands->SetComputeUAV(0, _roughness.GetUAV()->GPUHandle);
    commands->SetComputeUAV(1, _motion.GetUAV()->GPUHandle);
    commands->SetComputeUAV(3, _fallback.GetUAV()->GPUHandle);
    commands->Dispatch((_color.GetWidth() + 7) / 8, (_color.GetHeight() + 7) / 8, 1);
    commands->ResourceBarrier({ _roughness.GetResource()->GetNonPixelShaderResourceBarrier(),
        _motion.GetResource()->GetNonPixelShaderResourceBarrier(), _fallback.GetResource()->GetNonPixelShaderResourceBarrier(), _radiance.GetResource()->GetNonPixelShaderResourceBarrier() });

    FfxSssrDispatchDescription desc = {};
    desc.commandList = ffxGetCommandListDX12(commands->GetCommandList().Get());
    desc.color = SDKResource(scene);
    desc.depth = SDKResource(_inputs[1]->GetTexture());
    desc.normal = SDKResource(_inputs[2]->GetTexture());
    desc.materialParameters = SDKResource(&_roughness);
    desc.motionVectors = SDKResource(&_motion);
    desc.environmentMap = SDKResource(&_fallback);
    desc.brdfTexture = SDKResource(_inputs[6]->GetTexture());
    desc.output = SDKResource(&_radiance);
    CopyMatrix(desc.invViewProjection, viewProjection.Invert());
    CopyMatrix(desc.projection, projection);
    CopyMatrix(desc.invProjection, projection.Invert());
    CopyMatrix(desc.view, view);
    CopyMatrix(desc.invView, view.Invert());
    CopyMatrix(desc.prevViewProjection, _previousViewProjection);
    desc.renderSize = { _color.GetWidth(), _color.GetHeight() };
    desc.motionVectorScale = { 1, 1 };
    desc.iblFactor = 1;
    desc.normalUnPackMul = 1;
    desc.isRoughnessPerceptual = false;
    desc.temporalStabilityFactor = settings.TemporalStability;
    desc.depthBufferThickness = settings.Thickness;
    desc.roughnessThreshold = settings.RoughnessCutoff * settings.RoughnessCutoff;
    desc.varianceThreshold = settings.VarianceThreshold;
    desc.maxTraversalIntersections = settings.MaxIntersections;
    desc.minTraversalOccupancy = settings.MinOccupancy;
    const UINT maxMip = static_cast<UINT>(std::log2((std::max)(_color.GetWidth(), _color.GetHeight())));
    desc.mostDetailedMip = (std::min)(static_cast<UINT>(settings.MostDetailedMip), maxMip);
    desc.samplesPerQuad = settings.SamplesPerQuad;
    desc.temporalVarianceGuidedTracingEnabled = settings.VarianceGuided;
    if (_resources->FrameConstants.size() > FFX_MAX_QUEUED_FRAMES)
        _resources->Device->GetCommandQueue()->Flush();
    const auto result = ffxSssrContextDispatch(_context.get(), &desc);
    // The backend restores all imported resource states, but changes native binding state.
    commands->InvalidateCachedState();
    CheckFFX(result);
    bind();
    commands->SetPipelineState(_resolve);
    commands->ResourceBarrier({ _color.GetResource()->GetUnorderedAccessBarrier() });
    commands->SetComputeSRV(7, _radiance.GetSRV()->GPUHandle);
    commands->SetComputeSRV(8, _fallback.GetSRV()->GPUHandle);
    commands->SetComputeUAV(2, _color.GetUAV()->GPUHandle);
    commands->Dispatch((_color.GetWidth() + 7) / 8, (_color.GetHeight() + 7) / 8, 1);
    commands->ResourceBarrier({ _color.GetResource()->GetSRVBarrier() });
    _previousViewProjection = viewProjection;
    _previousJitter = jitter;
}

void GDX12SSSRPass::Resize()
{
    DestroyContext();
    for (auto* texture : { &_color, &_radiance, &_roughness, &_motion, &_fallback })
        texture->Resize(_inputs[0]->GetTexture()->GetWidth(), _inputs[0]->GetTexture()->GetHeight());
    _wasEnabled = false;
}

void GDX12SSSRPass::ClearDenendencies()
{
    DestroyContext();
    GDX12RenderPass::ClearDenendencies();
    _constants.reset();
    _root.reset();
    _prepare.Reset();
    _resolve.Reset();
}
