#pragma once
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12LocalLights.h"
#include "Engine.RendererDX12/GDX12RenderGraph.h"
#include <limits>
#include <stdexcept>

class GDX12Texture;
class IRenderGeometrySource;

enum class GDX12GeometryStage : std::uint8_t
{ Depth, DepthNormals, GBuffer, Forward, Shadow, ReflectionCapture, Transparency };
enum class GDX12GeometryFilter : std::uint8_t { All, Static, Dynamic };
enum class GDX12GeometryDepthPartition : std::uint8_t { All, Far, Near };

struct GDX12CaptureLighting
{
    GDX12Texture* SunShadow = nullptr;
    GDX12Texture* Irradiance = nullptr;
    GDX12Texture* Prefiltered = nullptr;
    GDX12Texture* BRDF = nullptr;
    GDX12Texture* LocalShadows = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS ShadowConstants = 0;
};

struct GDX12GeometryContext
{
    GDX12GeometryStage Stage = GDX12GeometryStage::Forward;
    Matrix ViewProjection = Matrix::Identity;
    Vector3 Eye = Vector3::Zero;
    D3D12_GPU_VIRTUAL_ADDRESS CameraConstants = 0;
    const GDX12CaptureLighting* CaptureLighting = nullptr;
    GDX12GeometryFilter Filter = GDX12GeometryFilter::All;
    uint64_t Key = 0;
    UINT FrameSlot = 0;
    UINT ColorTargetCount = 0;
    std::array<DXGI_FORMAT, 8> ColorFormats = {};
    DXGI_FORMAT DepthFormat = DXGI_FORMAT_UNKNOWN;
    GDX12Texture* SceneDepth = nullptr;
    GDX12Texture* PartitionDepth = nullptr;
    GDX12GeometryDepthPartition DepthPartition = GDX12GeometryDepthPartition::All;
    bool IncludeApplicationGeometry = true;
    const IRenderGeometrySource* Source = nullptr;
    UINT BatchOffset = 0;
    UINT BatchCount = UINT_MAX;
    // Targets, viewport and scissor remain owned by the containing pass.
};

struct GDX12RenderPrimitiveHandle
{
    UINT Index = UINT_MAX;
    uint64_t Generation = 0;
    bool operator==(const GDX12RenderPrimitiveHandle& other) const
    { return Index == other.Index && Generation == other.Generation; }
};

struct GDX12RenderPrimitive
{
    BoundingBox Bounds;
    // This existing GPU transform entry contains both current and previous matrices.
    UINT TransformIndex = 0;
    UINT FirstInstance = 0;
    UINT InstanceCount = 1;
    UINT MaterialIndex = 0;
    uint64_t GeometryRevision = 0;
    uint64_t MaterialRevision = 0;
    uint64_t TransformRevision = 0;
    uint64_t DeformationRevision = 0;
    bool Visible = true;
    bool Dynamic = false;
    bool CastsShadows = true;
    bool CapturedInReflections = true;
    bool WritesMotion = true;
    // Null identifies the built-in shared static/skinned mesh stream adapter.
    const IRenderGeometrySource* Source = nullptr;

    bool Matches(GDX12GeometryFilter filter) const
    { return filter == GDX12GeometryFilter::All || Dynamic == (filter == GDX12GeometryFilter::Dynamic); }
    uint64_t Revision() const
    {
        uint64_t result = 14695981039346656037ull;
        const auto append = [&](const auto& value)
        {
            const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
            for (size_t i = 0; i < sizeof(value); ++i) result = (result ^ bytes[i]) * 1099511628211ull;
        };
        append(GeometryRevision); append(MaterialRevision); append(TransformRevision); append(DeformationRevision);
        append(FirstInstance); append(InstanceCount); append(MaterialIndex); append(Bounds.Center); append(Bounds.Extents);
        append(Source);
        append(Visible); append(Dynamic); append(CastsShadows); append(CapturedInReflections); append(WritesMotion);
        return result;
    }
};

class GDX12RenderPrimitiveRegistry
{
public:
    GDX12RenderPrimitiveHandle Add(const GDX12RenderPrimitive& primitive)
    {
        UINT index;
        if (_free.empty())
        {
            if (_slots.size() == UINT_MAX) throw std::overflow_error("Too many render primitives");
            index = static_cast<UINT>(_slots.size());
            _slots.push_back({});
        }
        else { index = _free.back(); _free.pop_back(); }
        auto& slot = _slots[index];
        slot.Primitive = primitive;
        slot.Alive = true;
        return { index, slot.Generation };
    }
    GDX12RenderPrimitive* Get(GDX12RenderPrimitiveHandle handle)
    {
        if (handle.Index >= _slots.size()) return nullptr;
        auto& slot = _slots[handle.Index];
        return slot.Alive && slot.Generation == handle.Generation ? &slot.Primitive : nullptr;
    }
    const GDX12RenderPrimitive* Get(GDX12RenderPrimitiveHandle handle) const
    { return const_cast<GDX12RenderPrimitiveRegistry*>(this)->Get(handle); }
    void Update(GDX12RenderPrimitiveHandle handle, const GDX12RenderPrimitive& primitive)
    {
        auto* current = Get(handle);
        if (!current) throw std::invalid_argument("Stale render primitive handle");
        *current = primitive;
    }
    void Remove(GDX12RenderPrimitiveHandle handle)
    {
        if (!Get(handle)) return;
        auto& slot = _slots[handle.Index];
        slot.Alive = false;
        // An exhausted generation is retired rather than resurrecting an old handle.
        if (slot.Generation == (std::numeric_limits<uint64_t>::max)()) return;
        ++slot.Generation;
        _free.push_back(handle.Index);
    }
    template<class Visitor> void ForEach(Visitor&& visitor) const
    {
        for (UINT i = 0; i < _slots.size(); ++i)
            if (_slots[i].Alive) visitor(GDX12RenderPrimitiveHandle{ i, _slots[i].Generation }, _slots[i].Primitive);
    }
private:
    struct Slot { GDX12RenderPrimitive Primitive; uint64_t Generation = 1; bool Alive = false; };
    std::vector<Slot> _slots;
    std::vector<UINT> _free;
};

enum class GDX12DrawKind : std::uint8_t { Direct, Indexed, Indirect };
struct GDX12DrawDescription
{
    GDX12DrawKind Kind = GDX12DrawKind::Indirect;
    D3D12_DRAW_ARGUMENTS Direct = {};
    D3D12_DRAW_INDEXED_ARGUMENTS Indexed = {};
    ID3D12CommandSignature* Signature = nullptr;
    ID3D12Resource* Arguments = nullptr;
    UINT64 ArgumentOffset = 0;
    ID3D12Resource* Count = nullptr;
    UINT64 CountOffset = 0;
    UINT MaxCommands = 0;
    UINT ArgumentStride = 0;

    bool Empty() const
    {
        if (Kind == GDX12DrawKind::Direct) return Direct.VertexCountPerInstance == 0 || Direct.InstanceCount == 0;
        if (Kind == GDX12DrawKind::Indexed) return Indexed.IndexCountPerInstance == 0 || Indexed.InstanceCount == 0;
        if (Kind == GDX12DrawKind::Indirect) return MaxCommands == 0;
        throw std::invalid_argument("Unknown render batch draw kind");
    }
    void Validate() const
    {
        if (Empty()) return;
        if (Kind == GDX12DrawKind::Indirect && (!Signature || !Arguments || ArgumentStride == 0 ||
            ArgumentStride % sizeof(UINT) || ArgumentOffset % sizeof(UINT) || CountOffset % sizeof(UINT)))
            throw std::invalid_argument("Invalid indirect render batch");
    }
};

// Native commands and recording checks share this dispatch; gathering/binding never issues a draw.
template<class Commands> void GDX12EmitDraw(Commands& commands, const GDX12DrawDescription& draw)
{
    if (draw.Empty()) return;
    draw.Validate();
    switch (draw.Kind)
    {
    case GDX12DrawKind::Direct:
        commands.DrawInstanced(draw.Direct.VertexCountPerInstance, draw.Direct.InstanceCount,
            draw.Direct.StartVertexLocation, draw.Direct.StartInstanceLocation); break;
    case GDX12DrawKind::Indexed:
        commands.DrawIndexedInstanced(draw.Indexed.IndexCountPerInstance, draw.Indexed.InstanceCount,
            draw.Indexed.StartIndexLocation, draw.Indexed.BaseVertexLocation, draw.Indexed.StartInstanceLocation); break;
    case GDX12DrawKind::Indirect:
        commands.ExecuteIndirect(draw.Signature, draw.MaxCommands, draw.Arguments, draw.ArgumentOffset,
            draw.Count, draw.CountOffset); break;
    default: throw std::invalid_argument("Unknown render batch draw kind");
    }
}

struct GDX12RenderBatch;
class IRenderBatchBinding
{
public:
    virtual ~IRenderBatchBinding() = default;
    virtual bool Supports(const GDX12GeometryContext&, const GDX12RenderBatch&) const = 0;
    // Graphics state/resources only: no targets, clears, dispatches or draws.
    virtual void Bind(GDX12CommandList*) const = 0;
};

struct GDX12RenderBatch
{
    // Source-owned bindings/resources remain alive until the last submission using this output retires.
    const IRenderBatchBinding* Binding = nullptr;
    GDX12GeometryBuffer* Geometry = nullptr;
    std::vector<D3D12_VERTEX_BUFFER_VIEW> VertexBuffers;
    D3D12_INDEX_BUFFER_VIEW IndexBuffer = {};
    bool ExplicitGeometry = false; // Includes vertex pulling with zero IA vertex buffers.
    D3D_PRIMITIVE_TOPOLOGY Topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    UINT FirstInstance = 0;
    UINT InstanceCount = 0;
    std::vector<GDX12RenderGraph::BufferAccess> BufferAccesses;
    GDX12DrawDescription Draw;
};

class GDX12GraphicsBindings final : public IRenderBatchBinding
{
public:
    GDX12RootSignature* Root = nullptr;
    ComPtr<ID3D12PipelineState> Pipeline;
    ID3D12CommandSignature* IndirectSignature = nullptr;
    UINT IndirectStride = 0;
    GDX12GeometryStage Stage = GDX12GeometryStage::Forward;
    UINT ColorTargetCount = 0;
    std::array<DXGI_FORMAT, 8> ColorFormats = {};
    DXGI_FORMAT DepthFormat = DXGI_FORMAT_UNKNOWN;
    GDX12DescriptorHeap* Heap = nullptr;
    std::vector<std::pair<UINT, D3D12_GPU_VIRTUAL_ADDRESS>> CBVs;
    std::vector<std::pair<UINT, D3D12_GPU_DESCRIPTOR_HANDLE>> SRVs;
    struct Constant { UINT Parameter, Value, Offset; };
    std::vector<Constant> Constants;

    bool Supports(const GDX12GeometryContext& view, const GDX12RenderBatch& batch) const override
    {
        if (!Root || !Pipeline || view.Stage != Stage || view.ColorTargetCount != ColorTargetCount ||
            view.ColorTargetCount > ColorFormats.size() || view.DepthFormat != DepthFormat ||
            (batch.Draw.Kind == GDX12DrawKind::Indirect &&
                (batch.Draw.Signature != IndirectSignature || batch.Draw.ArgumentStride != IndirectStride))) return false;
        for (UINT i = 0; i < view.ColorTargetCount; ++i)
            if (view.ColorFormats[i] != ColorFormats[i]) return false;
        return true;
    }
    void Bind(GDX12CommandList* commands) const override
    {
        commands->SetGraphicsRootSignature(Root);
        commands->SetPipelineState(Pipeline);
        if (Heap) commands->SetDescriptorHeaps({ Heap });
        for (const auto& [slot, address] : CBVs) commands->SetGraphicsRootConstantBufferView(slot, address);
        for (const auto& [slot, descriptor] : SRVs) commands->SetGraphicsSRV(slot, descriptor);
        for (const auto& value : Constants)
            commands->GetCommandList()->SetGraphicsRoot32BitConstant(value.Parameter, value.Value, value.Offset);
    }
};

inline void GDX12EmitRenderBatch(GDX12CommandList* commands, const GDX12GeometryContext& view, const GDX12RenderBatch& batch)
{
    if (batch.Draw.Empty()) return;
    batch.Draw.Validate();
    if (!batch.Binding || !batch.Binding->Supports(view, batch))
        throw std::invalid_argument("Render batch is incompatible with the active geometry view");
    batch.Binding->Bind(commands);
    commands->InvalidateGeometryBuffer();
    if (batch.ExplicitGeometry)
    {
        commands->GetCommandList()->IASetVertexBuffers(0, static_cast<UINT>(batch.VertexBuffers.size()), batch.VertexBuffers.data());
        if (batch.IndexBuffer.BufferLocation) commands->GetCommandList()->IASetIndexBuffer(&batch.IndexBuffer);
    }
    else if (batch.Geometry) commands->SetGeometryBuffer(batch.Geometry);
    commands->SetPrimitiveTopology(batch.Topology);
    GDX12EmitDraw(*commands, batch.Draw);
}
