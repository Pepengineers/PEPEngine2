#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "xess/xess.h"
#include "xess/xess_d3d12.h"
#include <stdexcept>

class GDX12XeSSUpscalePass : public GDX12RenderPass
{
public:
    explicit GDX12XeSSUpscalePass(xess_quality_settings_t quality = XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS) : _quality(quality)
    {
        _flags = RENDER_PASS_FLAG_UPSCALER | RENDER_PASS_FLAG_USE_JITTER;
        _numInputs = 3;
        _numOutputs = 1;
        _outputs.push_back(&_output);
    }

    ~GDX12XeSSUpscalePass() override { DestroyContext(); }

    void Setup(GDX12DeviceResources* resources, GDX12DeviceResources* other, RenderPipelineCommonData* common) override
    {
        GDX12RenderPass::Setup(resources, other, common);
        _commonData->Upscaler = this;
        BuildContext();
        QueryRenderTargetResolution();
    }

    // Inputs: reverse-Z depth, unjittered NDC motion, linear HDR color on the render grid.
    void Initialize() override
    {
        auto* color = _inputs[2]->GetTexture();
        if (color->GetFormat() != DXGI_FORMAT_R16G16B16A16_FLOAT)
            throw std::invalid_argument("XeSS requires linear RGBA16F HDR input");
        GDX12TextureDesc desc;
        desc.Width = _commonData->WindowWidth;
        desc.Height = _commonData->WindowHeight;
        desc.Format = desc.SRVDesc.Format = desc.UAVDesc.Format = color->GetFormat();
        desc.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
        desc.SRVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.SRVDesc.Texture2D.MipLevels = 1;
        desc.CreateUAV = true;
        desc.UAVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        _output.Initialize(desc);
    }

    void Execute(GDX12CommandList* commands) override
    {
        auto* depth = _inputs[0]->GetTexture();
        auto* motion = _inputs[1]->GetTexture();
        auto* color = _inputs[2]->GetTexture();
        if (color->GetWidth() != _commonData->DownscaledWidth || color->GetHeight() != _commonData->DownscaledHeight ||
            depth->GetWidth() != color->GetWidth() || depth->GetHeight() != color->GetHeight() ||
            motion->GetWidth() != color->GetWidth() || motion->GetHeight() != color->GetHeight())
            throw std::invalid_argument("XeSS color, depth and motion must share the render grid");
        commands->BeginPixEvent("XeSS HDR Upscale", Colors::Blue);
        commands->ResourceBarrier({ depth->GetResource()->GetNonPixelShaderResourceBarrier(),
            motion->GetResource()->GetNonPixelShaderResourceBarrier(), color->GetResource()->GetNonPixelShaderResourceBarrier(),
            _output.GetResource()->GetUnorderedAccessBarrier() });
        xess_d3d12_execute_params_t params = {};
        params.pColorTexture = color->GetResource()->D3DResource.Get();
        params.pDepthTexture = depth->GetResource()->D3DResource.Get();
        params.pVelocityTexture = motion->GetResource()->D3DResource.Get();
        params.pOutputTexture = _output.GetResource()->D3DResource.Get();
        // GPUDataUpdateSystem applies a positive raster displacement in input pixels.
        params.jitterOffsetX = _commonData->ActiveCameraJitterOffsetX;
        params.jitterOffsetY = _commonData->ActiveCameraJitterOffsetY;
        params.inputWidth = color->GetWidth();
        params.inputHeight = color->GetHeight();
        params.resetHistory = _commonData->ResetUpscalerHistory || _commonData->ResetHistory ||
            _historyRevision != _commonData->HistoryRevision;
        params.exposureScale = 1.0f;
        const auto result = xessD3D12Execute(_context, commands->GetCommandList().Get(), &params);
        commands->InvalidateCachedState();
        Check(result);
        commands->ResourceBarrier({ _output.GetResource()->GetUAVBarrier() });
        _commonData->ResetUpscalerHistory = false;
        _commonData->ResetHistory = false;
        _historyRevision = _commonData->HistoryRevision;
        commands->EndPixEvent();
    }

    void QueryRenderTargetResolution() override
    {
        const xess_2d_t output = { _commonData->WindowWidth, _commonData->WindowHeight };
        xess_2d_t input = {}, minimum = {}, maximum = {};
        Check(xessGetOptimalInputResolution(_context, &output, _quality,
            &input, &minimum, &maximum));
        _commonData->DownscaledWidth = input.x;
        _commonData->DownscaledHeight = input.y;
    }

    void Resize() override
    {
        DestroyContext();
        _output.Resize(_commonData->WindowWidth, _commonData->WindowHeight);
        BuildContext();
    }

    void ClearDenendencies() override
    {
        DestroyContext();
        GDX12RenderPass::ClearDenendencies();
    }

private:
    static void Check(xess_result_t result)
    {
        if (result < XESS_RESULT_SUCCESS)
            throw std::runtime_error("XeSS failed: " + std::to_string(result));
    }

    void DestroyContext()
    {
        if (!_context) return;
        _resources->Device->GetCommandQueue()->Flush();
        xessDestroyContext(_context);
        _context = nullptr;
    }

    void BuildContext()
    {
        Check(xessD3D12CreateContext(_resources->Device->GetDevice().Get(), &_context));
        xess_d3d12_init_params_t params = {};
        params.outputResolution = { _commonData->WindowWidth, _commonData->WindowHeight };
        params.initFlags = XESS_INIT_FLAG_INVERTED_DEPTH | XESS_INIT_FLAG_USE_NDC_VELOCITY;
        params.qualitySetting = _quality;
        const auto result = xessD3D12Init(_context, &params);
        if (result < XESS_RESULT_SUCCESS)
        {
            DestroyContext();
            Check(result);
        }
        _commonData->ResetUpscalerHistory = true;
    }

    xess_quality_settings_t _quality;
    GDX12Texture _output;
    xess_context_handle_t _context = nullptr;
    uint64_t _historyRevision = UINT64_MAX;
};
