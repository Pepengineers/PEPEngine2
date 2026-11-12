#include "Rendering/Grass/GDX12GrassRenderer.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12LocalLights.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include "BistroArch.h"

namespace
{
    template<typename T> void Hash(uint64_t& hash, const T& value)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
    uint64_t SettingsHash(const GDX12GrassInstance& s, uint64_t& rootsHash)
    {
        uint64_t hash = 14695981039346656037ull;
        Hash(hash, s.Implementation); Hash(hash, s.ExtentsCm); Hash(hash, s.DensityPerSquareMetre);
        Hash(hash, s.MaxBlades); Hash(hash, s.Segments); Hash(hash, s.Seed); Hash(hash, s.HeightCm);
        Hash(hash, s.HeightVariation); Hash(hash, s.WidthCm); Hash(hash, s.BendSeconds); Hash(hash, s.MaxBendCm);
        Hash(hash, s.LodDistanceCm); Hash(hash, s.FarCardDistanceCm); Hash(hash, s.MaxDistanceCm);
        Hash(hash, s.WindId); Hash(hash, s.MaterialIndex); Hash(hash, s.MaterialRevision); Hash(hash, s.ResetRevision);
        Hash(hash, s.CastShadows); Hash(hash, s.CaptureReflections);
        rootsHash = 14695981039346656037ull;
        Hash(rootsHash, s.RootsCm.size());
        for (const auto& root : s.RootsCm) Hash(rootsHash, root);
        Hash(hash, rootsHash);
        return hash;
    }
    GDX12GrassFieldSnapshot::View ViewOf(GDX12GrassFieldProducer::FieldBuffer& buffer)
    {
        return { &buffer.GetResource(), buffer.GetSRV()->GPUHandle };
    }

    bool ValidWorld(const Matrix& world)
    {
        const auto* values = &world._11;
        for (UINT i = 0; i < 16; ++i) if (!std::isfinite(values[i]) || std::abs(values[i]) > 1e7f) return false;
        const float determinant = world.Determinant();
        return std::isfinite(determinant) && std::abs(determinant) >= 1e-9f;
    }
}

XMFLOAT4 GDX12GrassRenderer::BistroInteractionArea()
{
    static const auto area = []
    {
        BoundingBox bounds;
        BistroArch::SwarmBounds.Transform(bounds, Matrix::CreateTranslation(-Vector3(BistroArch::SwarmBounds.Center)) *
            Matrix::CreateRotationY(XMConvertToRadians(BistroArch::YawDegrees)) *
            Matrix::CreateTranslation(Vector3(BistroArch::SwarmBounds.Center)));
        return XMFLOAT4(bounds.Center.x, bounds.Center.z, bounds.Extents.x + 100, bounds.Extents.z + 100);
    }();
    return area;
}

uint64_t GDX12GrassRenderer::FieldInputSignature(const std::vector<GDX12GrassInstance>& grass,
    const GDX12GrassWindControls& controls)
{
    std::vector<std::pair<uint64_t, uint64_t>> patches;
    patches.reserve(grass.size());
    for (const auto& patch : grass)
    {
        uint64_t roots = 0;
        patches.emplace_back(patch.Id, SettingsHash(patch, roots));
    }
    std::sort(patches.begin(), patches.end());
    uint64_t revision = 14695981039346656037ull;
    Hash(revision, controls.GrassEnabled);
    Hash(revision, controls.GrassQuality);
    Hash(revision, controls.WindEnabled);
    Hash(revision, controls.WindQuality);
    for (const auto& [id, settings] : patches)
    {
        Hash(revision, id);
        Hash(revision, settings);
    }
    return revision;
}

GDX12GrassRenderer::GDX12GrassRenderer(GDX12DeviceResources* resources) : _resources(resources), _frames(resources->FrameConstants.size()) {}
GDX12GrassRenderer::GDX12GrassRenderer(GDX12DeviceResources* resources, ShadowMaterials materials)
    : GDX12GrassRenderer(resources)
{
    SetShadowMaterials(materials);
}

void GDX12GrassRenderer::SetShadowMaterials(ShadowMaterials materials)
{
    if (!materials.Heap || !materials.Buffer || !materials.Table.ptr || !materials.AlphaTable.ptr || !materials.Count)
        throw std::invalid_argument("Invalid native grass shadow material binding");
    _shadowMaterials = materials;
}

void GDX12GrassRenderer::ReleaseShadowFrame()
{
    assert(_shadowMaterials);
    auto& frame = CurrentFrame();
    _arenaOutput = nullptr;
    frame.OutputIndex.clear();
    frame.Outputs.clear();
    frame.RootUploads.clear();
    frame.SnapshotWindOwners.clear();
    frame.Snapshots = {};
}

GDX12DescriptorHeap* GDX12GrassRenderer::BindingHeap() const
{
    return _shadowMaterials ? _shadowMaterials->Heap : _resources->SRV_UAV_Heap.get();
}

GDX12GrassRenderer::~GDX12GrassRenderer()
{ if (_primitiveRegistry) RemovePrimitives(*_primitiveRegistry); }
GDX12GrassRenderer::Frame& GDX12GrassRenderer::CurrentFrame() { return _frames.at(_resources->CurrFrameConstantsIndex); }

template<typename T>
std::unique_ptr<GDX12UploadBuffer<T>> GDX12GrassRenderer::Buffer(UINT count, const bool srv, const bool uav)
{
    auto buffer = std::make_unique<GDX12UploadBuffer<T>>(_resources->Device, count, EBufferType::Default, false);
    auto* heap = BindingHeap();
    if (srv) buffer->CreateSRV(heap, heap->GetAvailableIndex(ConstantsResources));
    if (uav) buffer->CreateUAV(heap, heap->GetAvailableIndex(ConstantsResources));
    return buffer;
}

void GDX12GrassRenderer::Initialize()
{
    GDX12RootSignatureDesc compute;
    compute.VolatileSRVData = true;
    compute.NumSingleCBVSlots = 1;
    compute.NumSingleSRVSlots = 5;
    compute.NumSingleUAVSlots = 3;
    _computeRoot = std::make_unique<GDX12RootSignature>(_resources->Device, compute);
    GDX12RootSignatureDesc graphics;
    graphics.VolatileSRVData = true;
    graphics.NumSingleCBVSlots = 4;
    graphics.NumSingleSRVSlots = 8;
    graphics.SRVRanges.push_back(GDX12RootSignatureRange(Texture2D_RangeLength));
    graphics.StaticSamplers = GetStaticSamplers();
    CD3DX12_STATIC_SAMPLER_DESC shadowSampler(6, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT);
    shadowSampler.AddressU = shadowSampler.AddressV = shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    graphics.StaticSamplers.push_back(shadowSampler);
    _graphicsRoot = std::make_unique<GDX12RootSignature>(_resources->Device, graphics);
    auto& compiler = GDX12ShaderCompiler::GetInstance();
    _expandPSO = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _computeRoot.get(), APP_SHADERS_FOLDER "GrassExpand.hlsl", "ExpandCS");
    _clearPSO = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _computeRoot.get(), APP_SHADERS_FOLDER "GrassExpand.hlsl", "ClearCS");
    for (UINT i = 0; i < _graphicsPSO.size(); ++i)
    {
        const auto stage = static_cast<Stage>(i);
        if (_shadowMaterials && stage != Stage::Shadow) continue;
        const bool depth = stage == Stage::Depth || stage == Stage::Shadow;
        const bool capture = stage == Stage::Capture;
        D3D_SHADER_MACRO captureDefines[] = { { "GRASS_CAPTURE", "1" }, { nullptr, nullptr } };
        auto vs = compiler.CompileShader(_resources->Device, APP_SHADERS_FOLDER "GrassRender.hlsl", capture ? captureDefines : nullptr,
            depth ? "DepthVS" : "VS", "vs");
        auto ps = compiler.CompileShader(_resources->Device, APP_SHADERS_FOLDER "GrassRender.hlsl", capture ? captureDefines : nullptr,
            depth ? "DepthPS" : (capture ? "CapturePS" : (stage == Stage::PrepassNormals ? "PrepassPS" : "PS")), "ps");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = _graphicsRoot->GetRootSignature().Get();
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.FrontCounterClockwise = TRUE;
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        desc.DepthStencilState.DepthFunc = stage == Stage::Shadow ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        if (stage == Stage::GBuffer)
        {
            desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
            desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        }
        if (stage == Stage::Shadow)
        {
            desc.RasterizerState.DepthBias = 250;
            desc.RasterizerState.SlopeScaledDepthBias = 1.5f;
        }
        desc.NumRenderTargets = depth ? 0 : (capture || stage == Stage::PrepassNormals ? 1 : 5);
        if (capture || stage == Stage::PrepassNormals) desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        else if (!depth)
        {
            desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
            desc.RTVFormats[2] = desc.RTVFormats[3] = desc.RTVFormats[4] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        }
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        desc.SampleMask = UINT_MAX;
        desc.SampleDesc.Count = 1;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_graphicsPSO[i])));
    }
    D3D12_INDIRECT_ARGUMENT_DESC argument = {};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC signature = {};
    signature.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    signature.NumArgumentDescs = 1;
    signature.pArgumentDescs = &argument;
    ThrowIfFailed(_resources->Device->GetDevice()->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&_signature)));
    _initialized = true;
}

void GDX12GrassRenderer::BeginFrame(const RenderPipelineCommonData& common,
    std::vector<GDX12GrassInstance> grass, std::vector<GDX12WindInstance> wind, GDX12GrassWindControls controls,
    const std::vector<BistroInteractionSource>& sources, float deltaSeconds, const DisplayedFields* fields)
{
    auto& frame = CurrentFrame();
    DisplayedFields snapshots = fields ? *fields : DisplayedFields{};
    if (_shadowMaterials && !snapshots.Current) throw std::invalid_argument("Native grass shadow requires a captured field");
    if (snapshots.Current)
    {
        if (snapshots.Current->GetHeap() != BindingHeap())
            throw std::invalid_argument("Grass snapshot must use the renderer's device-local heap");
        if (!snapshots.Previous || snapshots.Previous->GetHeap() != snapshots.Current->GetHeap() ||
            !snapshots.Current->GetLayout().IsCompatibleWith(snapshots.Previous->GetLayout()))
        {
            snapshots.Previous = snapshots.Current;
            snapshots.ResetHistory = true;
        }
    }
    const bool routeChange = _usingSnapshot != static_cast<bool>(snapshots.Current);
    const bool restartLocal = _restartLocal && !snapshots.Current;
    const bool resetRenderHistory = common.ResetHistory || routeChange || _resumeSeed || snapshots.ResetHistory || restartLocal;
    _usingSnapshot = static_cast<bool>(snapshots.Current);
    frame.Snapshots = std::move(snapshots);
    frame.LocalSolver.reset();
    frame.Checkpoint.reset();
    frame.SnapshotWindOwners.clear();
    frame.RetiredVertices.clear();
    frame.RetiredIndices.clear();
    frame.RetiredGrass.clear();
    frame.DrawCursor = 0;
    frame.RootUploads.clear();
    frame.Outputs.clear();
    frame.OutputIndex.clear();
    _arenaOutput = nullptr;
    if (!_initialized) Initialize();
    const bool controlChange = controls.GrassEnabled != _controls.GrassEnabled || controls.GrassQuality != _controls.GrassQuality ||
        controls.WindEnabled != _controls.WindEnabled || controls.WindQuality != _controls.WindQuality;
    _controls = controls;
    _previousEye = _eye;
    _eye = common.CameraView.Invert().Translation();
    if (resetRenderHistory) _previousEye = _eye;
    bool resetInteraction = restartLocal || (routeChange && !_resumeSeed);
    _drawOrder.clear();
    std::unordered_set<uint64_t> activeGrass, activeWind;
    UINT vertexCapacity = 0, indexCapacity = 0;
    if (controls.GrassEnabled && controls.GrassQuality > 0)
    {
        const auto materialCount = _shadowMaterials ? _shadowMaterials->Count :
            _resources->FrameConstants[_resources->CurrFrameConstantsIndex]->MaterialCache->GetElementCount();
        for (auto& source : grass)
        {
            if (!source.Id || !IsValidGrassSettings(source) || !ValidWorld(source.World) ||
                source.MaterialIndex >= materialCount)
                throw std::invalid_argument("Invalid grass instance");
            static_cast<GDX12GrassSettings&>(source) = ResolveGrassSettings(source, controls.GrassQuality);
            const UINT count = GrassBladeCount(source);
            if (!count) continue;
            if (!activeGrass.insert(source.Id).second) throw std::invalid_argument("Duplicate grass owner");
            auto& state = _grass[source.Id];
            uint64_t rootsHash;
            const auto signature = SettingsHash(source, rootsHash);
            state.ResetHistory = resetRenderHistory || controlChange || state.SettingsHash != signature;
            resetInteraction |= state.SettingsHash != signature;
            state.PreviousWorld = state.ResetHistory ? source.World : state.Source.World;
            state.SettingsHash = signature;
            state.Source = std::move(source);
            GrassLocalBounds(state.Source).Transform(state.Bounds, state.Source.World);
            state.Bounds.Extents.x += state.Source.MaxBendCm;
            state.Bounds.Extents.z += state.Source.MaxBendCm;
            const float interactionReach = state.Source.HeightCm * (1 + state.Source.HeightVariation) *
                Vector3::TransformNormal(Vector3::UnitY, state.Source.World).Length();
            state.Bounds.Extents.x += interactionReach;
            state.Bounds.Extents.z += interactionReach;
            if (state.Source.WindId) activeWind.insert(state.Source.WindId);
            _drawOrder.push_back(state.Source.Id);
            const UINT ribbons = state.Source.Implementation == GDX12GrassImplementation::CrossedRibbons ? 2u : 1u;
            vertexCapacity = (std::max)(vertexCapacity, count * (state.Source.Segments + 1) * 2 * ribbons);
            indexCapacity = (std::max)(indexCapacity, count * state.Source.Segments * 6 * ribbons);
            if (state.Roots.empty())
            {
                state.Roots.resize(_frames.size());
                state.RootsUpload.resize(_frames.size());
                state.RootSignatures.resize(_frames.size());
            }
            const auto slot = _resources->CurrFrameConstantsIndex;
            auto& roots = state.Roots[slot];
            auto& upload = state.RootsUpload[slot];
            const UINT rootCount = (std::max)(1u, static_cast<UINT>(state.Source.RootsCm.size()));
            // Native shadow preparation can be declined before its root copy is recorded.
            bool dirty = _shadowMaterials || state.RootSignatures[slot] != rootsHash;
            if (!roots || roots->GetElementCount() < rootCount)
            {
                roots = Buffer<XMFLOAT3>(rootCount, true, false);
                upload = std::make_unique<GDX12UploadBuffer<XMFLOAT3>>(_resources->Device, rootCount, EBufferType::Upload, false);
                dirty = true;
            }
            if (dirty)
            {
                if (state.Source.RootsCm.empty()) upload->CopyData(0, XMFLOAT3(0, 0, 0));
                else upload->CopyData(0, state.Source.RootsCm.data(), static_cast<UINT>(state.Source.RootsCm.size()));
                frame.RootUploads.push_back({ roots.get(), upload.get() });
                state.RootSignatures[slot] = rootsHash;
            }
        }
    }
    for (auto it = _grass.begin(); it != _grass.end();)
    {
        if (activeGrass.count(it->first)) { ++it; continue; }
        _removedPrimitives.push_back(it->second.Primitive);
        frame.RetiredGrass.push_back(std::move(it->second));
        it = _grass.erase(it);
    }
    if (resetInteraction) ++_interactionResetRevision;
    wind.erase(std::remove_if(wind.begin(), wind.end(), [&](const auto& source)
    {
        return !controls.WindEnabled || controls.WindQuality <= 0 || !activeWind.count(source.Id);
    }), wind.end());
    if (frame.Snapshots.Current)
    {
        for (const auto& source : wind)
        {
            if (!frame.Snapshots.Current->MatchesWind(source, controls.WindQuality))
                throw std::invalid_argument("Grass snapshot does not match prepared wind inputs");
            frame.SnapshotWindOwners.insert(source.Id);
        }
    }
    else
    {
        if (!_fieldProducer)
            _fieldProducer = std::make_shared<GDX12GrassFieldProducer>(*_resources->Device, *_resources->SRV_UAV_Heap, _frames.size());
        const auto slot = _resources->CurrFrameConstantsIndex;
        const auto area = BistroInteractionArea();
        const bool resetWind = controlChange || (routeChange && !_resumeSeed);
        const GDX12GrassFieldProducer::Inputs current{
            wind, sources, area, deltaSeconds, controls.WindQuality, resetWind,
            vertexCapacity != 0, _interactionResetRevision, _carryWindDebt };
        if (_localContinuation && (resetInteraction || resetWind || !_fieldProducer->MatchesCurrent(current)))
        {
            // An authored reset closes the old input prefix.
            _localContinuation->Reset();
            _localContinuation.reset();
        }
        if (controls.UpdateHz >= 0)
        {
            if (!_localContinuation) _localContinuation.emplace();
            auto& pending = *_localContinuation;
            const auto now = std::chrono::steady_clock::now();
            pending.Push({pending.LastSeen() + 1, wind, sources, area, deltaSeconds,
                controls.WindQuality, resetWind, _interactionResetRevision, now});
            const bool due = resetInteraction || resetWind || _lastLocalSolve == std::chrono::steady_clock::time_point{} ||
                controls.UpdateHz == 0 || std::chrono::duration<double>(now - _lastLocalSolve).count() >= 1.0 / controls.UpdateHz;
            if (due && pending.Size())
            {
                const auto count = (std::min)(pending.Size(), GDX12GrassFieldProducer::MaxStepsPerInvocation);
                std::vector<GDX12GrassFieldProducer::Inputs> batch;
                for (size_t i = 0; i < count; ++i) batch.push_back(pending.At(i).Inputs());
                batch.front().NeedNeutralWind = vertexCapacity != 0;
                _fieldProducer->PrepareBatch(slot, batch.data(), batch.size());
                _fieldProducer->Commit(slot);
                _lastLocalInput = pending.At(count - 1).CapturedAt;
                for (size_t i = 0; i < count; ++i) pending.Commit(pending.Front()->Sequence);
                _lastLocalSolve = now;
                frame.LocalSolver = _fieldProducer;
            }
        }
        else if (_localContinuation && _localContinuation->Size())
        {
            auto& pending = *_localContinuation;
            const auto count = (std::min)(pending.Size(), GDX12GrassFieldProducer::MaxStepsPerInvocation);
            std::vector<GDX12GrassFieldProducer::Inputs> batch;
            for (size_t i = 0; i < count; ++i) batch.push_back(pending.At(i).Inputs());
            const bool includesCurrent = count < GDX12GrassFieldProducer::MaxStepsPerInvocation;
            if (includesCurrent)
                batch.push_back(current);
            batch.front().NeedNeutralWind = vertexCapacity != 0;
            _fieldProducer->PrepareBatch(slot, batch.data(), batch.size());
            _fieldProducer->Commit(slot);
            for (size_t i = 0; i < count; ++i) pending.Commit(pending.Front()->Sequence);
            if (includesCurrent) _localContinuation.reset();
            else if (!pending.Push({ pending.LastSeen() + 1, std::move(wind), sources, area, deltaSeconds,
                controls.WindQuality, false, _interactionResetRevision, std::chrono::steady_clock::now() }))
                throw std::logic_error("Local grass continuation lost an ordered input");
        }
        else
        {
            _fieldProducer->Prepare(slot, current);
            _fieldProducer->Commit(slot);
        }
        if (controls.UpdateHz < 0) frame.LocalSolver = _fieldProducer;
        _resumeSeed = false;
        _restartLocal = false;
        for (const auto id : _drawOrder)
        {
            auto& state = _grass.at(id);
            if (const auto field = _fieldProducer->FindWind(state.Source.WindId)) state.ResetHistory |= field->ResetHistory;
        }
    }
    if (!vertexCapacity)
    {
        frame.DrawParameters.clear();
        if (_vertices) frame.RetiredVertices.push_back(std::move(_vertices));
        if (_indices) frame.RetiredIndices.push_back(std::move(_indices));
        if (_arguments) frame.RetiredIndices.push_back(std::move(_arguments));
        return;
    }
    if (!_vertices || _vertices->GetElementCount() < vertexCapacity)
    {
        if (_vertices) frame.RetiredVertices.push_back(std::move(_vertices));
        _vertices = Buffer<Vertex>(vertexCapacity, true, true);
    }
    if (!_indices || _indices->GetElementCount() < indexCapacity)
    {
        if (_indices) frame.RetiredIndices.push_back(std::move(_indices));
        _indices = Buffer<UINT>(indexCapacity, false, true);
    }
    if (!_arguments) _arguments = Buffer<UINT>(8, false, true);
}

std::shared_ptr<const GDX12GrassRenderer::LocalCheckpoint> GDX12GrassRenderer::PrepareLocalCheckpoint()
{
    auto& frame = CurrentFrame();
    if (_restartLocal || !frame.LocalSolver || (_localContinuation && _localContinuation->Size())) return {};
    if (!frame.Checkpoint)
    {
        auto checkpoint = std::make_shared<LocalCheckpoint>();
        checkpoint->Layout = frame.LocalSolver->Snapshot(_resources->CurrFrameConstantsIndex);
        checkpoint->Packed = std::make_shared<GDX12Resource>();
        checkpoint->Packed->D3DResource = _resources->Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT,
            CD3DX12_RESOURCE_DESC::Buffer(UINT64(checkpoint->Layout.ElementCount) * sizeof(XMFLOAT4)),
            D3D12_RESOURCE_STATE_COMMON);
        frame.Checkpoint = std::move(checkpoint);
    }
    return frame.Checkpoint;
}

void GDX12GrassRenderer::RestartLocalSolver()
{
    _fieldProducer.reset();
    _localContinuation.reset();
    _carryWindDebt = false;
    _resumeSeed = false;
    _restartLocal = true;
    _lastLocalSolve = _lastLocalInput = {};
}

GDX12GrassRenderer::LocalSimulationStatus GDX12GrassRenderer::GetLocalSimulationStatus() const
{
    LocalSimulationStatus result;
    result.Ready = _fieldProducer && !_restartLocal;
    if (_lastLocalInput != std::chrono::steady_clock::time_point{})
        result.AgeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _lastLocalInput).count();
    if (_localContinuation)
    {
        result.DebtSeconds = _localContinuation->PendingSeconds();
        result.MissingSeconds = _localContinuation->MissingSeconds();
    }
    if (result.Ready)
    {
        double windDebt = 0;
        for (const auto& wind : _fieldProducer->CurrentSnapshot().Wind)
            windDebt = (std::max)(windDebt, wind.WindDebtSeconds);
        result.DebtSeconds += windDebt;
        result.AgeMs += windDebt * 1000;
    }
    return result;
}

void GDX12GrassRenderer::ResumeLocalSolver(std::shared_ptr<GDX12GrassFieldProducer> solver,
    uint64_t interactionResetRevision, GDX12GrassFieldInputQueue pending)
{
    if (pending.FirstMissing() || !solver || !solver->CanInstall(*_resources->Device, *_resources->SRV_UAV_Heap, _frames.size()))
        throw std::invalid_argument("Grass handoff solver does not match the local renderer");
    _fieldProducer = std::move(solver);
    _interactionResetRevision = interactionResetRevision;
    _resumeSeed = true;
    _restartLocal = false;
    _carryWindDebt = true;
    if (pending.Size()) _localContinuation.emplace(std::move(pending));
    else _localContinuation.reset();
}

void GDX12GrassRenderer::PrepareGPU(GDX12CommandList* commands)
{
    auto& frame = CurrentFrame();
    assert(!_shadowMaterials || frame.RootUploads.size() == _drawOrder.size());
    for (const auto& [roots, upload] : frame.RootUploads)
    {
        commands->ResourceBarrier({ roots->GetResource().GetCopyDestBarrier() });
        commands->GetCommandList()->CopyBufferRegion(roots->GetResource().D3DResource.Get(), 0,
            upload->GetResource().D3DResource.Get(), 0, UINT64(roots->GetElementCount()) * sizeof(XMFLOAT3));
        commands->ResourceBarrier({ roots->GetResource().GetNonPixelShaderResourceBarrier() });
    }
    if (frame.LocalSolver)
    {
        auto* packed = frame.Checkpoint ? frame.Checkpoint->Packed.get() : nullptr;
        if (packed) commands->ResourceBarrier({ packed->GetCopyDestBarrier() });
        frame.LocalSolver->Record(*commands, _resources->CurrFrameConstantsIndex, packed ? packed->D3DResource.Get() : nullptr);
        if (packed) commands->ResourceBarrier({ packed->GetTransitionBarrier(D3D12_RESOURCE_STATE_COMMON) });
    }
}

bool GDX12GrassRenderer::HasCasters() const
{
    for (const auto id : _drawOrder) if (_grass.at(id).Source.CastShadows) return true;
    return false;
}

GDX12GrassRenderer::Stage GDX12GrassRenderer::RenderStage(GDX12GeometryStage stage)
{
    switch (stage)
    {
    case GDX12GeometryStage::Depth: return Stage::Depth;
    case GDX12GeometryStage::DepthNormals: return Stage::PrepassNormals;
    case GDX12GeometryStage::GBuffer: return Stage::GBuffer;
    case GDX12GeometryStage::Forward: return Stage::Full;
    case GDX12GeometryStage::Shadow: return Stage::Shadow;
    case GDX12GeometryStage::ReflectionCapture: return Stage::Capture;
    default: throw std::invalid_argument("Grass has no transparent material batches");
    }
}

GDX12GrassRenderer::Output* GDX12GrassRenderer::FindOutput(const GDX12GeometryContext& view, UINT patch) const
{
    if (view.FrameSlot != _resources->CurrFrameConstantsIndex) throw std::invalid_argument("Grass view uses an inactive frame slot");
    const auto& index = _frames.at(view.FrameSlot).OutputIndex;
    const auto found = index.find({ view.Key, view.Stage, patch });
    return found == index.end() ? nullptr : found->second;
}

void GDX12GrassRenderer::PrepareView(const GDX12GeometryContext& view, UINT patch)
{
    auto& frame = CurrentFrame();
    frame.OutputIndex.erase({ view.Key, view.Stage, patch });
    if (view.Filter == GDX12GeometryFilter::Static || view.Stage == GDX12GeometryStage::Transparency || patch >= _drawOrder.size()) return;
    const auto stage = RenderStage(view.Stage);
    if (_shadowMaterials && stage != Stage::Shadow) throw std::invalid_argument("Native grass shadow slot has a non-shadow view");
    const auto id = _drawOrder.at(patch);
    const auto& state = _grass.at(id);
    const auto& s = state.Source;
    if ((stage == Stage::Shadow && !s.CastShadows) || (stage == Stage::Capture && !s.CaptureReflections)) return;
    if (stage == Stage::Capture && (!view.CaptureLighting || !view.CameraConstants || !view.CaptureLighting->LocalLights))
        throw std::invalid_argument("Grass capture requires lighting and camera");
    XMFLOAT4X4 clip;
    XMStoreFloat4x4(&clip, view.ViewProjection.Transpose());
    if (!LocalShadowFaceIntersects(clip, state.Bounds)) return;
    Parameters p = {};
    XMStoreFloat4x4(&p.World, s.World.Transpose());
    XMStoreFloat4x4(&p.PreviousWorld, state.PreviousWorld.Transpose());
    p.ViewProjection = clip;
    p.EyeMaxDistance = XMFLOAT4(view.Eye.x, view.Eye.y, view.Eye.z, s.MaxDistanceCm);
    p.PreviousEye = XMFLOAT4(_previousEye.x, _previousEye.y, _previousEye.z, 0);
    p.Shape = XMFLOAT4(s.HeightCm, s.WidthCm, s.HeightVariation, s.MaxBendCm);
    p.Lod = XMFLOAT4(s.LodDistanceCm, s.FarCardDistanceCm, s.BendSeconds, 0);
    p.Patch = XMFLOAT4(s.ExtentsCm.x, s.ExtentsCm.y, 0, 0);
    const auto& layout = frame.Snapshots.Current ? frame.Snapshots.Current->GetLayout() :
        _fieldProducer->CurrentSnapshot();
    p.InteractionArea = layout.InteractionArea;
    p.Padding0 = layout.InteractionGrid;
    p.Count = GrassBladeCount(s);
    p.Segments = s.Segments;
    p.Implementation = static_cast<UINT>(s.Implementation);
    p.Seed = s.Seed;
    p.PointSource = !s.RootsCm.empty();
    p.Material = s.MaterialIndex;
    p.Shadow = stage == Stage::Shadow;
    p.ResetHistory = state.ResetHistory;
    std::array<FieldView, 4> fieldViews;
    if (frame.Snapshots.Current)
    {
        const auto& current = *frame.Snapshots.Current;
        const auto& previous = p.ResetHistory ? current : *frame.Snapshots.Previous;
        fieldViews = { current.Trail(), previous.Trail(), current.Trail(), previous.Trail() };
        if (frame.SnapshotWindOwners.count(s.WindId))
        {
            const auto* wind = current.FindWind(s.WindId);
            p.WindArea = wind->Area;
            p.Patch.z = wind->TimeSeconds;
            p.Patch.w = previous.FindWind(s.WindId)->TimeSeconds;
            p.WindMode = 2;
            p.GridResolution = wind->Resolution;
            fieldViews[0] = current.Wind(s.WindId);
            fieldViews[1] = previous.Wind(s.WindId);
        }
    }
    else
    {
        auto neutral = ViewOf(*_fieldProducer->NeutralWind());
        fieldViews = { neutral, neutral, ViewOf(_fieldProducer->Trail()), ViewOf(_fieldProducer->PreviousTrail()) };
        if (const auto field = _fieldProducer->FindWind(s.WindId))
        {
            p.WindArea = field->Area;
            p.Patch.z = field->TimeSeconds;
            p.Patch.w = field->PreviousTimeSeconds;
            p.WindMode = 2;
            p.GridResolution = field->Resolution;
            fieldViews[0] = ViewOf(*field->Current);
            fieldViews[1] = ViewOf(*field->Previous);
        }
        if (_controls.UpdateHz >= 0 && !frame.LocalSolver)
        {
            fieldViews[1] = fieldViews[0];
            fieldViews[3] = fieldViews[2];
            p.Patch.w = p.Patch.z;
        }
    }
    const UINT block = frame.DrawCursor / ParametersPerBuffer;
    const UINT element = frame.DrawCursor++ % ParametersPerBuffer;
    if (block == frame.DrawParameters.size())
        frame.DrawParameters.push_back(std::make_unique<GDX12UploadBuffer<Parameters>>(_resources->Device, ParametersPerBuffer));
    auto& parameters = frame.DrawParameters[block];
    parameters->CopyData(element, p);

    auto output = std::make_unique<Output>();
    output->View = view;
    if (view.CaptureLighting)
    {
        output->Lighting = *view.CaptureLighting;
        output->View.CaptureLighting = &output->Lighting;
    }
    output->Patch = patch;
    output->Owner = id;
    output->Constants = parameters.get();
    output->ConstantIndex = element;
    output->Fields = fieldViews;
    auto& binding = output->Binding;
    const auto* constants = _resources->FrameConstants[_resources->CurrFrameConstantsIndex].get();
    binding.Root = _graphicsRoot.get();
    binding.Pipeline = _graphicsPSO.at(static_cast<size_t>(stage));
    binding.IndirectSignature = _signature.Get();
    binding.IndirectStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    binding.Stage = view.Stage;
    if (stage == Stage::Full || stage == Stage::GBuffer)
    {
        binding.ColorTargetCount = 5;
        binding.ColorFormats = { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16_FLOAT,
            DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT };
    }
    else if (stage == Stage::PrepassNormals || stage == Stage::Capture)
    {
        binding.ColorTargetCount = 1;
        binding.ColorFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
    binding.DepthFormat = DXGI_FORMAT_D32_FLOAT;
    binding.Heap = BindingHeap();
    binding.CBVs.push_back({ 0, parameters->GetElementAddress(element) });
    if (!_shadowMaterials)
    {
        if (view.CameraConstants) binding.CBVs.push_back({ 1, view.CameraConstants });
        binding.CBVs.push_back({ 2, constants->MainCB->GetElementAddress(0) });
    }
    binding.SRVs = { { 0, _vertices->GetSRV()->GPUHandle },
        { 1, _shadowMaterials ? _shadowMaterials->Table : constants->ShadowMaterials->GetSRV()->GPUHandle },
        { 8, _shadowMaterials ? _shadowMaterials->AlphaTable : BindingHeap()->GetGPUHandle(Texture2D_StartIndex) } };
    if (stage == Stage::Capture)
    {
        const auto& lighting = output->Lighting;
        binding.CBVs.push_back({ 3, lighting.ShadowConstants });
        binding.SRVs.insert(binding.SRVs.end(), {
            { 2, lighting.SunShadow->GetSRV()->GPUHandle }, { 3, lighting.Irradiance->GetSRV()->GPUHandle },
            { 4, lighting.Prefiltered->GetSRV()->GPUHandle }, { 5, lighting.BRDF->GetSRV()->GPUHandle },
            { 6, lighting.LocalLights->GetSRV()->GPUHandle }, { 7, lighting.LocalShadows->GetSRV()->GPUHandle } });
    }
    auto& batch = output->Batch;
    batch.Binding = &binding;
    batch.ExplicitGeometry = true;
    batch.IndexBuffer = { _indices->GetElementAddress(0), static_cast<UINT>(_indices->GetElementCount() * sizeof(UINT)), DXGI_FORMAT_R32_UINT };
    batch.InstanceCount = 1;
    batch.Draw = { GDX12DrawKind::Indirect, {}, {}, _signature.Get(), _arguments->GetResource().D3DResource.Get(),
        0, nullptr, 0, 1, sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) };
    batch.BufferAccesses = {
        { &_vertices->GetResource(), GDX12RenderGraph::Mode::Read },
        { &_indices->GetResource(), GDX12RenderGraph::Mode::Read },
        { &_arguments->GetResource(), GDX12RenderGraph::Mode::Read },
        { _shadowMaterials ? _shadowMaterials->Buffer : &constants->ShadowMaterials->GetResource(), GDX12RenderGraph::Mode::Read },
        { &parameters->GetResource(), GDX12RenderGraph::Mode::Read } };
    if (!_shadowMaterials)
        batch.BufferAccesses.push_back({ stage == Stage::Capture ? &output->Lighting.LocalLights->GetResource() :
            &constants->LightingLocalLights().GetResource(), GDX12RenderGraph::Mode::Read });
    frame.OutputIndex[{view.Key, view.Stage, patch}] = output.get();
    frame.Outputs.push_back(std::move(output));
}

bool GDX12GrassRenderer::Expand(GDX12CommandList* commands, const GDX12GeometryContext& view, UINT patch,
    bool packedFieldStateDeclared)
{
    if (packedFieldStateDeclared && !CurrentFrame().Snapshots.Current)
        throw std::logic_error("Declared grass field state requires packed snapshots");
    GDX12_SCOPED_EVENT(commands, "Bistro grass expansion");
    auto* output = FindOutput(view, patch);
    if (!output) return false;
    const auto& state = _grass.at(output->Owner);
    const auto& s = state.Source;
    commands->SetDescriptorHeaps({ BindingHeap() });
    commands->SetComputeRootSignature(_computeRoot.get());
    commands->SetComputeRootConstantBufferView(0, output->Constants->GetElementAddress(output->ConstantIndex));
    commands->ResourceBarrier({ _vertices->GetResource().GetUnorderedAccessBarrier(),
        _indices->GetResource().GetUnorderedAccessBarrier(), _arguments->GetResource().GetUnorderedAccessBarrier() });
    commands->SetComputeSRV(0, state.Roots[view.FrameSlot]->GetSRV()->GPUHandle);
    for (UINT i = 0; i < output->Fields.size(); ++i)
    {
        const auto& field = output->Fields[i];
        if (!packedFieldStateDeclared) commands->ResourceBarrier({ field.Resource->GetNonPixelShaderResourceBarrier() });
        commands->SetComputeSRV(i + 1, field.SRV);
    }
    commands->SetComputeUAV(0, _vertices->GetUAV()->GPUHandle);
    commands->SetComputeUAV(1, _indices->GetUAV()->GPUHandle);
    commands->SetComputeUAV(2, _arguments->GetUAV()->GPUHandle);
    commands->SetPipelineState(_clearPSO);
    commands->Dispatch(1, 1, 1);
    commands->ResourceBarrier({ _arguments->GetResource().GetUAVBarrier() });
    commands->SetPipelineState(_expandPSO);
    commands->Dispatch((GrassBladeCount(s) + 63) / 64, 1, 1);
    commands->ResourceBarrier({ _vertices->GetResource().GetSRVBarrier(),
        _indices->GetResource().GetGenericReadBarrier(), _arguments->GetResource().GetIndirectArgsBarrier() });
    _arenaOutput = output;
    return true;
}

void GDX12GrassRenderer::GatherBatches(const GDX12GeometryContext& view, std::vector<GDX12RenderBatch>& batches) const
{
    if (view.Filter == GDX12GeometryFilter::Static || view.Stage == GDX12GeometryStage::Transparency) return;
    const auto count = static_cast<UINT>(_drawOrder.size());
    const UINT end = view.BatchCount > count - (std::min)(view.BatchOffset, count) ? count : view.BatchOffset + view.BatchCount;
    for (UINT patch = view.BatchOffset; patch < end; ++patch)
    {
        const auto* output = FindOutput(view, patch);
        if (!output) continue;
        if (_arenaOutput != output) throw std::logic_error("Consume a prepared geometry range before reusing its direct-queue arena");
        batches.push_back(output->Batch);
    }
}

std::vector<GDX12RenderGraph::BufferAccess> GDX12GrassRenderer::PreparationAccesses() const
{
    using Mode = GDX12RenderGraph::Mode;
    std::vector<GDX12RenderGraph::BufferAccess> accesses;
    const auto& frame = _frames.at(_resources->CurrFrameConstantsIndex);
    for (const auto& [roots, upload] : frame.RootUploads)
    {
        accesses.push_back({ &roots->GetResource(), Mode::Write });
        accesses.push_back({ &upload->GetResource(), Mode::Read });
    }
    if (frame.LocalSolver) frame.LocalSolver->AppendAccesses(_resources->CurrFrameConstantsIndex, accesses);
    if (frame.Checkpoint) accesses.push_back({ frame.Checkpoint->Packed.get(), Mode::Write });
    return accesses;
}

std::vector<GDX12RenderGraph::BufferAccess> GDX12GrassRenderer::ExpansionAccesses(const GDX12GeometryContext& view, UINT patch) const
{
    using Mode = GDX12RenderGraph::Mode;
    const auto* output = FindOutput(view, patch);
    if (!output) return {};
    std::vector<GDX12RenderGraph::BufferAccess> accesses = {
        { &_grass.at(output->Owner).Roots[view.FrameSlot]->GetResource(), Mode::Read },
        { &output->Constants->GetResource(), Mode::Read },
        { &_vertices->GetResource(), Mode::Write }, { &_indices->GetResource(), Mode::Write }, { &_arguments->GetResource(), Mode::Write } };
    for (const auto& field : output->Fields)
        accesses.push_back({ field.Resource, Mode::Read });
    return accesses;
}

std::vector<GDX12RenderGraph::BufferAccess> GDX12GrassRenderer::DrawAccesses() const
{
    using Mode = GDX12RenderGraph::Mode;
    if (!_vertices) return {};
    const auto& constants = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    std::vector<GDX12RenderGraph::BufferAccess> accesses = {
        { &_vertices->GetResource(), Mode::Read }, { &_indices->GetResource(), Mode::Read }, { &_arguments->GetResource(), Mode::Read },
        { _shadowMaterials ? _shadowMaterials->Buffer : &constants->ShadowMaterials->GetResource(), Mode::Read } };
    if (!_shadowMaterials)
    {
        accesses.push_back({ &constants->LightingLocalLights().GetResource(), Mode::Read });
        accesses.push_back({ &constants->ShadowLights->GetResource(), Mode::Read });
    }
    const auto& frame = _frames.at(_resources->CurrFrameConstantsIndex);
    const UINT blocks = (frame.DrawCursor + ParametersPerBuffer - 1) / ParametersPerBuffer;
    for (UINT block = 0; block < blocks; ++block)
        accesses.push_back({ &frame.DrawParameters[block]->GetResource(), Mode::Read });
    return accesses;
}

void GDX12GrassRenderer::PublishPrimitives(GDX12RenderPrimitiveRegistry& registry, const IRenderGeometrySource* source)
{
    const auto& frame = CurrentFrame();
    const auto& snapshots = frame.Snapshots;
    const auto& layout = snapshots.Current ? snapshots.Current->GetLayout() :
        _fieldProducer->CurrentSnapshot();
    _primitiveRegistry = &registry;
    for (auto handle : _removedPrimitives) registry.Remove(handle);
    _removedPrimitives.clear();
    for (const auto id : _drawOrder)
    {
        auto& state = _grass.at(id);
        const auto& s = state.Source;
        GDX12RenderPrimitive primitive;
        primitive.Bounds = state.Bounds;
        primitive.Dynamic = true;
        primitive.CastsShadows = s.CastShadows;
        primitive.CapturedInReflections = s.CaptureReflections;
        primitive.MaterialIndex = s.MaterialIndex;
        primitive.GeometryRevision = state.SettingsHash;
        primitive.MaterialRevision = s.MaterialRevision;
        primitive.TransformRevision = 14695981039346656037ull;
        Hash(primitive.TransformRevision, s.World);
        const auto wind = std::lower_bound(layout.Wind.begin(), layout.Wind.end(), s.WindId,
            [](const auto& field, uint64_t owner) { return field.Owner < owner; });
        if (wind != layout.Wind.end() && wind->Owner == s.WindId &&
            (!snapshots.Current || frame.SnapshotWindOwners.count(s.WindId)))
        {
            primitive.DeformationRevision = wind->SettingsRevision;
            Hash(primitive.DeformationRevision, wind->TimeSeconds);
        }
        Hash(primitive.DeformationRevision, layout.InteractionRevision);
        Hash(primitive.DeformationRevision, layout.InteractionResetRevision);
        Hash(primitive.DeformationRevision, layout.InteractionArea);
        Hash(primitive.DeformationRevision, _usingSnapshot);
        primitive.Source = source;
        if (registry.Get(state.Primitive)) registry.Update(state.Primitive, primitive);
        else state.Primitive = registry.Add(primitive);
    }
}

std::optional<std::vector<GDX12GrassRenderer::ShadowPatch>> GDX12GrassRenderer::FreezeShadowPatches(
    const GDX12RenderPrimitiveRegistry& registry, const IRenderGeometrySource& source, bool shadowCastersOnly) const
{
    std::vector<ShadowPatch> patches;
    for (const auto id : _drawOrder)
    {
        const auto& state = _grass.at(id);
        if (shadowCastersOnly && !state.Source.CastShadows) continue;
        const auto* primitive = registry.Get(state.Primitive);
        if (!primitive || primitive->Source != &source || (shadowCastersOnly && !primitive->CastsShadows) ||
            primitive->MaterialIndex != state.Source.MaterialIndex || primitive->MaterialRevision != state.Source.MaterialRevision)
            return {};
        if (!primitive->Visible) continue;
        patches.push_back({state.Primitive, state.Source, primitive->Bounds});
    }
    return patches;
}

void GDX12GrassRenderer::RemovePrimitives(GDX12RenderPrimitiveRegistry& registry)
{
    for (const auto& [id, state] : _grass) registry.Remove(state.Primitive);
    for (auto handle : _removedPrimitives) registry.Remove(handle);
    _removedPrimitives.clear();
    _primitiveRegistry = nullptr;
}

uint64_t GDX12GrassRenderer::GeometryAllocationBytes() const
{
    uint64_t bytes = 0;
    const auto add = [&](const auto& buffer)
    {
        if (!buffer) return;
        const auto desc = buffer->GetResource().D3DResource->GetDesc();
        bytes += _resources->Device->GetDevice()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    };
    add(_vertices); add(_indices); add(_arguments);
    return bytes;
}

GDX12ParticleInteractionField GDX12GrassRenderer::InteractionField() const
{
    const auto slot = _resources->CurrFrameConstantsIndex;
    const auto& snapshot = _frames.at(slot).Snapshots.Current;
    if (!_fieldProducer && !snapshot) return {};
    const auto& layout = snapshot ? snapshot->GetLayout() : _fieldProducer->CurrentSnapshot();
    const auto view = snapshot ? snapshot->Presence() : ViewOf(_fieldProducer->Presence());
    return { view.Resource, view.SRV, layout.InteractionArea, layout.InteractionGrid, BistroArch::GrassPosition.y };
}
