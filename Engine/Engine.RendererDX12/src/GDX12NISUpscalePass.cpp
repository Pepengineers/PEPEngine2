#include "Engine.RendererDX12/RenderPasses/GDX12NISUpscalePass.h"
#include "Common/RuntimePaths.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <nis/NIS_Config.h>
#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>

namespace
{
    constexpr auto ColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr auto CoefficientFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
    constexpr UINT ConfigWords = offsetof(NISConfig, reserved1) / sizeof(UINT) + 1;
    constexpr float Sharpness = 0.1f;

    ComPtr<ID3DBlob> GetScalerShader(GDX12Device* device, UINT blockWidth, UINT blockHeight, UINT threads)
    {
        // Quality changes the input grid, while the DXC variant depends on the dispatch shape and target.
        using Variant = std::tuple<std::string, UINT, UINT, UINT>;
        static std::map<Variant, ComPtr<ID3DBlob>> shaders;
        static std::mutex mutex;
        const Variant variant{GDX12ShaderCompiler::GetShaderTargetForModel(device, "cs"), blockWidth, blockHeight, threads};
        std::scoped_lock lock(mutex);
        const auto cached = shaders.find(variant);
        if (cached != shaders.end()) return cached->second;

        const auto width = std::to_string(blockWidth);
        const auto height = std::to_string(blockHeight);
        const auto groupSize = std::to_string(threads);
        const D3D_SHADER_MACRO defines[] = {
            {"NIS_HDR_MODE", "1"},
            {"NIS_BLOCK_WIDTH", width.c_str()},
            {"NIS_BLOCK_HEIGHT", height.c_str()},
            {"NIS_THREAD_GROUP_SIZE", groupSize.c_str()},
            {nullptr, nullptr}
        };
        const auto shader = Engine::Core::GetExecutableDirectory() / L"Shaders" / L"NIS" / L"NIS_Main.hlsl";
        auto bytecode = GDX12ShaderCompiler::GetInstance().CompileShader(device, shader.wstring(), defines, "main", "cs");
        return shaders.emplace(variant, std::move(bytecode)).first->second;
    }
}

GDX12NISUpscalePass::GDX12NISUpscalePass(float ratio) : _ratio(ratio)
{
    _flags = RENDER_PASS_FLAG_UPSCALER;
    _numInputs = 3;
    _numOutputs = 1;
    _outputs = {&_output};
}

void GDX12NISUpscalePass::Setup(GDX12DeviceResources* resources, RenderPipelineCommonData* common)
{
    GDX12RenderPass::Setup(resources, common);
    auto* device = resources->Device->GetDevice().Get();
    D3D12_FEATURE_DATA_FORMAT_SUPPORT color = {ColorFormat};
    D3D12_FEATURE_DATA_FORMAT_SUPPORT coefficients = {CoefficientFormat};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &color, sizeof(color))) ||
        FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &coefficients, sizeof(coefficients))) ||
        !(color.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) ||
        !(color.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) ||
        !(coefficients.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD))
        throw std::runtime_error("NIS texture formats are unavailable on the post adapter");

    QueryRenderTargetResolution();
    try { CreateScaler(); }
    catch (const DxException& error) { throw std::runtime_error("NIS: " + WStringToString(error.ToString())); }
}

void GDX12NISUpscalePass::QueryRenderTargetResolution()
{
    const auto scale = [this](UINT size)
    {
        // The SDK accepts a maximum 2x scale, including when the output has odd dimensions.
        return (std::max)((size + 1) / 2, static_cast<UINT>(size / _ratio));
    };
    _commonData->DownscaledWidth = scale(_commonData->WindowWidth);
    _commonData->DownscaledHeight = scale(_commonData->WindowHeight);
}

void GDX12NISUpscalePass::CreateScaler()
{
    auto* device = _resources->Device;
    DXGI_ADAPTER_DESC1 adapter = {};
    ThrowIfFailed(device->GetAdapter()->GetDesc1(&adapter));
    auto architecture = NISGPUArchitecture::AMD_Generic;
    if (adapter.VendorId == 0x10de) architecture = NISGPUArchitecture::NVIDIA_Generic;
    else if (adapter.VendorId == 0x8086) architecture = NISGPUArchitecture::Intel_Generic;
    NISOptimizer optimizer(true, architecture);
    _blockWidth = optimizer.GetOptimalBlockWidth();
    _blockHeight = optimizer.GetOptimalBlockHeight();

    GDX12RootSignatureDesc root;
    root.Constants = {ConfigWords};
    root.NumSingleSRVSlots = 3;
    root.NumSingleUAVSlots = 1;
    root.StaticSamplers = {CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP)};
    _root = std::make_unique<GDX12RootSignature>(device, root);
    const auto shader = GetScalerShader(device, _blockWidth, _blockHeight, optimizer.GetOptimalThreadGroupSize());
    _pipeline = GDX12RenderHelpers::CreateComputePipeline(device, _root.get(), *shader.Get());

    constexpr UINT width = kFilterSize / 4;
    constexpr UINT height = kPhaseCount;
    const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(CoefficientFormat, width, height, 1, 1);
    auto* queue = device->GetCommandQueue();
    const float* tables[] = {&coef_scale[0][0], &coef_usm[0][0]};
    auto* commands = queue->GetCommandList();
    try
    {
        for (UINT i = 0; i < _coefficients.size(); ++i)
        {
            auto texture = device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COPY_DEST);
            const auto uploadSize = GetRequiredIntermediateSize(texture.Get(), 0, 1);
            auto upload = device->CreateResource(D3D12_HEAP_TYPE_UPLOAD, CD3DX12_RESOURCE_DESC::Buffer(uploadSize),
                D3D12_RESOURCE_STATE_GENERIC_READ);
            commands->RetainResource(texture);
            commands->RetainResource(upload);
            const D3D12_SUBRESOURCE_DATA source = {tables[i], kFilterSize * sizeof(float), sizeof(coef_scale)};
            if (!UpdateSubresources<1>(commands->GetCommandList().Get(), texture.Get(), upload.Get(), 0, 0, 1, &source))
                throw std::runtime_error("NIS coefficient upload failed");
            commands->ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(texture.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)});

            GDX12TextureDesc views;
            views.Width = width;
            views.Height = height;
            views.Format = CoefficientFormat;
            views.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
            views.SRVHeapIndex = views.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
            views.SRVDesc = {CoefficientFormat, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
            views.SRVDesc.Texture2D.MipLevels = 1;
            views.ExternalResource = std::move(texture);
            _coefficients[i].Initialize(std::move(views));
        }
        queue->ExecuteCommandList(commands);
    }
    catch (...)
    {
        queue->DiscardCommandList(commands);
        throw;
    }
    queue->CPUWaitForFenceValue(commands->FenceValue);
    queue->CollectCompletedLists();
}

void GDX12NISUpscalePass::Initialize()
{
    if (_inputs[2]->GetTexture()->GetFormat() != ColorFormat)
        throw std::invalid_argument("NIS requires linear RGBA16F HDR input");
    GDX12RenderHelpers::InitializeComputeTexture(_output, *_resources,
        _commonData->WindowWidth, _commonData->WindowHeight, ColorFormat);
}

void GDX12NISUpscalePass::Resize()
{
    _output.Resize(_commonData->WindowWidth, _commonData->WindowHeight);
}

void GDX12NISUpscalePass::Execute(GDX12CommandList* commands)
{
    auto* input = _inputs[2]->GetTexture();
    const auto inputWidth = input->GetWidth(), inputHeight = input->GetHeight();
    const auto width = _output.GetWidth(), height = _output.GetHeight();
    NISConfig config = {};
    if (!NVScalerUpdateConfig(config, Sharpness, 0, 0, inputWidth, inputHeight, inputWidth, inputHeight,
        0, 0, width, height, width, height, NISHDRMode::Linear))
        throw std::invalid_argument("NIS input-to-output scale is outside the SDK range");

    GDX12_SCOPED_EVENT(commands, "NIS HDR Upscale", Colors::Blue);
    commands->ResourceBarrier({input->GetResource()->GetNonPixelShaderResourceBarrier(),
        _output.GetResource()->GetUnorderedAccessBarrier()});
    commands->SetComputeRootSignature(_root.get());
    commands->SetDescriptorHeaps({_resources->SRV_UAV_Heap.get()});
    commands->GetCommandList()->SetComputeRoot32BitConstants(0, ConfigWords, &config, 0);
    commands->SetComputeSRV(0, input->GetSRV()->GPUHandle);
    commands->SetComputeSRV(1, _coefficients[0].GetSRV()->GPUHandle);
    commands->SetComputeSRV(2, _coefficients[1].GetSRV()->GPUHandle);
    commands->SetComputeUAV(0, _output.GetUAV()->GPUHandle);
    commands->SetPipelineState(_pipeline.Get());
    commands->Dispatch((width + _blockWidth - 1) / _blockWidth, (height + _blockHeight - 1) / _blockHeight, 1);
    commands->ResourceBarrier({_output.GetResource()->GetUAVBarrier()});
}
