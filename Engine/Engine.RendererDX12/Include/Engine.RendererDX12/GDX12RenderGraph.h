#pragma once

#include <cstdint>
#include "Engine.RendererDX12/GDX12Submission.h"
#include "Engine.RendererDX12/D3DHelpers.h"
#include <functional>
#include <iosfwd>
#include <optional>
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
        GDX12Submission::TransferVersion Version{};
    };
    struct BufferAccess
    {
        GDX12Resource* Resource;
        Mode Usage;
        // Explicit constant-buffer reads can share a DEFAULT buffer across queues.
        D3D12_RESOURCE_STATES State = UnmanagedState;
    };
    // IDs index Batches in this compiled plan only; they are never fence values.
    enum class PlannedBatchId : UINT { Invalid = UINT_MAX };
    struct BatchDependency
    {
        PlannedBatchId Producer, Consumer;
    };
    struct PlannedBatch
    {
        PlannedBatchId Id;
        GDX12Submission::Placement Location;
        // Legacy primary is resolved by the executor, not a fabricated AdapterId.
        bool LegacyPrimary = false;
        std::vector<Pass> Passes;
        std::vector<Access> Accesses;
    };
    struct VersionInput
    {
        Resource View;
        GDX12Submission::TransferVersion Version;
        PlannedBatchId Consumer;
    };
    struct PlannedView
    {
        Resource View;
        GDX12Submission::ResourceIdentity Identity;
        bool Imported, CrossAdapter, LogicalToken;
        std::vector<Resource> Fallbacks;
    };
    struct CompiledPlan
    {
        std::vector<PlannedBatch> Batches;
        std::vector<PlannedView> Views;
        // Includes per-queue FIFO edges, including same-queue dependencies.
        std::vector<BatchDependency> Dependencies;
        // Exact external inputs: runtime must retain reservations and bind import readiness.
        std::vector<VersionInput> Inputs;
        bool RequiresNewExecutor = false;
    };
    struct QueueBinding
    {
        GDX12Device* Device;
        GDX12CommandQueue* Queue;
    };
    struct ViewBinding
    {
        Resource View;
        GDX12Submission::ResourceIdentity Identity;
        // An aliasing shared_ptr may retain the placed heap / transport slot owner.
        std::shared_ptr<GDX12Resource> Native;
    };
    struct ExactInputBinding
    {
        Resource View;
        GDX12Submission::TransferVersion Version;
        GDX12Submission::CompletionPoint Ready;
        std::shared_ptr<const void> Reservation;
        // A remote shared export needs both its actual issuer and its source view identity.
        GDX12CommandQueue* SharedProducer = nullptr;
        GDX12Submission::ResourceIdentity SharedSource{};
    };
    struct ExecutionBindings
    {
        std::vector<QueueBinding> Queues;
        std::vector<ViewBinding> Views;
        std::vector<ExactInputBinding> ExactInputs;
        // Immutable per-invocation descriptor, PSO and upload-slot owners.
        std::vector<std::shared_ptr<const void>> KeepAlive;
    };
    struct ExecutionReceipt
    {
        std::vector<GDX12Submission::CompletionPoint> Batches;
    };
    class PlanExecutor;
    class BoundResources
    {
    public:
        ID3D12Resource* Get(Resource view) const;
    private:
        friend class PlanExecutor;
        std::vector<std::pair<Resource, ID3D12Resource*>> _views;
    };
    // One externally serialized domain for the supplied queues and physical allocations.
    // Drain before resetting/destroying their queues or devices.
    class PlanExecutor
    {
    public:
        explicit PlanExecutor(size_t maxInFlight);
        ~PlanExecutor();
        PlanExecutor(const PlanExecutor&) = delete;
        PlanExecutor& operator=(const PlanExecutor&) = delete;
        // Empty receipt means bounded in-flight capacity was exhausted.
        std::optional<ExecutionReceipt> TrySubmit(const GDX12RenderGraph& graph, const ExecutionBindings& bindings);
        void CollectCompleted();
        void Drain();
        size_t PendingCount() const;
        bool IsSubmissionFaulted() const;
    private:
        struct State;
        std::unique_ptr<State> _state;
    };
    struct Statistics
    {
        UINT Passes = 0, CulledPasses = 0, TransientTextures = 0, PhysicalTextures = 0;
        UINT64 LogicalBytes = 0, PhysicalBytes = 0;
        UINT AsyncPasses = 0, AsyncJoins = 0;
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
    // Register each device-local view once; shared backing views use the same AllocationId.
    void RegisterResource(Resource resource, GDX12Submission::ResourceIdentity identity, bool crossAdapter = false);
    void ImportVersion(Resource resource, GDX12Submission::TransferVersion version);
    void Compile(bool reuseTextures = true);
    // Pure compiler path: no device flush, resource allocation, rebinding or GPU work.
    void CompilePlan();
    const CompiledPlan& GetCompiledPlan() const;
    struct LegacyBoundaryView
    {
        IRenderPassLink* Link;
        GDX12Texture* Texture;
        std::shared_ptr<GDX12Resource> Native;
    };
    struct LegacyExternalView
    {
        GDX12Submission::ResourceIdentity Identity;
        std::shared_ptr<GDX12Resource> Native;
        Mode Usage;
        D3D12_RESOURCE_STATES PlacedState;
        std::optional<D3D12_RESOURCE_STATES> LegacyAfter;
    };
    using ResolveLegacyExternalViews = std::function<std::vector<LegacyExternalView>()>;
    struct LegacyBoundary
    {
        GDX12RenderPass* Replaced;
        std::vector<LegacyBoundaryView> Inputs;
        std::vector<LegacyBoundaryView> Outputs;
        std::vector<LegacyExternalView> ExternalViews;
    };
    using SubmitLegacyReplacement = std::function<std::optional<GDX12Submission::CompletionPoint>(
        const LegacyBoundary&, GDX12Submission::CompletionPoint prefix)>;

    // Call after AddPass and before Compile. Reconfiguring a pass replaces its previous point.
    // An empty result from a configured resolver keeps that pass in the legacy range.
    void SetLegacySplit(GDX12RenderPass& replaced, std::vector<IRenderPassLink*> inputs,
        std::vector<IRenderPassLink*> outputs, ResolveLegacyExternalViews resolve = {});
    // A callback may return nullopt only when it has proven no placed work was submitted.
    GDX12CommandList* ExecuteLegacySplit(GDX12CommandList* commands, GDX12CommandQueue* compute,
        const SubmitLegacyReplacement& submit);
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
        bool Registered = false, CrossAdapter = false, LogicalToken = false;
        GDX12Submission::ResourceIdentity Identity{};
        GDX12Submission::TransferVersion ImportedVersion{};
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
        bool HasPlacement = false, CompleteAccesses = false;
        GDX12Submission::Placement Location{};
        bool LastAsync = false;
        UINT64 LastForkFence = 0, LastComputeFence = 0;
        std::string LastJoinBefore;
        std::vector<std::pair<uintptr_t, Mode>> LastBuffers;
        std::vector<Pass> ExplicitDependencies, Dependencies;
    };
    struct Slot
    {
        Resource Description;
        UINT Last;
        std::shared_ptr<GDX12TextureResource> ResourceObject;
    };
    struct LegacySplit
    {
        std::vector<Resource> Inputs, Outputs;
        ResolveLegacyExternalViews ResolveExternal;
    };
    void CompileInternal(bool reuseTextures, bool allocateTextures);
    void CheckLegacyExecution(GDX12CommandQueue* computeQueue) const;
    void ResetLegacyStatistics();
    GDX12CommandList* RecordLegacyRange(GDX12CommandList* commands, GDX12CommandQueue* computeQueue,
        UINT begin, UINT end, const std::shared_ptr<const void>& owners = {});
    void BuildPlan();
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
    bool _resourcesReady = false;
    bool _executingSplit = false;
    std::unordered_map<Pass, LegacySplit> _legacySplits;
    CompiledPlan _plan;
    std::unordered_map<Pass, std::unique_ptr<GDX12GPUTimer>> _computeTimers;
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
    void SetPlacement(GDX12Submission::Placement location);
    // Declaration: all physical texture/buffer accesses use registered graph views.
    // GetBufferAccesses() is a legacy unmanaged contract and cannot satisfy this declaration.
    void DeclareCompleteAccesses();
    void UseVersion(Graph::Resource resource, Graph::Mode mode, GDX12Submission::TransferVersion version,
        D3D12_RESOURCE_STATES state);
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
