#pragma once

#include "Engine.RendererDX12/FrameGraph/PassContext.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"

#include <functional>
#include <memory>
#include <type_traits>

class CompiledFrameGraph;

class InputPinRef;
class OutputPinRef;

class NodeHandle
{
public:
    InputPinRef GetInput(const PinId& id) const;
    OutputPinRef GetOutput(const PinId& id) const;
private:
    friend class FrameGraph;
    NodeHandle(std::weak_ptr<FrameGraphDefinition> definition, NodeId id) : _definition(std::move(definition)), _id(std::move(id)) {}
    std::weak_ptr<FrameGraphDefinition> _definition;
    NodeId _id;
};

class OutputPinRef
{
public:
    void SetStorageType(MemoryType storage) const;
    MemoryType GetStorageType() const;
private:
    friend class NodeHandle;
    friend class FrameGraph;
    OutputPinRef(std::weak_ptr<FrameGraphDefinition> definition, PinAddress address) : _definition(std::move(definition)), _address(std::move(address)) {}
    std::weak_ptr<FrameGraphDefinition> _definition;
    PinAddress _address;
};

class InputPinRef
{
public:
    void SetAccessType(MemoryType access) const;
    MemoryType GetAccessType() const;
private:
    friend class NodeHandle;
    friend class FrameGraph;
    InputPinRef(std::weak_ptr<FrameGraphDefinition> definition, PinAddress address) : _definition(std::move(definition)), _address(std::move(address)) {}
    std::weak_ptr<FrameGraphDefinition> _definition;
    PinAddress _address;
};

class FrameGraph
{
public:
    FrameGraph(GDX12Device* primary, GDX12Device* secondary = nullptr);
    ~FrameGraph() = default;
    FrameGraph(const FrameGraph&) = delete;
    FrameGraph& operator=(const FrameGraph&) = delete;
    template<class T, class... Args> NodeHandle AddPrimaryPass(NodeId id, Args&&... args)
    { return AddPass<T>(std::move(id), DEVICE_ROLE_PRIMARY, std::forward<Args>(args)...); }
    template<class T, class... Args> NodeHandle AddSecondaryPass(NodeId id, Args&&... args)
    {
        if (!_secondary) { throw std::invalid_argument("ERROR: Node '" + id + "' is assigned to the secondary device, but that device is not registered. Pass a secondary GDX12Device to the "
            "FrameGraph constructor or use AddPrimaryPass()."); }
        return AddPass<T>(std::move(id), DEVICE_ROLE_SECONDARY, std::forward<Args>(args)...);
    }
    void ConnectNodes(const OutputPinRef& output, const InputPinRef& input);
    // Require `before` to execute ahead of `after` without adding a resource connection.
    void AddExecutionDependency(const NodeHandle& before, const NodeHandle& after);
    // Allows you to import external textures to FrameGraph via function
    // Works as an output pin coming from nowhere
    OutputPinRef ImportTexture(NodeId id, std::function<GDX12Texture*()> provider);
    void SetDeviceResources(GDX12DeviceResources* primary, GDX12DeviceResources* secondary, RenderPipelineCommonData* common);
    void SetTransportQueuePolicy(TransportQueuePolicy policy);
    void SetFrameSlotResourcesCount(uint32_t count);
    // Return a handle for an existing node identifier, including imported texture nodes.
    NodeHandle GetNode(const NodeId& id) const;
    // Return handles of passes matching T
    template<class T> std::vector<NodeHandle> GetPassNodes() const
    {
        std::vector<NodeHandle> nodes;
        for (const auto& pass : _passes)
        {
            if (dynamic_cast<const T*>(pass.second.get()))
            { nodes.push_back(NodeHandle(_definition, pass.first)); }
        }
        return nodes;
    }
    bool UsesDevice(EDeviceRole role) const;
    uint32_t GetDevicePassFlags(EDeviceRole role) const;
    // Returns FrameGraph's CPU declarations
    const FrameGraphDefinition& GetDefinition() const { return *_definition; }
    // Resolve requirements, allocate resources and initialize passes.
    std::unique_ptr<CompiledFrameGraph> Compile();
private:
    template<class T, class... Args> NodeHandle AddPass(NodeId id, EDeviceRole role, Args&&... args)
    {
        static_assert(std::is_base_of_v<GDX12RenderPass, T>, "FrameGraph passes must implement GDX12RenderPass");
        if (_definition->IsCompiled) { throw std::logic_error("ERROR: Given FrameGraph has already been compiled. Create and configure a new FrameGraph before calling "
            "Compile()."); }
        if (id.empty() || _definition->Nodes.count(id)) { throw std::invalid_argument("ERROR: NodeId '" + id + "' is empty or already registered. Use a non-empty, unique NodeId."); }
        auto pass = std::make_unique<T>(std::forward<Args>(args)...);
        NodeDefinition node;
        node.Id = id;
        node.DeviceRole = role;
        pass->Describe(node.Description);
        node.Description.ValidatePorts(id);
        _definition->Nodes.emplace(id, std::move(node));
        _passes.emplace(id, std::move(pass));
        return NodeHandle(_definition, std::move(id));
    }
    GDX12Device* _primary;
    GDX12Device* _secondary;
    GDX12DeviceResources* _primaryResources = nullptr;
    GDX12DeviceResources* _secondaryResources = nullptr;
    RenderPipelineCommonData* _common = nullptr;
    std::shared_ptr<FrameGraphDefinition> _definition;
    std::map<NodeId, std::unique_ptr<GDX12RenderPass>> _passes;
    std::map<NodeId, std::function<GDX12Texture*()>> _imports;
    FrameGraphCompileData _compileData;
};

// Owns executable passes, the resolved plan and per-slot allocations after successful graph compilation.
class CompiledFrameGraph
{
public:
    ~CompiledFrameGraph();
    CompiledFrameGraph(const CompiledFrameGraph&) = delete;
    CompiledFrameGraph& operator=(const CompiledFrameGraph&) = delete;
    // Wait for the selected slot, process pending rebuilds and submit the next frame's commands.
    void ExecuteFrame();
    // Request resource recreation at the next ExecuteFrame().
    void RequestFrameSlotResourcesRebuild();
    void WaitForCurrentFrameSlotResources();
    // Block the CPU until submitted DIRECT and COPY work on the graph's devices has completed.
    void WaitForGPUCompletion();
    // Wait for GPU work and drop slot references before resizing imported swap-chain resources.
    void ReleaseFrameSlotResourcesForResize();
    void ResizeFrameSlotResources(UInt2 display);
    const FrameGraphPlan& GetExecutionPlan() const;
    // Describe placement, requested policies, fallbacks, GPU timings and cumulative copy statistics.
    std::string GetDebugReport() const;
    uint32_t GetDevicePassFlags(EDeviceRole role) const;
    uint32_t GetCurrentFrameSlotResourcesIndex() const;
    bool UsesDevice(EDeviceRole role) const;
    // Access the owned pass by identifier and C++ type; throws for imports or a mismatched type.
    template<class T> T& GetPass(const NodeId& id)
    {
        auto* pass = dynamic_cast<T*>(&FindPass(id));
        if (!pass) { throw std::invalid_argument("ERROR: Pass '" + id + "' does not match the requested C++ type. Use GetPass<T>() with the C++ type used when adding this pass."); }
        return *pass;
    }
private:
    friend class FrameGraph;
    GDX12RenderPass& FindPass(const NodeId& id) const;
    struct Runtime;
    explicit CompiledFrameGraph(std::unique_ptr<Runtime> runtime);
    std::unique_ptr<Runtime> _runtime;
};
