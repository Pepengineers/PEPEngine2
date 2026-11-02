#include "Engine.RendererDX12/RenderPasses/GDX12ParticlesPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12Particles.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace
{
    struct Particle
    {
        XMFLOAT3 Position; float Age;
        XMFLOAT3 Previous; float Lifetime;
        XMFLOAT3 Velocity; float Rotation;
        float PreviousAge, PreviousRotation, SizeRandom;
        UINT Seed;
    };
    struct ParticleDraw
    {
        XMFLOAT3 Position; float Size;
        XMFLOAT3 Previous; float Rotation;
        XMFLOAT4 Color;
        float SoftDistance; UINT TextureIndex, Frame, Columns;
        UINT Rows, EdgeSamples; float ViewDepth, PreviousSize;
        float PreviousRotation, Age, PreviousAge; UINT Geometry;
    };
    struct EmitterConstants
    {
        XMFLOAT4X4 World;
        XMFLOAT3 Direction; float ConeCos;
        XMFLOAT3 Extents; UINT Shape;
        XMFLOAT3 Acceleration; float DeltaTime;
        XMFLOAT3 Wind; float Drag;
        XMFLOAT4 ColorStart, ColorEnd;
        XMFLOAT2 Lifetimes, Speeds, Sizes;
        float SizeVariation, RenderScale;
        float EmissionRate, AngularVelocity, SoftDistance, MaxDistance;
        float MinPixelSize, FramesPerSecond; UINT TextureIndex, Blend;
        UINT Columns, Rows, Substeps, EdgeSamples;
        UINT Seed, Paused; XMFLOAT2 RenderSize;
        UINT DepthCollision; float CollisionRadius, CollisionRestitution, CollisionThickness;
        UINT Geometry; XMFLOAT3 Padding;
    };
    static_assert(sizeof(Particle) == 64);
    static_assert(sizeof(ParticleDraw) == 96);
    static_assert(sizeof(EmitterConstants) == 288);
    static_assert(sizeof(GDX12ParticleMeshVertex) == 44);

    template<class T>
    using Buffer = GDX12UploadBuffer<T>;

    template<class T>
    std::unique_ptr<Buffer<T>> CreateBuffer(GDX12DeviceResources* resources, UINT count)
    {
        auto buffer = std::make_unique<Buffer<T>>(resources->Device, count, EBufferType::Default, false);
        auto* heap = resources->SRV_UAV_Heap.get();
        // Keep buffers outside the material texture ranges and the small postprocess range.
        buffer->CreateSRV(heap, heap->GetAvailableIndex(TextureCube_StartIndex + TextureCube_RangeLength));
        buffer->CreateUAV(heap, heap->GetAvailableIndex(TextureCube_StartIndex + TextureCube_RangeLength));
        return buffer;
    }

    template<class T>
    void UAV(GDX12CommandList* commands, Buffer<T>& buffer)
    {
        auto& resource = buffer.GetResource();
        commands->ResourceBarrier({ resource.GetCurrentState() == D3D12_RESOURCE_STATE_UNORDERED_ACCESS ?
            resource.GetUAVBarrier() : resource.GetUnorderedAccessBarrier() });
    }

    struct Pool
    {
        UINT Capacity = 0;
        UINT ReadIndex = 0;
        uint64_t ResetRevision = 0;
        uint64_t BurstTotal = 0;
        bool Initialized = false;
        std::unique_ptr<Buffer<Particle>> Particles;
        std::unique_ptr<Buffer<UINT>> Alive[2];
        std::unique_ptr<Buffer<UINT>> Dead, Counters, Args;
        std::vector<std::unique_ptr<Buffer<EmitterConstants>>> Constants;

        Pool(GDX12DeviceResources* resources, UINT capacity) : Capacity(capacity)
        {
            Particles = CreateBuffer<Particle>(resources, capacity);
            Alive[0] = CreateBuffer<UINT>(resources, capacity);
            Alive[1] = CreateBuffer<UINT>(resources, capacity);
            Dead = CreateBuffer<UINT>(resources, capacity);
            Counters = CreateBuffer<UINT>(resources, 8);
            Args = CreateBuffer<UINT>(resources, 9);
            for (size_t i = 0; i < resources->FrameConstants.size(); ++i)
                Constants.push_back(std::make_unique<Buffer<EmitterConstants>>(resources->Device, 1));
        }
    };
}

struct GDX12ParticlesPass::State
{
    UINT Capacity = 0;
    bool Sorted = false;
    bool Mesh = false;
    bool Active = false;
    bool WasEnabled = false;
    bool ResetMotion = true;
    std::unordered_map<uint64_t, std::unique_ptr<Pool>> Pools;
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
        GDX12RootSignatureDesc root;
        root.VolatileSRVData = true;
        root.Constants = { 4 };
        root.NumSingleCBVSlots = 2;
        root.NumSingleSRVSlots = 4;
        root.SRVRanges.emplace_back(Texture2D_RangeLength);
        root.NumSingleUAVSlots = 12;
        root.StaticSamplers.emplace_back(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        Root = std::make_unique<GDX12RootSignature>(resources->Device, root);
        for (const auto* entry : { "InitializePool", "PrepareSimulation", "Simulate", "PrepareSpawn", "Spawn",
            "PublishAlive", "Cull", "ClearDraws", "BuildDrawArgs" })
            CompileCompute(resources, "ParticleSimulation.hlsl", entry, Root.get());

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
            FireflyVertices = std::make_unique<Buffer<GDX12ParticleMeshVertex>>(resources->Device, FireflyParticleVertexCount, EBufferType::Upload, false);
            FireflyVertices->CopyData(0, GetFireflyParticleMesh().data(), FireflyParticleVertexCount);
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

    void AllocateDraws(GDX12DeviceResources* resources, UINT capacity, bool sorted, bool mesh)
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

    void Draw(GDX12CommandList* commands, GDX12DeviceResources* resources, RenderPipelineCommonData* common, UINT mode)
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

void GDX12ParticlesPass::Execute(GDX12CommandList* commands)
{
    _resources->ParticleMotion = nullptr;
    const auto global = GetParticleSettings();
    if (_resources->ParticleEmitters.empty())
    {
        if (_state)
        {
            _resources->Device->GetCommandQueue()->Flush();
            _state.reset();
            _commonData->ResetUpscalerHistory = true;
        }
        return;
    }
    if (!global.Enabled)
    {
        if (_state)
        {
            _state->Active = false;
            if (_state->WasEnabled) _commonData->ResetUpscalerHistory = true;
            _state->WasEnabled = false;
        }
        return;
    }
    std::vector<GDX12ParticleEmitter> emitters = _resources->ParticleEmitters;
    std::unordered_set<uint64_t> ids;
    UINT total = 0;
    bool sorted = false;
    bool mesh = false;
    for (auto& emitter : emitters)
    {
        if (!emitter.Id || !ids.insert(emitter.Id).second || !IsValidParticleEmitterSettings(emitter) ||
            !std::isfinite(emitter.DeltaTime) || emitter.DeltaTime < 0)
            throw std::invalid_argument("Invalid particle emitter snapshot");
        const auto* matrix = reinterpret_cast<const float*>(&emitter.World);
        for (UINT i = 0; i < 16; ++i)
            if (!std::isfinite(matrix[i])) throw std::invalid_argument("Nonfinite particle transform");
        const float determinant = emitter.World.Determinant();
        if (!std::isfinite(determinant) || std::abs(determinant) < 0.000001f)
            throw std::invalid_argument("Singular particle transform");
        if (emitter.World._14 != 0 || emitter.World._24 != 0 || emitter.World._34 != 0 || emitter.World._44 != 1)
            throw std::invalid_argument("Particle transform must be affine");
        static_cast<GDX12ParticleEmitterSettings&>(emitter) = ResolveParticleEmitterSettings(emitter, global.Quality);
        if (emitter.MaxParticles > 4194304 - total) throw std::length_error("Particle scene capacity exceeds 4194304");
        total += emitter.MaxParticles;
        sorted |= emitter.Blend == ParticleBlend::SortedAlpha;
        mesh |= emitter.Geometry != ParticleGeometry::Billboard;
    }
    if (!_state)
    {
        _state = std::make_unique<State>();
        _state->Initialize(_resources);
    }
    auto& state = *_state;
    state.Active = true;
    state.ResetMotion = _commonData->ResetHistory || !state.WasEnabled;
    if (!state.WasEnabled) _commonData->ResetUpscalerHistory = true;
    state.WasEnabled = true;
    bool changed = total != state.Capacity || sorted != state.Sorted || mesh != state.Mesh;
    for (const auto& pool : state.Pools) changed |= ids.count(pool.first) == 0;
    for (const auto& emitter : emitters)
    {
        const auto found = state.Pools.find(emitter.Id);
        changed |= found == state.Pools.end() || found->second->Capacity != emitter.MaxParticles;
    }
    if (changed)
    {
        // All replacements happen before recording any particle work, after earlier submissions retire.
        _resources->Device->GetCommandQueue()->Flush();
        for (auto it = state.Pools.begin(); it != state.Pools.end();)
            if (!ids.count(it->first)) it = state.Pools.erase(it);
            else ++it;
        for (const auto& emitter : emitters)
        {
            auto& pool = state.Pools[emitter.Id];
            if (!pool || pool->Capacity != emitter.MaxParticles)
                pool = std::make_unique<Pool>(_resources, emitter.MaxParticles);
        }
        if (total != state.Capacity || sorted != state.Sorted || mesh != state.Mesh) state.AllocateDraws(_resources, total, sorted, mesh);
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
    for (const auto& emitter : emitters)
    {
        auto& pool = *state.Pools.at(emitter.Id);
        const bool reset = !pool.Initialized || pool.ResetRevision != emitter.ResetRevision;
        if (reset)
        {
            _commonData->ResetUpscalerHistory = true;
            pool.ReadIndex = 0;
            pool.BurstTotal = 0;
            pool.ResetRevision = emitter.ResetRevision;
            pool.Initialized = true;
        }
        const uint64_t pending = emitter.BurstTotal >= pool.BurstTotal ? emitter.BurstTotal - pool.BurstTotal : 0;
        const UINT burst = emitter.Paused ? 0 : static_cast<UINT>((std::min)(pending, uint64_t(emitter.MaxParticles)));
        if (!emitter.Paused) pool.BurstTotal = emitter.BurstTotal;
        EmitterConstants constants = {};
        XMStoreFloat4x4(&constants.World, XMMatrixTranspose(emitter.World));
        constants.Direction = emitter.Direction;
        constants.ConeCos = std::cos(XMConvertToRadians(emitter.ConeAngleDegrees));
        constants.Extents = emitter.ShapeExtents;
        constants.Shape = static_cast<UINT>(emitter.Shape);
        constants.Acceleration = emitter.Acceleration;
        constants.DeltaTime = emitter.Paused ? 0 : (std::min)(emitter.DeltaTime * emitter.TimeScale, emitter.MaxDeltaTime);
        constants.Wind = emitter.Wind;
        constants.Drag = emitter.Drag;
        constants.ColorStart = emitter.ColorStart;
        constants.ColorEnd = emitter.ColorEnd;
        constants.Lifetimes = XMFLOAT2(emitter.LifetimeMin, emitter.LifetimeMax);
        constants.Speeds = XMFLOAT2(emitter.SpeedMin, emitter.SpeedMax);
        constants.Sizes = XMFLOAT2(emitter.SizeStart, emitter.SizeEnd);
        constants.SizeVariation = emitter.SizeVariation;
        constants.RenderScale = emitter.RenderScale;
        constants.EmissionRate = emitter.EmissionRate;
        constants.AngularVelocity = emitter.AngularVelocity;
        constants.SoftDistance = emitter.SoftDistance;
        constants.MaxDistance = emitter.MaxDistance;
        constants.MinPixelSize = emitter.MinPixelSize;
        constants.FramesPerSecond = emitter.FramesPerSecond;
        constants.TextureIndex = UINT_MAX;
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
            commands->ResourceBarrier({ texture->GetResource()->GetSRVBarrier() });
        }
        constants.Blend = static_cast<UINT>(emitter.Blend);
        constants.Columns = emitter.AtlasColumns;
        constants.Rows = emitter.AtlasRows;
        constants.Substeps = emitter.Substeps;
        constants.EdgeSamples = emitter.EdgeSamples;
        constants.Seed = emitter.Seed;
        constants.Paused = emitter.Paused;
        constants.RenderSize = XMFLOAT2(static_cast<float>(width), static_cast<float>(height));
        constants.DepthCollision = emitter.DepthCollision;
        constants.CollisionRadius = emitter.CollisionRadius;
        constants.CollisionRestitution = emitter.CollisionRestitution;
        constants.CollisionThickness = emitter.CollisionThickness;
        constants.Geometry = static_cast<UINT>(emitter.Geometry);
        pool.Constants[_resources->CurrFrameConstantsIndex]->CopyData(0, constants);
        commands->SetComputeRootConstantBufferView(2, pool.Constants[_resources->CurrFrameConstantsIndex]->GetElementAddress(0));
        const UINT parameters[] = { pool.Capacity, burst, state.Capacity, state.ResetMotion || reset };
        commands->GetCommandList()->SetComputeRoot32BitConstants(0, 4, parameters, 0);
        UAV(commands, *pool.Particles);
        UAV(commands, *pool.Alive[pool.ReadIndex ^ 1]);
        UAV(commands, *pool.Dead);
        UAV(commands, *pool.Counters);
        UAV(commands, *pool.Args);
        commands->ResourceBarrier({ pool.Alive[pool.ReadIndex]->GetResource().GetNonPixelShaderResourceBarrier() });
        commands->SetComputeSRV(1, pool.Alive[pool.ReadIndex]->GetSRV()->GPUHandle);
        commands->SetComputeUAV(0, pool.Particles->GetUAV()->GPUHandle);
        commands->SetComputeUAV(1, pool.Alive[pool.ReadIndex ^ 1]->GetUAV()->GPUHandle);
        commands->SetComputeUAV(2, pool.Dead->GetUAV()->GPUHandle);
        commands->SetComputeUAV(3, pool.Counters->GetUAV()->GPUHandle);
        commands->SetComputeUAV(4, pool.Args->GetUAV()->GPUHandle);
        if (reset)
        {
            commands->SetPipelineState(state.Compute.at("InitializePool"));
            commands->Dispatch((pool.Capacity + 255) / 256, 1, 1);
            UAV(commands, *pool.Counters);
            UAV(commands, *pool.Particles);
            UAV(commands, *pool.Dead);
        }
        commands->SetPipelineState(state.Compute.at("PrepareSimulation"));
        commands->Dispatch(1, 1, 1);
        UAV(commands, *pool.Counters);
        commands->ResourceBarrier({ pool.Args->GetResource().GetIndirectArgsBarrier() });
        commands->SetPipelineState(state.Compute.at("Simulate"));
        commands->ExecuteIndirect(state.DispatchSignature.Get(), 1, pool.Args->GetResource().D3DResource.Get(), 0, nullptr);
        UAV(commands, *pool.Counters);
        UAV(commands, *pool.Dead);
        UAV(commands, *pool.Particles);
        UAV(commands, *pool.Alive[pool.ReadIndex ^ 1]);
        UAV(commands, *pool.Args);
        commands->SetPipelineState(state.Compute.at("PrepareSpawn"));
        commands->Dispatch(1, 1, 1);
        UAV(commands, *pool.Counters);
        commands->ResourceBarrier({ pool.Args->GetResource().GetIndirectArgsBarrier() });
        commands->SetPipelineState(state.Compute.at("Spawn"));
        commands->ExecuteIndirect(state.DispatchSignature.Get(), 1, pool.Args->GetResource().D3DResource.Get(), 12, nullptr);
        UAV(commands, *pool.Counters);
        UAV(commands, *pool.Particles);
        UAV(commands, *pool.Args);
        commands->SetPipelineState(state.Compute.at("PublishAlive"));
        commands->Dispatch(1, 1, 1);
        UAV(commands, *pool.Counters);
        commands->ResourceBarrier({ pool.Args->GetResource().GetIndirectArgsBarrier(),
            pool.Alive[pool.ReadIndex ^ 1]->GetResource().GetNonPixelShaderResourceBarrier() });
        commands->SetComputeSRV(1, pool.Alive[pool.ReadIndex ^ 1]->GetSRV()->GPUHandle);
        commands->SetPipelineState(state.Compute.at("Cull"));
        commands->ExecuteIndirect(state.DispatchSignature.Get(), 1, pool.Args->GetResource().D3DResource.Get(), 24, nullptr);
        UAV(commands, *state.Counts);
        UAV(commands, *state.Draws);
        UAV(commands, *state.Indices);
        UAV(commands, *state.Keys[0]);
        UAV(commands, *state.Payloads[0]);
        pool.ReadIndex ^= 1;
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
