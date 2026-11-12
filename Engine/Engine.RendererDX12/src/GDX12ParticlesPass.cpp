#include "Engine.RendererDX12/RenderPasses/GDX12ParticlesPass.h"
#include "GDX12ParticleOffload.h"
#include <algorithm>
#include <stdexcept>

using namespace ParticleResources;

struct GDX12ParticlesPass::State
{
    UINT Capacity = 0;
    bool Sorted = false;
    bool Mesh = false;
    bool Active = false;
    bool WasEnabled = false;
    bool ResetMotion = true;
    UINT ConstantCount = 0;
    std::vector<std::unique_ptr<Buffer<EmitterConstants>>> Constants;
    std::unique_ptr<GDX12RootSignature> Root, SortRoot;
    std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> Compute;
    ComPtr<ID3D12PipelineState> Weighted, Additive, Alpha, Motion;
    ComPtr<ID3D12PipelineState> MeshWeighted, MeshAdditive, MeshMotion;
    std::unique_ptr<Buffer<GDX12ParticleMeshVertex>> FireflyVertices;
    ComPtr<ID3D12CommandSignature> DispatchSignature, DrawSignature;
    std::unique_ptr<Buffer<ParticleDraw>> Draws;
    std::unique_ptr<Buffer<UINT>> Counts, Indices, DrawArgs, SortParameters;
    std::unique_ptr<Buffer<UINT>> Keys[2], Payloads[2], Sum, Reduced;
    std::unique_ptr<GDX12Texture> MotionTexture;

    void Initialize(GDX12DeviceResources* resources)
    {
        Root = CreateRoot(resources);
        for (const auto* entry : { "Cull", "CullImported", "ClearDraws", "BuildDrawArgs" })
            CompileCompute(resources, "ParticleRender.hlsl", entry, Root.get());

        D3D12_INDIRECT_ARGUMENT_DESC argument = {};
        argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC signature = {};
        signature.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
        signature.NumArgumentDescs = 1;
        signature.pArgumentDescs = &argument;
        ThrowIfFailed(resources->Device->GetDevice()->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&DispatchSignature)));
        argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        signature.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
        ThrowIfFailed(resources->Device->GetDevice()->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&DrawSignature)));

        InitializeRaster(resources, false);
    }

    void InitializeRaster(GDX12DeviceResources* resources, bool mesh)
    {
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        auto& compiler = GDX12ShaderCompiler::GetInstance();
        auto vs = compiler.CompileShader(resources->Device, SHADERS_FOLDER "ParticleRaster.hlsl", nullptr, mesh ? "MeshVS" : "VS", "vs");
        for (UINT mode = 0; mode < 4; ++mode)
        {
            if (mesh && mode == 2) continue;
            auto ps = compiler.CompileShader(resources->Device, SHADERS_FOLDER "ParticleRaster.hlsl", nullptr,
                mode == 0 ? "WeightedPS" : mode == 3 ? "MotionPS" : "ColorPS", "ps");
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
            desc.pRootSignature = Root->GetRootSignature().Get();
            desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
            desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
            if (mesh) desc.InputLayout = {layout, static_cast<UINT>(std::size(layout))};
            desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            desc.RasterizerState.CullMode = mesh ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
            desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
            desc.DepthStencilState.DepthEnable = false;
            desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            desc.DepthStencilState.StencilEnable = false;
            desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            desc.BlendState.IndependentBlendEnable = true;
            auto& blend = desc.BlendState.RenderTarget[0];
            blend.BlendEnable = true;
            blend.SrcBlend = D3D12_BLEND_ONE;
            blend.DestBlend = mode == 2 ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ONE;
            blend.SrcBlendAlpha = mode == 0 || mode == 3 ? D3D12_BLEND_ONE : D3D12_BLEND_ZERO;
            blend.DestBlendAlpha = D3D12_BLEND_ONE;
            desc.SampleMask = UINT_MAX;
            desc.SampleDesc.Count = 1;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = mode == 0 ? 2 : 1;
            desc.RTVFormats[0] = mode == 3 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
            if (mode == 0)
            {
                desc.RTVFormats[1] = DXGI_FORMAT_R16_FLOAT;
                auto& reveal = desc.BlendState.RenderTarget[1];
                reveal.BlendEnable = true;
                reveal.SrcBlend = D3D12_BLEND_ZERO;
                reveal.DestBlend = D3D12_BLEND_INV_SRC_COLOR;
                reveal.SrcBlendAlpha = D3D12_BLEND_ZERO;
                reveal.DestBlendAlpha = D3D12_BLEND_ONE;
            }
            auto& target = mesh ? (mode == 0 ? MeshWeighted : mode == 1 ? MeshAdditive : MeshMotion) :
                (mode == 0 ? Weighted : mode == 1 ? Additive : mode == 2 ? Alpha : Motion);
            ThrowIfFailed(resources->Device->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(target.ReleaseAndGetAddressOf())));
        }
        if (mesh)
        {
            FireflyVertices = std::make_unique<Buffer<GDX12ParticleMeshVertex>>(resources->Device, FireflyParticleVertexCount, EBufferType::Default, false);
            Buffer<GDX12ParticleMeshVertex> upload(resources->Device, FireflyParticleVertexCount, EBufferType::Upload, false);
            upload.CopyData(0, GetFireflyParticleMesh().data(), FireflyParticleVertexCount);
            auto* queue = resources->Device->GetCommandQueue();
            auto* commands = queue->GetCommandList();
            commands->ResourceBarrier({FireflyVertices->GetResource().GetCopyDestBarrier()});
            commands->GetCommandList()->CopyBufferRegion(FireflyVertices->GetResource().D3DResource.Get(), 0,
                upload.GetResource().D3DResource.Get(), 0, UINT64(FireflyParticleVertexCount) * sizeof(GDX12ParticleMeshVertex));
            commands->ResourceBarrier({FireflyVertices->GetResource().GetGenericReadBarrier()});
            queue->ExecuteCommandList(commands);
            // Lazy mesh initialization completes its upload before any color or motion draw.
            queue->Flush();
        }
    }

    void CompileCompute(GDX12DeviceResources* resources, const char* file, const char* entry, GDX12RootSignature* root)
    {
        const std::wstring path = std::wstring(SHADERS_FOLDER) + StringToWString(file);
        Compute[entry] = GDX12RenderHelpers::CreateComputePipeline(resources->Device, root, path.c_str(), entry);
    }

    void InitializeSort(GDX12DeviceResources* resources)
    {
        if (SortRoot) return;
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options = {};
        ThrowIfFailed(resources->Device->GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options)));
        if (!options.WaveOps || options.WaveLaneCountMin < 16 ||
            resources->Device->GetDeviceFeatures().Features.HighestShaderModel() < D3D_SHADER_MODEL_6_0)
            throw std::runtime_error("FidelityFX particle sorting requires SM6 wave operations");
        GDX12RootSignatureDesc root;
        root.VolatileSRVData = true;
        root.Constants = { 2 };
        root.NumSingleCBVSlots = 1;
        root.NumSingleUAVSlots = 6;
        SortRoot = std::make_unique<GDX12RootSignature>(resources->Device, root);
        for (const auto* entry : { "Count", "Reduce", "Scan", "ScanAdd", "Scatter" })
            CompileCompute(resources, "ParticleSort.hlsl", entry, SortRoot.get());
    }

    void AllocateDraws(GDX12DeviceResources* resources, const UINT capacity, const bool sorted, const bool mesh)
    {
        Capacity = capacity;
        Sorted = sorted;
        Mesh = mesh;
        if (mesh && !FireflyVertices) InitializeRaster(resources, true);
        Draws = CreateBuffer<ParticleDraw>(resources, capacity);
        Counts = CreateBuffer<UINT>(resources, 6);
        Indices = CreateBuffer<UINT>(resources, capacity * (mesh ? 4 : 2));
        DrawArgs = CreateBuffer<UINT>(resources, 26);
        SortParameters = CreateBuffer<UINT>(resources, 64); // GPU-written CBV, 256-byte allocation.
        for (UINT i = 0; i < 2; ++i)
        {
            Keys[i] = CreateBuffer<UINT>(resources, sorted ? capacity : 1);
            Payloads[i] = CreateBuffer<UINT>(resources, sorted ? capacity : 1);
        }
        Sum.reset();
        Reduced.reset();
        if (sorted)
        {
            InitializeSort(resources);
            Sum = CreateBuffer<UINT>(resources, 16 * 800);
            Reduced = CreateBuffer<UINT>(resources, 16 * 2);
        }
    }

    void BindGlobalUAVs(GDX12CommandList* commands)
    {
        commands->SetComputeUAV(5, Draws->GetUAV()->GPUHandle);
        commands->SetComputeUAV(6, Counts->GetUAV()->GPUHandle);
        commands->SetComputeUAV(7, Indices->GetUAV()->GPUHandle);
        commands->SetComputeUAV(8, Keys[0]->GetUAV()->GPUHandle);
        commands->SetComputeUAV(9, Payloads[0]->GetUAV()->GPUHandle);
        commands->SetComputeUAV(10, DrawArgs->GetUAV()->GPUHandle);
        commands->SetComputeUAV(11, SortParameters->GetUAV()->GPUHandle);
    }

    void Sort(GDX12CommandList* commands)
    {
        commands->SetComputeRootSignature(SortRoot.get());
        commands->ResourceBarrier({ SortParameters->GetResource().GetGenericReadBarrier() });
        commands->SetComputeRootConstantBufferView(1, SortParameters->GetElementAddress(0));
        UAV(commands, *Sum);
        UAV(commands, *Reduced);
        commands->SetComputeUAV(4, Sum->GetUAV()->GPUHandle);
        commands->SetComputeUAV(5, Reduced->GetUAV()->GPUHandle);
        for (UINT pass = 0; pass < 8; ++pass)
        {
            const UINT source = pass & 1;
            const UINT dest = source ^ 1;
            UAV(commands, *Keys[source]);
            UAV(commands, *Keys[dest]);
            UAV(commands, *Payloads[source]);
            UAV(commands, *Payloads[dest]);
            commands->SetComputeUAV(0, Keys[source]->GetUAV()->GPUHandle);
            commands->SetComputeUAV(1, Keys[dest]->GetUAV()->GPUHandle);
            commands->SetComputeUAV(2, Payloads[source]->GetUAV()->GPUHandle);
            commands->SetComputeUAV(3, Payloads[dest]->GetUAV()->GPUHandle);
            const char* entries[] = { "Count", "Reduce", "Scan", "ScanAdd", "Scatter" };
            for (UINT stage = 0; stage < 5; ++stage)
            {
                const UINT parameters[] = { pass * 4, stage };
                commands->GetCommandList()->SetComputeRoot32BitConstants(0, 2, parameters, 0);
                commands->SetPipelineState(Compute.at(entries[stage]));
                if (stage == 2) commands->Dispatch(1, 1, 1);
                else commands->ExecuteIndirect(DispatchSignature.Get(), 1, DrawArgs->GetResource().D3DResource.Get(),
                    stage == 1 || stage == 3 ? 60 : 48, nullptr);
                commands->ResourceBarrier({ Sum->GetResource().GetUAVBarrier(), Reduced->GetResource().GetUAVBarrier(),
                    Keys[dest]->GetResource().GetUAVBarrier(), Payloads[dest]->GetResource().GetUAVBarrier() });
            }
        }
    }

    void Draw(GDX12CommandList* commands, GDX12DeviceResources* resources, RenderPipelineCommonData* common, const UINT mode)
    {
        commands->SetGraphicsRootSignature(Root.get());
        commands->SetDescriptorHeaps({ resources->SRV_UAV_Heap.get() });
        auto& frame = resources->FrameConstants[resources->CurrFrameConstantsIndex];
        commands->SetGraphicsRootConstantBufferView(1, frame->CameraCB->GetElementAddress(common->ActiveCameraCBufferIndex));
        const UINT region = mode < 3 ? mode : mode - 1;
        const UINT blend = mode < 3 ? mode : mode - 3;
        const UINT parameters[] = { region * Capacity, blend, Capacity, ResetMotion };
        commands->GetCommandList()->SetGraphicsRoot32BitConstants(0, 4, parameters, 0);
        commands->SetGraphicsSRV(0, Draws->GetSRV()->GPUHandle);
        commands->SetGraphicsSRV(1, Indices->GetSRV()->GPUHandle);
        commands->SetGraphicsSRV(2, Payloads[0]->GetSRV()->GPUHandle);
        commands->SetGraphicsSRV(4, resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));
        commands->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (mode >= 3)
        {
            const D3D12_VERTEX_BUFFER_VIEW view = {FireflyVertices->GetElementAddress(0),
                FireflyParticleVertexCount * sizeof(GDX12ParticleMeshVertex), sizeof(GDX12ParticleMeshVertex)};
            commands->GetCommandList()->IASetVertexBuffers(0, 1, &view);
        }
        commands->ExecuteIndirect(DrawSignature.Get(), 1, DrawArgs->GetResource().D3DResource.Get(), mode < 3 ? mode * 16 : 72 + (mode - 3) * 16, nullptr);
    }
};
GDX12ParticlesPass::GDX12ParticlesPass()
{
    _flags = RENDER_PASS_FLAG_USE_CAMERAS | RENDER_PASS_FLAG_USE_MATERIALS;
    _numInputs = 3; // scene depth, existing WBOIT accumulation and revealage
}

GDX12ParticlesPass::~GDX12ParticlesPass() = default;

void GDX12ParticlesPass::Initialize()
{
    if (_inputs[1]->GetTexture()->GetFormat() != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        _inputs[2]->GetTexture()->GetFormat() != DXGI_FORMAT_R16_FLOAT)
        throw std::invalid_argument("Particles require HDR WBOIT accumulation/revealage");
}

void GDX12ParticlesPass::BeginFrame()
{
    _resources->ParticleMotion = nullptr;
    const auto global = GetParticleSettings();
    auto* simulation = _resources->ParticleSimulation.get();
    if (!simulation || !simulation->_state)
    {
        if (_state)
        {
            _resources->Device->GetCommandQueue()->Flush();
            _state.reset();
            _commonData->ResetUpscalerHistory = true;
        }
        return;
    }
    if (!global.Enabled || !global.DrawEnabled || !simulation->_state->Active)
    {
        if (_state)
        {
            _state->Active = false;
            if (_state->WasEnabled) _commonData->ResetUpscalerHistory = true;
            _state->WasEnabled = false;
        }
        return;
    }
    const auto& snapshot = *simulation->_state;
    if (!_state)
    {
        _state = std::make_unique<State>();
        _state->Initialize(_resources);
    }
    auto& state = *_state;
    state.Active = true;
    state.ResetMotion = snapshot.ResetMotion || !state.WasEnabled;
    if (!state.WasEnabled) _commonData->ResetUpscalerHistory = true;
    state.WasEnabled = true;
    bool sorted = false, mesh = false;
    for (const auto& emitter : snapshot.Emitters)
    {
        sorted |= emitter.Blend == ParticleBlend::SortedAlpha;
        mesh |= emitter.Geometry == ParticleGeometry::Firefly;
    }
    const bool drawsChanged = snapshot.Capacity != state.Capacity || sorted != state.Sorted || mesh != state.Mesh;
    const bool constantsChanged = snapshot.Emitters.size() != state.ConstantCount;
    if (drawsChanged || constantsChanged)
    {
        _resources->Device->GetCommandQueue()->Flush();
        if (drawsChanged) state.AllocateDraws(_resources, snapshot.Capacity, sorted, mesh);
        if (constantsChanged)
        {
            state.ConstantCount = static_cast<UINT>(snapshot.Emitters.size());
            state.Constants.clear();
            for (size_t i = 0; i < _resources->FrameConstants.size(); ++i)
                state.Constants.push_back(std::make_unique<Buffer<EmitterConstants>>(_resources->Device,
                    state.ConstantCount));
        }
        _commonData->ResetUpscalerHistory = true;
    }
    auto* depth = _inputs[0]->GetTexture();
    const UINT width = depth->GetWidth();
    const UINT height = depth->GetHeight();
    if (!state.MotionTexture)
    {
        GDX12TextureDesc desc;
        desc.Width = width;
        desc.Height = height;
        desc.Format = desc.SRVDesc.Format = desc.RTVDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.SRVDesc.Texture2D.MipLevels = 1;
        desc.CreateRTV = true;
        desc.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        desc.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
        desc.SRVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.RTVHeap = _resources->RTVHeap.get();
        desc.RTVHeapIndex = desc.RTVHeap->GetAvailableIndex();
        desc.ClearValue = XMFLOAT4(0, 0, 0, 0);
        state.MotionTexture = std::make_unique<GDX12Texture>();
        state.MotionTexture->Initialize(desc);
    }
    for (UINT i = 0; i < state.ConstantCount; ++i)
    {
        const auto& emitter = snapshot.Emitters[i];
        auto constants = MakeConstants(emitter, width, height);
        if (emitter.Texture)
        {
            auto* texture = emitter.Texture->PrimaryDeviceTexture;
            if (!texture || !texture->HasSRV() || texture->GetSemantic() != ETextureSemantic::Color ||
                texture->GetWidth() < emitter.AtlasColumns || texture->GetHeight() < emitter.AtlasRows ||
                texture->GetSRV()->HeapIndex < Texture2D_StartIndex ||
                texture->GetSRV()->HeapIndex >= Texture2D_StartIndex + Texture2D_RangeLength ||
                texture->GetResource()->D3DResource->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                texture->GetResource()->D3DResource->GetDesc().DepthOrArraySize != 1)
                throw std::invalid_argument("Particle atlas must be a renderer-owned 2D texture on the primary GPU");
            constants.TextureIndex = texture->GetSRV()->HeapIndex - Texture2D_StartIndex;
        }
        state.Constants[_resources->CurrFrameConstantsIndex]->CopyData(i, constants);
    }
}

void GDX12ParticlesPass::Execute(GDX12CommandList* commands)
{
    if (!_state || !_state->Active) return;
    auto& state = *_state;
    auto& simulation = *_resources->ParticleSimulation->_state;
    auto* depth = _inputs[0]->GetTexture();
    GDX12_SCOPED_EVENT(commands, "GPU particles", Colors::Orange);
    commands->SetComputeRootSignature(state.Root.get());
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    commands->ResourceBarrier({ depth->GetResource()->GetSRVBarrier() });
    commands->SetComputeSRV(3, depth->GetSRV()->GPUHandle);
    commands->SetComputeRootConstantBufferView(1, frame->CameraCB->GetElementAddress(_commonData->ActiveCameraCBufferIndex));
    UAV(commands, *state.Draws);
    UAV(commands, *state.Counts);
    UAV(commands, *state.Indices);
    UAV(commands, *state.Keys[0]);
    UAV(commands, *state.Payloads[0]);
    UAV(commands, *state.DrawArgs);
    UAV(commands, *state.SortParameters);
    state.BindGlobalUAVs(commands);
    commands->SetPipelineState(state.Compute.at("ClearDraws"));
    commands->Dispatch(1, 1, 1);
    UAV(commands, *state.Counts);
    for (UINT i = 0; i < state.ConstantCount; ++i)
    {
        const auto& emitter = simulation.Emitters[i];
        commands->SetComputeRootConstantBufferView(2, state.Constants[_resources->CurrFrameConstantsIndex]->GetElementAddress(i));
        UINT base = 0;
        const bool imported = simulation.Offload && simulation.Offload->Find(emitter.Id, base);
        const UINT parameters[] = {emitter.MaxParticles, base, state.Capacity, state.ResetMotion};
        commands->GetCommandList()->SetComputeRoot32BitConstants(0, 4, parameters, 0);
        if (emitter.Texture)
            commands->ResourceBarrier({emitter.Texture->PrimaryDeviceTexture->GetResource()->GetSRVBarrier()});
        if (imported)
        {
            auto& display = simulation.Offload->Display();
            commands->ResourceBarrier({display.GetResource().GetNonPixelShaderResourceBarrier()});
            commands->SetComputeSRV(0, display.GetSRV()->GPUHandle);
            commands->SetPipelineState(state.Compute.at("CullImported"));
            commands->Dispatch((emitter.MaxParticles + 255) / 256, 1, 1);
        }
        else
        {
            auto& pool = *simulation.Pools.at(emitter.Id);
            commands->ResourceBarrier({pool.Particles->GetResource().GetNonPixelShaderResourceBarrier(),
                pool.Alive[pool.ReadIndex]->GetResource().GetNonPixelShaderResourceBarrier(),
                pool.Counters->GetResource().GetNonPixelShaderResourceBarrier(), pool.Args->GetResource().GetIndirectArgsBarrier()});
            commands->SetComputeSRV(0, pool.Particles->GetSRV()->GPUHandle);
            commands->SetComputeSRV(1, pool.Alive[pool.ReadIndex]->GetSRV()->GPUHandle);
            commands->SetComputeSRV(2, pool.Counters->GetSRV()->GPUHandle);
            commands->SetPipelineState(state.Compute.at("Cull"));
            commands->ExecuteIndirect(state.DispatchSignature.Get(), 1, pool.Args->GetResource().D3DResource.Get(), 24, nullptr);
        }
        UAV(commands, *state.Counts);
        UAV(commands, *state.Draws);
        UAV(commands, *state.Indices);
        UAV(commands, *state.Keys[0]);
        UAV(commands, *state.Payloads[0]);
    }
    commands->SetPipelineState(state.Compute.at("BuildDrawArgs"));
    commands->Dispatch(1, 1, 1);
    commands->ResourceBarrier({ state.DrawArgs->GetResource().GetIndirectArgsBarrier() });
    if (state.Sorted) state.Sort(commands);
    commands->ResourceBarrier({ state.Draws->GetResource().GetSRVBarrier(), state.Indices->GetResource().GetSRVBarrier(),
        state.Payloads[0]->GetResource().GetSRVBarrier(), depth->GetResource()->GetSRVBarrier(),
        _inputs[1]->GetTexture()->GetResource()->GetRenderTargetBarrier(),
        _inputs[2]->GetTexture()->GetResource()->GetRenderTargetBarrier() });
    commands->SetViewport(depth->GetViewport());
    commands->SetScissorRect(depth->GetScissorRect());
    commands->SetRenderTargets({ _inputs[1]->GetTexture(), _inputs[2]->GetTexture() }, nullptr);
    commands->SetGraphicsRootSignature(state.Root.get());
    commands->SetGraphicsSRV(3, depth->GetSRV()->GPUHandle);
    commands->SetPipelineState(state.Weighted);
    state.Draw(commands, _resources, _commonData, 0);
    if (state.Mesh)
    {
        commands->SetPipelineState(state.MeshWeighted);
        state.Draw(commands, _resources, _commonData, 3);
    }
    commands->ResourceBarrier({ state.MotionTexture->GetResource()->GetRenderTargetBarrier() });
    commands->SetRenderTargets({ state.MotionTexture.get() }, nullptr);
    commands->ClearRenderTargetView(state.MotionTexture.get());
    commands->SetPipelineState(state.Motion);
    for (UINT mode = 0; mode < 3; ++mode) state.Draw(commands, _resources, _commonData, mode);
    if (state.Mesh)
    {
        commands->SetPipelineState(state.MeshMotion);
        for (UINT mode = 3; mode < 5; ++mode) state.Draw(commands, _resources, _commonData, mode);
    }
    commands->ResourceBarrier({ state.MotionTexture->GetResource()->GetSRVBarrier() });
    _resources->ParticleMotion = state.MotionTexture.get();
}

void GDX12ParticlesPass::Composite(GDX12CommandList* commands, GDX12Texture* hdr)
{
    if (!_state || !_state->Active) return;
    auto& state = *_state;
    auto* depth = _inputs[0]->GetTexture();
    GDX12_SCOPED_EVENT(commands, "Sorted and additive HDR particles", Colors::Orange);
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    commands->ResourceBarrier({ hdr->GetResource()->GetRenderTargetBarrier(), depth->GetResource()->GetSRVBarrier() });
    commands->SetViewport(hdr->GetViewport());
    commands->SetScissorRect(hdr->GetScissorRect());
    commands->SetRenderTargets({ hdr }, nullptr);
    commands->SetGraphicsRootSignature(state.Root.get());
    commands->SetGraphicsSRV(3, depth->GetSRV()->GPUHandle);
    if (state.Sorted)
    {
        commands->SetPipelineState(state.Alpha);
        state.Draw(commands, _resources, _commonData, 2);
    }
    commands->SetPipelineState(state.Additive);
    state.Draw(commands, _resources, _commonData, 1);
    if (state.Mesh)
    {
        commands->SetPipelineState(state.MeshAdditive);
        state.Draw(commands, _resources, _commonData, 4);
    }
}

void GDX12ParticlesPass::Resize()
{
    _resources->ParticleMotion = nullptr;
    if (_state)
    {
        _state->MotionTexture.reset();
        _state->WasEnabled = false;
    }
}

void GDX12ParticlesPass::ClearDenendencies()
{
    if (_resources) _resources->ParticleMotion = nullptr;
    _state.reset();
    GDX12RenderPass::ClearDenendencies();
}
