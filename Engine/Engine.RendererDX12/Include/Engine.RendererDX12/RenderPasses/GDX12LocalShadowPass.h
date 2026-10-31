#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12LocalLights.h"
#include <optional>
#include <stdexcept>

class GDX12LocalShadowPass final : public GDX12RenderPass
{
public:
    GDX12LocalShadowPass()
    {
        _flags = RENDER_PASS_FLAG_USE_GEOMETRY | RENDER_PASS_FLAG_USE_MATERIALS | RENDER_PASS_FLAG_USE_INSTANCES;
        _numOutputs = 1;
        _maps = std::make_unique<GDX12Texture>();
        _outputs.push_back(_maps.get());
    }

    void Initialize() override
    {
        CreateMaps(1);
        GDX12RootSignatureDesc graphics;
        graphics.Constants.push_back(2);
        graphics.NumSingleSRVSlots = 4;
        graphics.StaticSamplers = GetStaticSamplers();
        graphics.SRVRanges.push_back(GDX12RootSignatureRange(Texture2D_RangeLength));
        _graphicsRoot = std::make_unique<GDX12RootSignature>(_resources->Device, graphics);
        GDX12RootSignatureDesc compute;
        compute.Constants.push_back(2);
        compute.NumSingleSRVSlots = 4;
        compute.NumSingleUAVSlots = 2;
        _computeRoot = std::make_unique<GDX12RootSignature>(_resources->Device, compute);
        auto& compiler = GDX12ShaderCompiler::GetInstance();
        auto vs = compiler.CompileShader(_resources->Device, SHADERS_FOLDER "LocalShadowPass.hlsl", nullptr, "VS", "vs");
        auto ps = compiler.CompileShader(_resources->Device, SHADERS_FOLDER "LocalShadowPass.hlsl", nullptr, "PS", "ps");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline = {};
        pipeline.pRootSignature = _graphicsRoot->GetRootSignature().Get();
        pipeline.InputLayout = { _resources->InputLayouts["Default"].data(), static_cast<UINT>(_resources->InputLayouts["Default"].size()) };
        pipeline.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pipeline.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pipeline.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pipeline.RasterizerState.FrontCounterClockwise = TRUE;
        pipeline.RasterizerState.DepthBias = 250;
        pipeline.RasterizerState.SlopeScaledDepthBias = 1.5f;
        pipeline.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pipeline.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pipeline.SampleMask = UINT_MAX;
        pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pipeline.SampleDesc.Count = 1;
        ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&_depthPSO)));
        D3D_SHADER_MACRO defines[] = { { "SHADOW_CULLING", "1" }, { nullptr, nullptr } };
        const char* entries[] = { "CS", "ClearCS" };
        ComPtr<ID3D12PipelineState>* targets[] = { &_cullPSO, &_clearPSO };
        for (UINT i = 0; i < 2; ++i)
        {
            auto cs = compiler.CompileShader(_resources->Device, SHADERS_FOLDER "LocalShadowPass.hlsl", defines, entries[i], "cs");
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
            desc.pRootSignature = _computeRoot->GetRootSignature().Get();
            desc.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
            ThrowIfFailed(_resources->Device->GetDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(targets[i]->GetAddressOf())));
        }
        D3D12_INDIRECT_ARGUMENT_DESC arguments[2] = {};
        arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        arguments[0].Constant.Num32BitValuesToSet = 1;
        arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC signature = {};
        signature.ByteStride = sizeof(GDX12IndirectDrawArgs);
        signature.NumArgumentDescs = 2;
        signature.pArgumentDescs = arguments;
        ThrowIfFailed(_resources->Device->GetDevice()->CreateCommandSignature(&signature,
            _graphicsRoot->GetRootSignature().Get(), IID_PPV_ARGS(&_signature)));
        _visibility.resize(_resources->FrameConstants.size());
    }

    UINT GetLastFrameRenderedViews() const { return _renderedViews; }

    void Execute(GDX12CommandList* commands) override
    {
        UINT slices = 0;
        for (const auto& light : _resources->LocalLights)
            if (light.ShadowFirstSlice != UINT_MAX) slices = (std::max)(slices, light.ShadowFirstSlice + LocalShadowFaceCount(light));
        if ((std::max)(slices, 1u) != _sliceCount) CreateMaps((std::max)(slices, 1u));
        _renderedViews = 0;
        _cached.resize(_resources->LocalLights.size());
        auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
        auto& visibility = _visibility[_resources->CurrFrameConstantsIndex];
        const UINT count = _resources->IndirectCommandsCache->GetElementCount();
        if (!visibility.Counter) visibility.Counter = CreateBuffer<UINT>(1);
        if (!visibility.Commands || visibility.Commands->GetElementCount() < count)
            visibility.Commands = CreateBuffer<GDX12IndirectDrawArgs>((std::max)(count, 1u));
        commands->BeginPixEvent("Point and spot shadow maps", Colors::DarkGoldenrod);
        commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
        for (UINT lightIndex = 0; lightIndex < _resources->LocalLights.size(); ++lightIndex)
        {
            const auto& light = _resources->LocalLights[lightIndex];
            if (light.ShadowFirstSlice == UINT_MAX)
            {
                _cached[lightIndex].reset();
                continue;
            }
            const auto& cached = _cached[lightIndex];
            if (!ShouldRenderLocalShadow(light, cached ? &*cached : nullptr)) continue;
            for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
            {
                commands->SetComputeRootSignature(_computeRoot.get());
                commands->ResourceBarrier({ visibility.Counter->GetResource().GetUnorderedAccessBarrier(),
                    visibility.Commands->GetResource().GetUnorderedAccessBarrier() });
                commands->SetComputeUAV(1, visibility.Counter->GetUAV()->GPUHandle);
                commands->SetPipelineState(_clearPSO);
                commands->Dispatch(1, 1, 1);
                commands->ResourceBarrier({ visibility.Counter->GetResource().GetUAVBarrier() });
                commands->SetPipelineState(_cullPSO);
                const UINT parameters[] = { lightIndex, face };
                commands->GetCommandList()->SetComputeRoot32BitConstants(0, 2, parameters, 0);
                commands->SetComputeSRV(0, frame->InstanceCache->GetSRV()->GPUHandle);
                commands->SetComputeSRV(1, _resources->IndirectCommandsCache->GetSRV()->GPUHandle);
                commands->SetComputeSRV(2, frame->MaterialCache->GetSRV()->GPUHandle);
                commands->SetComputeSRV(3, frame->LocalLightCache->GetSRV()->GPUHandle);
                commands->SetComputeUAV(0, visibility.Commands->GetUAV()->GPUHandle);
                if (count) commands->Dispatch((count + 63) / 64, 1, 1);
                commands->ResourceBarrier({ visibility.Commands->GetResource().GetIndirectArgsBarrier(),
                    visibility.Counter->GetResource().GetIndirectArgsBarrier(), _maps->GetResource()->GetDepthWriteBarrier() });
                commands->SetGraphicsRootSignature(_graphicsRoot.get());
                commands->SetPipelineState(_depthPSO);
                commands->SetGraphicsSRV(0, frame->MaterialCache->GetSRV()->GPUHandle);
                commands->SetGraphicsSRV(1, frame->TransformCache->GetSRV()->GPUHandle);
                commands->SetGraphicsSRV(2, frame->InstanceCache->GetSRV()->GPUHandle);
                commands->SetGraphicsSRV(3, frame->LocalLightCache->GetSRV()->GPUHandle);
                commands->SetGraphicsSRV(4, _resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));
                commands->GetCommandList()->SetGraphicsRoot32BitConstant(0, lightIndex * 6 + face, 1);
                commands->SetGeometryBuffer(_resources->GeometryBuffer.get());
                commands->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                commands->SetViewport({ 0, 0, static_cast<float>(LocalShadowResolution), static_cast<float>(LocalShadowResolution), 0, 1 });
                commands->SetScissorRect({ 0, 0, LocalShadowResolution, LocalShadowResolution });
                const auto dsv = _dsvs[light.ShadowFirstSlice + face]->CPUHandle;
                commands->GetCommandList()->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
                commands->GetCommandList()->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
                if (count) commands->ExecuteIndirect(_signature.Get(), count,
                    visibility.Commands->GetResource().D3DResource.Get(), 0, visibility.Counter->GetResource().D3DResource.Get(), 0);
                ++_renderedViews;
            }
            _cached[lightIndex] = light;
        }
        commands->ResourceBarrier({ _maps->GetResource()->GetSRVBarrier() });
        commands->EndPixEvent();
    }

    void ClearDenendencies() override
    {
        GDX12RenderPass::ClearDenendencies();
        _visibility.clear();
        _cached.clear();
        _dsvs.clear();
        _maps.reset();
        _graphicsRoot.reset();
        _computeRoot.reset();
        _signature.Reset();
        _depthPSO.Reset();
        _cullPSO.Reset();
        _clearPSO.Reset();
    }

private:
    template<typename T> std::unique_ptr<GDX12UploadBuffer<T>> CreateBuffer(UINT count)
    {
        auto buffer = std::make_unique<GDX12UploadBuffer<T>>(_resources->Device, count, EBufferType::Default, false);
        buffer->CreateUAV(_resources->SRV_UAV_Heap.get(), _resources->SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        return buffer;
    }

    void CreateMaps(UINT slices)
    {
        if (slices > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) throw std::runtime_error("Local shadow maps exceed the DX12 texture-array layer limit");
        if (_sliceCount) _resources->Device->GetCommandQueue()->Flush();
        _cached.clear();
        _dsvs.clear();
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto resource = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, LocalShadowResolution,
            LocalShadowResolution, static_cast<UINT16>(slices), 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1;
        GDX12TextureDesc desc;
        ThrowIfFailed(_resources->Device->GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &resource, D3D12_RESOURCE_STATE_COMMON, &clear, IID_PPV_ARGS(&desc.ExternalResource)));
        desc.Format = desc.SRVDesc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.Width = desc.Height = LocalShadowResolution;
        desc.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
        desc.SRVHeapIndex = _sliceCount ? _maps->GetSRV()->HeapIndex : desc.SRV_UAV_Heap->GetAvailableIndex(ShadowMaps_StartIndex, ShadowMaps_RangeLength);
        desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        desc.SRVDesc.Texture2DArray.ArraySize = slices;
        desc.SRVDesc.Texture2DArray.MipLevels = 1;
        _maps->Initialize(desc);
        for (UINT slice = 0; slice < slices; ++slice)
        {
            auto descriptor = std::make_unique<GDX12Descriptor>();
            D3D12_DEPTH_STENCIL_VIEW_DESC view = {};
            view.Format = DXGI_FORMAT_D32_FLOAT;
            view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.FirstArraySlice = slice;
            view.Texture2DArray.ArraySize = 1;
            descriptor->InitAsDSV(desc.ExternalResource.Get(), &view, _resources->DSVHeap.get(), _resources->DSVHeap->GetAvailableIndex());
            _dsvs.push_back(std::move(descriptor));
        }
        _sliceCount = slices;
    }
    struct Visibility
    {
        std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>> Commands;
        std::unique_ptr<GDX12UploadBuffer<UINT>> Counter;
    };
    std::vector<Visibility> _visibility;
    std::vector<std::optional<GDX12LocalLightConstants>> _cached;
    std::vector<std::unique_ptr<GDX12Descriptor>> _dsvs;
    std::unique_ptr<GDX12Texture> _maps;
    std::unique_ptr<GDX12RootSignature> _graphicsRoot, _computeRoot;
    ComPtr<ID3D12PipelineState> _depthPSO, _cullPSO, _clearPSO;
    ComPtr<ID3D12CommandSignature> _signature;
    UINT _sliceCount = 0;
    UINT _renderedViews = 0;
};
