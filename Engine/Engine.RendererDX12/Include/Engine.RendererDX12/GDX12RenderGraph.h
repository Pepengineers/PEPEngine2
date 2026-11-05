#pragma once

#include <cstdint>
#include "Engine.RendererDX12/D3DHelpers.h"
#include <iosfwd>
#include <unordered_map>

class GDX12CommandList;
class GDX12CommandQueue;
class GDX12GPUTimer;
class GDX12Resource;
class GDX12Device;
class GDX12Texture;
class GDX12TextureResource;
class GDX12RenderPass;
class GDX12RenderGraphBuilder;
class IRenderPassLink;

class GDX12RenderGraph
{
public:
    using Resource = UINT;
    using Pass = UINT;
    enum class Mode : std::uint8_t { Read, Write, ReadWrite };
    enum class Lifetime : std::uint8_t { Transient, Persistent, Dynamic };
    enum class SharedResource : std::uint8_t { Scene, Visibility, Environment, ReflectionProbes, ParticleSimulation, Particles };
    static constexpr auto UnmanagedState = static_cast<D3D12_RESOURCE_STATES>(UINT_MAX);
    struct Access
    {
        Resource Id;
        Mode Usage;
        // Omit for nodes that perform multiple internal transitions (SDKs, simulation, captures).
        D3D12_RESOURCE_STATES State = static_cast<D3D12_RESOURCE_STATES>(UINT_MAX);
    };
    struct BufferAccess
    {
        GDX12Resource* Resource;
        Mode Usage;
    };
    struct Statistics
    {
        UINT Passes = 0, CulledPasses = 0, TransientTextures = 0, PhysicalTextures = 0;
        UINT64 LogicalBytes = 0, PhysicalBytes = 0;
        UINT AsyncPasses = 0;
        mutable double ComputeTimeMS = -1;
        UINT64 ForkFence = 0, ComputeFence = 0;
        std::string JoinBefore;
    };

    explicit GDX12RenderGraph(GDX12Device* device = nullptr);
    ~GDX12RenderGraph();
    Resource Import(std::string name, GDX12Texture* texture = nullptr);
    Resource Create(std::string name, GDX12Texture* texture = nullptr, bool transient = false);
    // A conditional output may forward an input instead of writing its own target.
    void AddFallback(Resource output, Resource input);
    // Pass instances must outlive the graph. Recording happens once, execution every frame.
    Pass AddPass(GDX12RenderPass& pass);
    void ImportLink(std::string name, IRenderPassLink* link);
    void ImportShared(SharedResource resource, std::string name);
    void DependsOn(Pass pass, Pass prerequisite);
    void Export(Resource resource);
    void Compile(bool reuseTextures = true);
    // Returns the final DIRECT list; the caller submits it and retires the frame with its fence.
    GDX12CommandList* Execute(GDX12CommandList* commands, GDX12CommandQueue* computeQueue = nullptr);
    void WriteDot(std::ostream& output) const;
    const Statistics& GetStatistics() const;
    const std::vector<Pass>& GetExecutionOrder() const { return _order; }

private:
    friend class GDX12RenderGraphBuilder;
    struct ResourceInfo
    {
        std::string Name;
        GDX12Texture* Texture = nullptr;
        IRenderPassLink* Link = nullptr;
        bool Imported = false, Transient = false, Exported = false;
        std::vector<Resource> Fallbacks;
        UINT First = UINT_MAX, Last = 0, Slot = UINT_MAX;
    };
    struct Node
    {
        std::string Name;
        std::vector<Access> Accesses;
        GDX12RenderPass* Instance = nullptr;
        bool SideEffect = false;
        bool AsyncCompute = false;
        bool LastAsync = false;
        std::vector<std::pair<uintptr_t, Mode>> LastBuffers;
        std::vector<Pass> ExplicitDependencies, Dependencies;
    };
    struct Slot
    {
        Resource Description;
        UINT Last;
        std::shared_ptr<GDX12TextureResource> ResourceObject;
    };
    void CheckResource(Resource resource) const;
    void CheckPass(Pass pass) const;
    void ExpandRead(Resource resource, std::vector<Resource>& result) const;
    GDX12Device* _device;
    std::vector<ResourceInfo> _resources;
    std::vector<Node> _nodes;
    std::vector<Pass> _order;
    std::vector<Slot> _slots;
    std::unordered_map<IRenderPassLink*, Resource> _links;
    std::unordered_map<SharedResource, Resource> _shared;
    Statistics _statistics;
    bool _compiled = false;
    std::unique_ptr<GDX12GPUTimer> _computeTimer;
};

class GDX12RenderGraphBuilder
{
public:
    using Graph = GDX12RenderGraph;
    using Lifetime = Graph::Lifetime;
    using SharedResource = Graph::SharedResource;
    void SetName(std::string name);
    void HasSideEffects();
    void AsyncCompute();
    void Use(Graph::Resource resource, Graph::Mode mode, D3D12_RESOURCE_STATES state = Graph::UnmanagedState);
    void Read(IRenderPassLink* link, D3D12_RESOURCE_STATES state = Graph::UnmanagedState);
    void ReadInputs(const std::vector<IRenderPassLink*>& inputs, D3D12_RESOURCE_STATES state = Graph::UnmanagedState);
    void ReadWrite(IRenderPassLink* link, D3D12_RESOURCE_STATES state = Graph::UnmanagedState);
    void Write(IRenderPassLink* link, std::string name, Lifetime lifetime = Lifetime::Transient,
        D3D12_RESOURCE_STATES state = Graph::UnmanagedState, GDX12Texture* storage = nullptr);
    void Read(SharedResource resource);
    void Write(SharedResource resource, std::string name);
    void Fallback(IRenderPassLink* output, IRenderPassLink* input);
    void Export(IRenderPassLink* link);
private:
    friend class GDX12RenderGraph;
    GDX12RenderGraphBuilder(Graph& graph, Graph::Node& node) : _graph(graph), _node(node) {}
    Graph::Resource Find(IRenderPassLink* link) const;
    Graph& _graph;
    Graph::Node& _node;
};
