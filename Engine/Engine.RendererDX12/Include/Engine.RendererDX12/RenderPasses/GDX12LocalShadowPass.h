#pragma once
#include "Engine.RendererDX12/GDX12InstanceBatcher.h"
#include "Engine.RendererDX12/GDX12RenderGeometrySource.h"
#include "Engine.RendererDX12/RenderPasses/GDX12GeometryViewPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12LocalLights.h"
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

class GDX12LocalShadowPass final : public GDX12GeometryViewPass
{
public:
    std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override
    { return _resources ? _resources->GetGeometryBufferAccesses(GDX12GeometryStage::Shadow) : std::vector<GDX12RenderGraph::BufferAccess>{}; }

    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Local shadows");
        graph.Read(SharedResource::Scene);
        graph.ReadInputs(_inputs);
        graph.Write(_outputs[0], "Local shadows cache", Lifetime::Dynamic);
        graph.HasSideEffects();
    }

    GDX12LocalShadowPass(bool staticOnly = false) : _staticOnly(staticOnly)
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
        graphics.InstancedGeometry = true;
        graphics.Constants.push_back(2);
        graphics.NumSingleSRVSlots = 4;
        graphics.StaticSamplers = GetStaticSamplers();
        graphics.SRVRanges.push_back(GDX12RootSignatureRange(Texture2D_RangeLength));
        _graphicsRoot = std::make_unique<GDX12RootSignature>(_resources->Device, graphics);
        GDX12RootSignatureDesc compute;
        compute.Constants.push_back(3);
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
        D3D_SHADER_MACRO defines[] = { { "SHADOW_CULLING", "1" }, { "STATIC_ONLY", _staticOnly ? "1" : "0" }, { nullptr, nullptr } };
        _cullPSO = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _computeRoot.get(),
            SHADERS_FOLDER "LocalShadowPass.hlsl", "CS", defines);
        _counterReset = std::make_unique<GDX12UploadBuffer<UINT>>(_resources->Device, 1, EBufferType::Upload, false);
        _counterReset->CopyData(0, 0u);
        _signature = GDX12RenderHelpers::CreateIndexedDrawSignature(_resources->Device, _graphicsRoot.get());

        _visibility.resize(_resources->FrameConstants.size());
        if (!_staticOnly)
        {
            _staticShadows = std::make_unique<GDX12LocalShadowPass>(true);
            _staticShadows->Setup(_resources, _otherResources, _commonData);
            _staticShadows->Initialize();
        }
    }

    void InvalidateCache()
    {
        _prepared = false;
        _cached.clear();
        if (_staticShadows) _staticShadows->InvalidateCache();
    }

    UINT GetLastFrameRenderedViews() const { return _renderedViews; }
    UINT GetLastFrameStaticRenderedViews() const { return _staticShadows ? _staticShadows->_renderedViews : _renderedViews; }

    void Prepare()
    {
        if (_prepared) return;
        const bool perFaceVisibility = _resources->LocalShadowCasters.has_value();
        _compactDynamic = !_staticOnly && perFaceVisibility;
        _preparedSceneRevision = _compactDynamic ? _resources->GeometryRevision : _resources->ReflectionStaticRevision;
        _preparedFallbackDynamicRevision = !_staticOnly && !perFaceVisibility ? _resources->ReflectionDynamicRevision : 0;
        if (perFaceVisibility && _resources->LocalShadowVisibility.size() != _resources->LocalLights.size())
            throw std::logic_error("Prepare shadow visibility before rendering local shadows");
        if (_staticShadows) _staticShadows->Prepare();
        if (!perFaceVisibility && (_staticRevision != _resources->ReflectionStaticRevision ||
            (!_staticOnly && _dynamicRevision != _resources->ReflectionDynamicRevision)))
        {
            _cached.clear();
            _staticRevision = _resources->ReflectionStaticRevision;
            _dynamicRevision = _resources->ReflectionDynamicRevision;
        }
        UINT slices = 0;
        const UINT resolution = _resources->ShadowQuality.LocalResolution;
        if (resolution != 0)
            for (const auto& light : _resources->LocalLights)
                if (light.ShadowFirstSlice != UINT_MAX) slices = (std::max)(slices, light.ShadowFirstSlice + LocalShadowFaceCount(light));
        if ((std::max)(slices, 1u) != _sliceCount || (std::max)(resolution, 1u) != _maps->GetWidth())
            CreateMaps((std::max)(slices, 1u));
        if (slices == 0)
        {
            _cached.clear();
            _preparedCache.clear();
            _dirtyFaces.clear();
            _views.clear();
            _viewFaces.clear();
            _prepared = true;
            return;
        }
        const UINT count = _resources->IndirectCommandsCache->GetElementCount();
        _commandCount = count;
        auto& visibility = _visibility[_resources->CurrFrameConstantsIndex];
        if (!visibility.Counter) visibility.Counter = CreateBuffer<UINT>(1);
        if (!visibility.Commands || visibility.Commands->GetElementCount() < count)
            visibility.Commands = CreateBuffer<GDX12IndirectDrawArgs>((std::max)(count, 1u));
        _cached.resize(_resources->LocalLights.size());
        _preparedCache.resize(_resources->LocalLights.size());
        _dirtyFaces.assign(_resources->LocalLights.size(), 0);
        _emptyDynamicFaces.assign(_resources->LocalLights.size(), 0);
        _views.clear();
        _viewFaces.clear();
        for (UINT lightIndex = 0; lightIndex < _resources->LocalLights.size(); ++lightIndex)
        {
            const auto& light = _resources->LocalLights[lightIndex];
            if (light.ShadowFirstSlice == UINT_MAX) { _preparedCache[lightIndex].reset(); continue; }
            const auto& cached = _cached[lightIndex];
            auto cacheKey = light;
            if (_staticOnly) cacheKey.ShadowFlags &= ~2u;
            std::array<uint64_t, 6> casters = {};
            if (perFaceVisibility)
            {
                const auto& visible = _resources->LocalShadowVisibility[lightIndex];
                casters = _staticOnly ? visible.StaticCasters : visible.AllCasters;
                if (!_staticOnly)
                    for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
                        if (visible.StaticCasters[face] == visible.AllCasters[face])
                            _emptyDynamicFaces[lightIndex] |= 1u << face;
            }
            const bool force = ShouldRenderLocalShadow(cacheKey, cached ? &cached->Light : nullptr);
            for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
                if (force || cached->Casters[face] != casters[face]) _dirtyFaces[lightIndex] |= 1u << face;
            _preparedCache[lightIndex] = CachedShadow{ light, casters };
            for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
            {
                GDX12GeometryContext view = { GDX12GeometryStage::Shadow, Matrix(light.ShadowViewProj[face]).Transpose(), Vector3(light.Position) };
                view.Filter = _staticOnly ? GDX12GeometryFilter::Static : GDX12GeometryFilter::Dynamic;
                view.Key = (3ull << 60) | (static_cast<uint64_t>(_staticOnly) << 59) | (lightIndex * 6ull + face);
                view.FrameSlot = _resources->CurrFrameConstantsIndex;
                view.DepthFormat = DXGI_FORMAT_D32_FLOAT;
                _views.push_back(view);
                _viewFaces.push_back({ lightIndex, face });
            }
        }
        _prepared = true;
    }

    std::vector<GDX12ShadowOutput> DescribePreparedShadows(const GDX12ShadowSelection& selection) const
    {
        std::vector<GDX12ShadowOutput> result;
        if (!_prepared || selection.Scope == GDX12ShadowSelection::Mode::Sun) return result;
        for (const auto& prepared : _preparedCache)
        {
            if (!prepared || (prepared->Light.ShadowFlags & 1u) == 0u ||
                !selection.IncludesLocal(GetLocalShadowKey(prepared->Light))) continue;
            auto casters = prepared->Casters;
            if (!_staticOnly && !_compactDynamic) casters.fill(_preparedFallbackDynamicRevision);
            result.push_back(BuildLocalShadowOutput(prepared->Light, casters, _preparedSceneRevision, _staticOnly,
                _maps->GetWidth()));
        }
        return result;
    }

    std::vector<GDX12ShadowOutput> DescribePreparedStaticShadows(const GDX12ShadowSelection& selection) const
    {
        return _staticShadows ? _staticShadows->DescribePreparedShadows(selection) : DescribePreparedShadows(selection);
    }

    UINT GetGeometryViewCapacity() const override
    {
        UINT faces = 0;
        if (_resources && _resources->ShadowQuality.LocalResolution != 0)
            for (const auto& light : _resources->LocalLights)
                if (light.ShadowFirstSlice != UINT_MAX) faces += LocalShadowFaceCount(light);
        return faces * (_staticOnly ? 1 : 2);
    }

    const GDX12GeometryContext* GetGeometryView(UINT slot) const override
    {
        if (!_prepared) return nullptr;
        const UINT capacity = static_cast<UINT>(_views.size());
        if (_staticShadows)
        {
            if (slot < capacity) return _staticShadows->GetGeometryView(slot);
            slot -= capacity;
        }
        if (slot >= capacity) return nullptr;
        const auto [lightIndex, face] = _viewFaces[slot];
        if ((_dirtyFaces[lightIndex] & (1u << face)) == 0) return nullptr;
        return &_views[slot];
    }

    void BeginGeometry(GDX12CommandList* commands) override
    {
        if (!_prepared) Prepare();
        _dynamicBegun = false;
        _renderedViews = 0;
        if (_staticShadows)
        {
            _staticShadows->SetSplitGeometry(_splitGeometry);
            _staticShadows->BeginGeometry(commands);
        }
        if (_mapsNeedClear)
        {
            commands->ResourceBarrier({ _maps->GetResource()->GetDepthWriteBarrier() });
            for (const auto& dsv : _dsvs)
                commands->GetCommandList()->ClearDepthStencilView(dsv->CPUHandle, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
            _mapsNeedClear = false;
        }
        else if (_staticOnly)
        {
            for (UINT slot = 0; slot < _views.size(); ++slot)
            {
                if (!GetGeometryView(slot)) continue;
                const auto [lightIndex, face] = _viewFaces[slot];
                const auto& light = _preparedCache[lightIndex]->Light;
                commands->ResourceBarrier({ _maps->GetResource()->GetDepthWriteBarrier() });
                commands->GetCommandList()->ClearDepthStencilView(_dsvs[light.ShadowFirstSlice + face]->CPUHandle,
                    D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
            }
        }
        if (_views.empty()) return;
        auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
        if (_compactDynamic) _commandCount = frame->DynamicShadowCommandCount;
        auto& binding = _meshBinding;
        binding.Stage = GDX12GeometryStage::Shadow;
        binding.DepthFormat = DXGI_FORMAT_D32_FLOAT;
        binding.IndirectSignature = _signature.Get();
        binding.IndirectStride = sizeof(GDX12IndirectDrawArgs);
        binding.Heap = _resources->SRV_UAV_Heap.get();
        binding.Root = _graphicsRoot.get();
        const auto geometryAddress = _resources->GeometryBuffer->GetVertexAddress();
        const auto geometrySlot = static_cast<UINT>(binding.Root->GetGeometryRootParamIndex());
        binding.RootSRVs = { { geometrySlot, geometryAddress }, { geometrySlot + 1,
            geometryAddress } };
        binding.Pipeline = _depthPSO;
        binding.SRVs = { { 0, frame->ShadowMaterials->GetSRV()->GPUHandle },
            { 1, frame->ShadowTransforms->GetSRV()->GPUHandle },
            { 2, frame->ShadowInstances->GetSRV()->GPUHandle },
            { 3, frame->ShadowLights->GetSRV()->GPUHandle }, { 4, _resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex) } };
        binding.Constants = { { 0, 0, 1 } };
        _faceBinding = binding;
        _faceBinding.CBVs.clear();
        _faceBinding.RootSRVs.clear();
        _faceBinding.SRVs.clear();
        _meshBindingsRecorded = false;
        _computeBindingsRecorded = false;
    }

    void BindGeometryViewTargets(GDX12CommandList* commands, UINT slot) override
    {
        if (!GetGeometryView(slot)) return;
        if (_staticShadows && slot < _views.size())
        {
            _staticShadows->BindGeometryViewTargets(commands, slot);
            return;
        }
        if (_staticShadows) slot -= static_cast<UINT>(_views.size());
        const auto [lightIndex, face] = _viewFaces[slot];
        const auto& light = _preparedCache[lightIndex]->Light;
        commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
        commands->ResourceBarrier({ _maps->GetResource()->GetDepthWriteBarrier() });
        commands->SetViewport(_maps->GetViewport());
        commands->SetScissorRect(_maps->GetScissorRect());
        const auto dsv = _dsvs[light.ShadowFirstSlice + face]->CPUHandle;
        commands->GetCommandList()->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    }

    void ExecuteGeometryView(GDX12CommandList* commands, UINT slot) override
    {
        const auto* prepared = GetGeometryView(slot);
        if (!prepared) return;
        if (_staticShadows && slot < _views.size())
        {
            _staticShadows->ExecuteGeometryView(commands, slot);
            return;
        }
        BeginDynamicGeometry(commands);
        BindGeometryViewTargets(commands, slot);
        if (_staticShadows) slot -= static_cast<UINT>(_views.size());
        const auto [lightIndex, face] = _viewFaces[slot];
        auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
        // The static depth copy still runs; this face has no dynamic caster to cull or draw.
        if (_emptyDynamicFaces[lightIndex] & (1u << face)) { ++_renderedViews; return; }
        auto& visibility = _visibility[_resources->CurrFrameConstantsIndex];
        const UINT count = _commandCount;
        const bool canReuse = _splitGeometry || _resources->GeometrySources.empty();
        const bool reuseCompute = canReuse && _computeBindingsRecorded && commands->UsesComputeRootSignature(_computeRoot.get());
        commands->SetComputeRootSignature(_computeRoot.get());
        commands->ResourceBarrier({ visibility.Counter->GetResource().GetCopyDestBarrier(),
            visibility.Commands->GetResource().GetUnorderedAccessBarrier() });
        if (!reuseCompute) commands->SetComputeUAV(1, visibility.Counter->GetUAV()->GPUHandle);
        {
            GDX12_SCOPED_EVENT(commands, "Local shadow clear");
            commands->GetCommandList()->CopyBufferRegion(visibility.Counter->GetResource().D3DResource.Get(), 0,
                _counterReset->GetResource().D3DResource.Get(), 0, sizeof(UINT));
            commands->ResourceBarrier({ visibility.Counter->GetResource().GetUnorderedAccessBarrier() });
        }
        {
            GDX12_SCOPED_EVENT(commands, "Local shadow cull");
            commands->SetPipelineState(_cullPSO);
            const UINT parameters[] = { lightIndex, face, count };
            commands->GetCommandList()->SetComputeRoot32BitConstants(0, 3, parameters, 0);
            if (!reuseCompute)
            {
                commands->SetComputeSRV(0, frame->ShadowInstances->GetSRV()->GPUHandle);
                commands->SetComputeSRV(1, (_compactDynamic ? frame->DynamicShadowTemplates : frame->ShadowTemplates)->GetSRV()->GPUHandle);
                commands->SetComputeSRV(2, frame->ShadowMaterials->GetSRV()->GPUHandle);
                commands->SetComputeSRV(3, frame->ShadowLights->GetSRV()->GPUHandle);
                commands->SetComputeUAV(0, visibility.Commands->GetUAV()->GPUHandle);
            }
            if (count) commands->Dispatch((count + 63) / 64, 1, 1);
        }
        _computeBindingsRecorded = true;
        if (_resources->SkeletalBatchCount)
        {
            if (!visibility.Batches) visibility.Batches = std::make_unique<GDX12InstanceBatcher>(_resources->Device);
            visibility.Batches->Build(commands, *frame->ShadowInstances, *visibility.Commands, *visibility.Counter,
                _resources->SkeletalBatchCount, _resources->SkeletalInstanceCount);
        }
        else visibility.Batches.reset();
        {
            GDX12_SCOPED_EVENT(commands, "Local shadow draw");
            commands->ResourceBarrier({ visibility.Commands->GetResource().GetIndirectArgsBarrier(),
                visibility.Counter->GetResource().GetIndirectArgsBarrier() });
            _meshBinding.RootSRVs[1].second = visibility.Batches ? visibility.Batches->VisibleInstancesAddress()
                : _resources->GeometryBuffer->GetVertexAddress();
            _meshBinding.Constants[0].Value = lightIndex * 6 + face;
            _faceBinding.Constants[0].Value = lightIndex * 6 + face;
            auto view = *prepared;
            view.IncludeApplicationGeometry = !_splitGeometry;
            GDX12RenderBatch mesh;
            // Compute culling preserves graphics bindings; application draws and native state invalidation do not.
            mesh.Binding = canReuse && _meshBindingsRecorded && commands->UsesGraphicsRootSignature(_graphicsRoot.get())
                ? &_faceBinding : &_meshBinding;
            mesh.Geometry = _resources->GeometryBuffer.get();
            mesh.InstanceCount = count;
            mesh.Draw = { GDX12DrawKind::Indirect, {}, {}, _signature.Get(),
                count ? (visibility.Batches ? visibility.Batches->Commands() : *visibility.Commands).GetResource().D3DResource.Get() : nullptr, 0,
                count ? visibility.Counter->GetResource().D3DResource.Get() : nullptr, 0,
                count, sizeof(GDX12IndirectDrawArgs) };
            _resources->EmitGeometry(commands, view, mesh);
            _meshBindingsRecorded = true;
        }
        ++_renderedViews;
    }

    void EndGeometry(GDX12CommandList* commands) override
    {
        BeginDynamicGeometry(commands);
        commands->ResourceBarrier({ _maps->GetResource()->GetSRVBarrier() });
        _cached.swap(_preparedCache);
        _prepared = false;
    }

    void ClearDenendencies() override
    {
        GDX12RenderPass::ClearDenendencies();
        _visibility.clear();
        _cached.clear();
        _preparedCache.clear();
        _dirtyFaces.clear();
        _emptyDynamicFaces.clear();
        _views.clear();
        _viewFaces.clear();
        _prepared = false;
        _dsvs.clear();
        _maps.reset();
        _staticShadows.reset();
        _graphicsRoot.reset();
        _computeRoot.reset();
        _signature.Reset();
        _depthPSO.Reset();
        _cullPSO.Reset();
        _counterReset.reset();
        _meshBinding = {};
        _faceBinding = {};
    }

private:
    void BeginDynamicGeometry(GDX12CommandList* commands)
    {
        if (_dynamicBegun) return;
        _dynamicBegun = true;
        // Static application batches must finish before the dynamic slices copy their depth.
        if (_staticShadows) _staticShadows->EndGeometry(commands);
        const bool hasDirtyFaces = std::any_of(_dirtyFaces.begin(), _dirtyFaces.end(), [](UINT faces) { return faces != 0; });
        if (_staticShadows && hasDirtyFaces)
        {
            GDX12_SCOPED_EVENT(commands, "Local shadow copy");
            auto* source = _staticShadows->_maps->GetResource();
            auto* target = _maps->GetResource();
            commands->ResourceBarrier({ source->GetCopySourceBarrier(), target->GetCopyDestBarrier() });
            for (UINT lightIndex = 0; lightIndex < _preparedCache.size(); ++lightIndex)
            {
                if (!_preparedCache[lightIndex]) continue;
                const auto& light = _preparedCache[lightIndex]->Light;
                for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
                {
                    if ((_dirtyFaces[lightIndex] & (1u << face)) == 0) continue;
                    const UINT slice = light.ShadowFirstSlice + face;
                    const CD3DX12_TEXTURE_COPY_LOCATION src(source->D3DResource.Get(), slice);
                    const CD3DX12_TEXTURE_COPY_LOCATION dst(target->D3DResource.Get(), slice);
                    commands->GetCommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                }
            }
        }
        if (hasDirtyFaces) commands->ResourceBarrier({ _maps->GetResource()->GetDepthWriteBarrier() });
    }

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
        _preparedCache.clear();
        _dirtyFaces.clear();
        _views.clear();
        _viewFaces.clear();
        _prepared = false;
        _dsvs.clear();
        const UINT resolution = (std::max)(1u, _resources->ShadowQuality.LocalResolution);
        const auto resource = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, resolution,
            resolution, static_cast<UINT16>(slices), 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1;
        GDX12TextureDesc desc;
        desc.ExternalResource = _resources->Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, resource,
            D3D12_RESOURCE_STATE_COMMON, &clear);
        desc.Format = desc.SRVDesc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.Width = desc.Height = resolution;
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
        _mapsNeedClear = true;
        _maps->GetResource()->D3DResource->SetName(L"Local shadow maps");
    }
    struct Visibility
    {
        std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>> Commands;
        std::unique_ptr<GDX12UploadBuffer<UINT>> Counter;
        std::unique_ptr<GDX12InstanceBatcher> Batches;
    };
    std::vector<Visibility> _visibility;
    struct CachedShadow
    {
        GDX12LocalLightConstants Light;
        std::array<uint64_t, 6> Casters;
    };
    std::vector<std::optional<CachedShadow>> _cached, _preparedCache;
    std::vector<UINT> _dirtyFaces, _emptyDynamicFaces;
    std::vector<GDX12GeometryContext> _views;
    std::vector<std::pair<UINT, UINT>> _viewFaces;
    bool _dynamicBegun = false;
    bool _prepared = false;
    std::vector<std::unique_ptr<GDX12Descriptor>> _dsvs;
    std::unique_ptr<GDX12Texture> _maps;
    std::unique_ptr<GDX12LocalShadowPass> _staticShadows;
    std::unique_ptr<GDX12RootSignature> _graphicsRoot, _computeRoot;
    ComPtr<ID3D12PipelineState> _depthPSO, _cullPSO;
    std::unique_ptr<GDX12UploadBuffer<UINT>> _counterReset;
    ComPtr<ID3D12CommandSignature> _signature;
    uint64_t _staticRevision = 0;
    uint64_t _dynamicRevision = 0;
    uint64_t _preparedSceneRevision = 0;
    uint64_t _preparedFallbackDynamicRevision = 0;
    bool _staticOnly = false;
    bool _compactDynamic = false;
    UINT _sliceCount = 0;
    bool _mapsNeedClear = true;
    UINT _renderedViews = 0;
    UINT _commandCount = 0;
    GDX12GraphicsBindings _meshBinding, _faceBinding;
    bool _meshBindingsRecorded = false;
    bool _computeBindingsRecorded = false;
};
