#pragma once
#include "Engine.RendererDX12/GDX12InstanceBatcher.h"
#include "Engine.RendererDX12/GDX12RenderGeometrySource.h"
#include "Engine.RendererDX12/RenderPasses/GDX12GeometryViewPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <array>
#include <cstring>

class GDX12CascadedShadowPass final : public GDX12GeometryViewPass
{
public:
    std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override
    { return _resources ? _resources->GetGeometryBufferAccesses(GDX12GeometryStage::Shadow) : std::vector<GDX12RenderGraph::BufferAccess>{}; }

    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Sun shadows");
        graph.Read(SharedResource::Scene);
        graph.ReadInputs(_inputs);
        graph.Write(_outputs[0], "Sun shadows cache", Lifetime::Dynamic);
        graph.HasSideEffects();
    }

    GDX12CascadedShadowPass()
    {
        _flags = RENDER_PASS_FLAG_USE_GEOMETRY | RENDER_PASS_FLAG_USE_MATERIALS | RENDER_PASS_FLAG_USE_INSTANCES;
        _numOutputs = 1;
        _atlas = std::make_unique<GDX12Texture>();
        _outputs.push_back(_atlas.get());
    }

    void Initialize() override
    {
        GDX12TextureDesc texture;
        texture.Width = texture.Height = 4096;
        texture.Format = texture.DSVDesc.Format = DXGI_FORMAT_D32_FLOAT;
        texture.SRVDesc.Format = DXGI_FORMAT_R32_FLOAT;
        texture.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        texture.SRVDesc.Texture2D.MipLevels = 1;
        texture.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        texture.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
        texture.SRVHeapIndex = texture.SRV_UAV_Heap->GetAvailableIndex(ShadowMaps_StartIndex, ShadowMaps_RangeLength);
        texture.CreateDSV = true;
        texture.DSVHeap = _resources->DSVHeap.get();
        texture.DSVHeapIndex = texture.DSVHeap->GetAvailableIndex();
        texture.DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        texture.ClearValue = { 1, 0, 0, 0 };
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, 4096, 4096,
            1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1.0f;
        texture.ExternalResource = _resources->Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc,
            D3D12_RESOURCE_STATE_COMMON, &clear);
        _atlas->Initialize(texture);
        _atlasNeedsClear = true;

        GDX12RootSignatureDesc graphics;
        graphics.InstancedGeometry = true;
        graphics.Constants.push_back(2);
        graphics.NumSingleCBVSlots = 1;
        graphics.NumSingleSRVSlots = 3;
        graphics.StaticSamplers = GetStaticSamplers();
        graphics.SRVRanges.push_back(GDX12RootSignatureRange(Texture2D_RangeLength));
        _graphicsRoot = std::make_unique<GDX12RootSignature>(_resources->Device, graphics);
        GDX12RootSignatureDesc compute;
        compute.NumSingleCBVSlots = 1;
        compute.NumSingleSRVSlots = 3;
        compute.NumSingleUAVSlots = 5;
        _computeRoot = std::make_unique<GDX12RootSignature>(_resources->Device, compute);

        auto& compiler = GDX12ShaderCompiler::GetInstance();
        auto vs = compiler.CompileShader(_resources->Device, SHADERS_FOLDER "ShadowPass.hlsl", nullptr, "VS", "vs");
        auto ps = compiler.CompileShader(_resources->Device, SHADERS_FOLDER "ShadowPass.hlsl", nullptr, "PS", "ps");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline = {};
        pipeline.pRootSignature = _graphicsRoot->GetRootSignature().Get();
        pipeline.InputLayout = { _resources->InputLayouts["Default"].data(), static_cast<UINT>(_resources->InputLayouts["Default"].size()) };
        pipeline.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pipeline.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pipeline.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pipeline.RasterizerState.FrontCounterClockwise = TRUE;
        pipeline.RasterizerState.DepthBias = 500;
        pipeline.RasterizerState.SlopeScaledDepthBias = 1.5f;
        pipeline.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pipeline.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pipeline.SampleMask = UINT_MAX;
        pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pipeline.SampleDesc.Count = 1;
        ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&_shadowPSO)));

        D3D_SHADER_MACRO defines[] = { { "SHADOW_CULLING", "1" }, { nullptr, nullptr } };
        const char* entries[] = { "CS", "ClearCS" };
        ComPtr<ID3D12PipelineState>* targets[] = { &_cullingPSO, &_clearPSO };
        for (UINT i = 0; i < 2; ++i)
        {
            *targets[i] = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _computeRoot.get(),
                SHADERS_FOLDER "ShadowPass.hlsl", entries[i], defines);
        }
        _signature = GDX12RenderHelpers::CreateIndexedDrawSignature(_resources->Device, _graphicsRoot.get());

        _visibility.resize(_resources->FrameConstants.size());
    }

    void Prepare()
    {
        if (_prepared) return;
        const auto& lighting = _resources->Lighting;
        _updating = (lighting.SunShadowFlags & 1u) != 0u &&
            ((lighting.SunShadowFlags & 2u) != 0u || !_rendered ||
                _cachedGeometryRevision != _resources->GeometryRevision || _cachedRevision != lighting.SunShadowRevision ||
                std::memcmp(&_cachedSettings, &_resources->ShadowSettings, sizeof(_cachedSettings)) != 0);
        if (_updating)
        {
            _preparedSettings = _resources->ShadowSettings;
            _preparedGeometryRevision = _resources->GeometryRevision;
            _preparedRevision = lighting.SunShadowRevision;
            for (UINT cascade = 0; cascade < 4; ++cascade)
            {
                auto& view = _views[cascade];
                view = { GDX12GeometryStage::Shadow, Matrix(_preparedSettings.ViewProj[cascade]).Transpose(),
                    _commonData->CameraView.Invert().Translation() };
                view.Key = (2ull << 60) | cascade;
                view.FrameSlot = _resources->CurrFrameConstantsIndex;
                view.DepthFormat = DXGI_FORMAT_D32_FLOAT;
            }
        }
        _prepared = true;
    }

    UINT GetGeometryViewCapacity() const override { return 4; }
    const GDX12GeometryContext* GetGeometryView(UINT slot) const override
    { return _prepared && _updating && slot < _views.size() ? &_views[slot] : nullptr; }

    void BeginGeometry(GDX12CommandList* commands) override
    {
        if (!_prepared) Prepare();
        if (_atlasNeedsClear && !_updating)
        {
            commands->ResourceBarrier({ _atlas->GetResource()->GetDepthWriteBarrier() });
            commands->GetCommandList()->ClearDepthStencilView(_atlas->GetDSV()->CPUHandle, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
            _atlasNeedsClear = false;
        }
        if (!_updating) return;
        const UINT count = _resources->IndirectCommandsCache->GetElementCount();
        auto& visibility = _visibility[_resources->CurrFrameConstantsIndex];
        auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
        // RenderModule waits for this frame's fence before these buffers may grow.
        if (!visibility.Counters)
        {
            visibility.Counters = CreateBuffer<UINT>(4);
            for (auto& buffer : visibility.Commands) buffer = CreateBuffer<GDX12IndirectDrawArgs>((std::max)(count, 1u));
        }
        for (auto& buffer : visibility.Commands)
            if (count > buffer->GetElementCount()) buffer = CreateBuffer<GDX12IndirectDrawArgs>(count);
        GDX12_SCOPED_EVENT(commands, "Cascaded shadow maps", Colors::DarkSlateBlue);
        commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
        commands->SetComputeRootSignature(_computeRoot.get());
        commands->ResourceBarrier({ visibility.Counters->GetResource().GetUnorderedAccessBarrier() });
        for (auto& buffer : visibility.Commands)
            commands->ResourceBarrier({ buffer->GetResource().GetUnorderedAccessBarrier() });
        commands->SetComputeUAV(4, visibility.Counters->GetUAV()->GPUHandle);
        commands->SetPipelineState(_clearPSO);
        commands->Dispatch(1, 1, 1);
        commands->ResourceBarrier({ visibility.Counters->GetResource().GetUAVBarrier() });
        commands->SetPipelineState(_cullingPSO);
        commands->SetComputeRootConstantBufferView(0, frame->ShadowCB->GetElementAddress(0));
        commands->SetComputeSRV(0, frame->ShadowInstances->GetSRV()->GPUHandle);
        commands->SetComputeSRV(1, frame->ShadowTemplates->GetSRV()->GPUHandle);
        commands->SetComputeSRV(2, frame->ShadowMaterials->GetSRV()->GPUHandle);
        for (UINT cascade = 0; cascade < 4; ++cascade)
            commands->SetComputeUAV(cascade, visibility.Commands[cascade]->GetUAV()->GPUHandle);
        if (count) commands->Dispatch((count + 63) / 64, 1, 1);

        for (UINT cascade = 0; cascade < 4; ++cascade)
        {
            auto& batches = visibility.Batches[cascade];
            if (_resources->SkeletalBatchCount)
            {
                if (!batches) batches = std::make_unique<GDX12InstanceBatcher>(_resources->Device);
                batches->Build(commands, *frame->ShadowInstances, *visibility.Commands[cascade], *visibility.Counters,
                    _resources->SkeletalBatchCount, _resources->SkeletalInstanceCount, cascade);
            }
            else batches.reset();
        }
        commands->ResourceBarrier({ _atlas->GetResource()->GetDepthWriteBarrier(),
            visibility.Counters->GetResource().GetIndirectArgsBarrier() });
        commands->SetRenderTargets({}, _atlas.get());
        commands->GetCommandList()->ClearDepthStencilView(_atlas->GetDSV()->CPUHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        _atlasNeedsClear = false;
    }

    void BindGeometryViewTargets(GDX12CommandList* commands, UINT cascade) override
    {
        if (!GetGeometryView(cascade)) return;
        commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
        commands->ResourceBarrier({ _atlas->GetResource()->GetDepthWriteBarrier() });
        commands->SetRenderTargets({}, _atlas.get());
        const UINT x = (cascade & 1u) * 2048;
        const UINT y = (cascade >> 1u) * 2048;
        commands->SetViewport({ static_cast<float>(x), static_cast<float>(y), 2048.0f, 2048.0f, 0.0f, 1.0f });
        commands->SetScissorRect({ static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + 2048), static_cast<LONG>(y + 2048) });
    }

    void ExecuteGeometryView(GDX12CommandList* commands, UINT cascade) override
    {
        const auto* prepared = GetGeometryView(cascade);
        if (!prepared) return;
        BindGeometryViewTargets(commands, cascade);
        auto& visibility = _visibility[_resources->CurrFrameConstantsIndex];
        auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
        const UINT count = _resources->IndirectCommandsCache->GetElementCount();
        GDX12GraphicsBindings binding;
        binding.Stage = GDX12GeometryStage::Shadow;
        binding.DepthFormat = DXGI_FORMAT_D32_FLOAT;
        binding.IndirectSignature = _signature.Get();
        binding.IndirectStride = sizeof(GDX12IndirectDrawArgs);
        binding.Heap = _resources->SRV_UAV_Heap.get();
        binding.Root = _graphicsRoot.get();
        const auto geometryAddress = _resources->GeometryBuffer->GetVertexAddress();
        const auto geometrySlot = static_cast<UINT>(binding.Root->GetGeometryRootParamIndex());
        binding.RootSRVs = { { geometrySlot, geometryAddress }, { geometrySlot + 1,
            visibility.Batches[cascade] ? visibility.Batches[cascade]->VisibleInstancesAddress() : geometryAddress } };
        binding.Pipeline = _shadowPSO;
        binding.CBVs.push_back({ 1, frame->ShadowCB->GetElementAddress(0) });
        binding.SRVs.push_back({ 0, frame->ShadowMaterials->GetSRV()->GPUHandle });
        binding.SRVs.push_back({ 1, frame->ShadowTransforms->GetSRV()->GPUHandle });
        binding.SRVs.push_back({ 2, frame->ShadowInstances->GetSRV()->GPUHandle });
        binding.SRVs.push_back({ 3, _resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex) });
        binding.Constants = { { 0, cascade, 1 } };
        commands->ResourceBarrier({ visibility.Commands[cascade]->GetResource().GetIndirectArgsBarrier() });
        auto view = *prepared;
        view.IncludeApplicationGeometry = !_splitGeometry;
        GDX12RenderBatch mesh;
        mesh.Binding = &binding;
        mesh.Geometry = _resources->GeometryBuffer.get();
        mesh.InstanceCount = count;
        mesh.Draw = { GDX12DrawKind::Indirect, {}, {}, _signature.Get(),
            count ? (visibility.Batches[cascade] ? visibility.Batches[cascade]->Commands() : *visibility.Commands[cascade]).GetResource().D3DResource.Get() : nullptr, 0,
            count ? visibility.Counters->GetResource().D3DResource.Get() : nullptr, cascade * sizeof(UINT),
            count, sizeof(GDX12IndirectDrawArgs) };
        _resources->EmitGeometry(commands, view, mesh);
    }

    void EndGeometry(GDX12CommandList* commands) override
    {
        commands->ResourceBarrier({ _atlas->GetResource()->GetSRVBarrier() });
        if (_updating)
        {
            _cachedGeometryRevision = _preparedGeometryRevision;
            _rendered = true;
            _cachedSettings = _preparedSettings;
            _cachedRevision = _preparedRevision;
        }
        _prepared = false;
        _updating = false;
    }

    void ClearDenendencies() override
    {
        GDX12RenderPass::ClearDenendencies();
        _prepared = _updating = _rendered = false;
        _visibility.clear();
        _atlas.reset();
        _graphicsRoot.reset();
        _computeRoot.reset();
        _signature.Reset();
        _shadowPSO.Reset();
        _cullingPSO.Reset();
        _clearPSO.Reset();
    }

private:
    std::array<GDX12GeometryContext, 4> _views;
    bool _prepared = false, _updating = false;
    GDX12ShadowConstants _preparedSettings;
    uint64_t _preparedGeometryRevision = 0;
    UINT _preparedRevision = 0;
    bool _atlasNeedsClear = true;
    template<typename T>
    std::unique_ptr<GDX12UploadBuffer<T>> CreateBuffer(UINT count)
    {
        auto buffer = std::make_unique<GDX12UploadBuffer<T>>(_resources->Device, count, EBufferType::Default, false);
        buffer->CreateUAV(_resources->SRV_UAV_Heap.get(), _resources->SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        return buffer;
    }
    struct Visibility
    {
        std::array<std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>>, 4> Commands;
        std::unique_ptr<GDX12UploadBuffer<UINT>> Counters;
        std::array<std::unique_ptr<GDX12InstanceBatcher>, 4> Batches;
    };
    std::vector<Visibility> _visibility;
    std::unique_ptr<GDX12Texture> _atlas;
    std::unique_ptr<GDX12RootSignature> _graphicsRoot, _computeRoot;
    ComPtr<ID3D12PipelineState> _shadowPSO, _cullingPSO, _clearPSO;
    ComPtr<ID3D12CommandSignature> _signature;
    GDX12ShadowConstants _cachedSettings;
    uint64_t _cachedGeometryRevision = 0;
    UINT _cachedRevision = 0;
    bool _rendered = false;
};
