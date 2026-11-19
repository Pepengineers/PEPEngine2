#include "Engine.RendererDX12/FrameGraph/FrameGraph.h"
#include "Engine.RendererDX12/FrameGraph/FrameGraphResources.h"
#include "Engine.RendererDX12/FrameGraph/ComputeCopyResources.h"
#include "FrameGraphDiagnostics.h"

#include <algorithm>
#include <set>
#include <variant>

namespace
{
    D3D12_RESOURCE_STATES UsageState(ResourceUsage usage)
    {
        if (HasUsage(usage, ResourceUsage::Present)) { return D3D12_RESOURCE_STATE_PRESENT; }
        if (HasUsage(usage, ResourceUsage::RenderTarget)) { return D3D12_RESOURCE_STATE_RENDER_TARGET; }
        if (HasUsage(usage, ResourceUsage::DepthWrite)) { return D3D12_RESOURCE_STATE_DEPTH_WRITE; }
        if (HasUsage(usage, ResourceUsage::UnorderedAccess)) { return D3D12_RESOURCE_STATE_UNORDERED_ACCESS; }
        if (HasUsage(usage, ResourceUsage::CopyDest)) { return D3D12_RESOURCE_STATE_COPY_DEST; }
        if (HasUsage(usage, ResourceUsage::CopySource)) { return D3D12_RESOURCE_STATE_COPY_SOURCE; }
        uint32_t state = 0;
        if (HasUsage(usage, ResourceUsage::DepthRead)) { state |= D3D12_RESOURCE_STATE_DEPTH_READ; }
        if (HasUsage(usage, ResourceUsage::PixelShaderRead)) { state |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; }
        if (HasUsage(usage, ResourceUsage::NonPixelShaderRead)) { state |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; }
        if (HasUsage(usage, ResourceUsage::IndirectArguments)) { state |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT; }
        if (HasUsage(usage, ResourceUsage::VertexIndex)) { state |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER; }
        if (HasUsage(usage, ResourceUsage::ConstantBuffer)) { state |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER; }
        return D3D12_RESOURCE_STATES(state);
    }
}

InputPinRef NodeHandle::GetInput(const PinId& id) const
{
    auto definition = _definition.lock();
    if (!definition) { throw std::logic_error("ERROR: Given FrameGraph handle refers to a destroyed graph. Obtain a new handle from a live graph."); }
    if (!definition->Nodes.at(_id).Description.Inputs.count(id))
    { throw std::invalid_argument("ERROR: " + _id + ": input '" + id + "' is not declared. Declare the input in Describe()."); }

    return InputPinRef(_definition, {_id, id});
}

OutputPinRef NodeHandle::GetOutput(const PinId& id) const
{
    auto definition = _definition.lock();
    if (!definition) { throw std::logic_error("ERROR: Given FrameGraph handle refers to a destroyed graph. Obtain a new handle from a live graph"); }
    if (!definition->Nodes.at(_id).Description.Outputs.count(id)) 
    { throw std::invalid_argument("ERROR: " + _id + ": output '" + id + "' is not declared. Declare the output in Describe()."); }

    return OutputPinRef(_definition, {_id, id});
}

void OutputPinRef::SetStorageType(MemoryType value) const
{
    auto definition = _definition.lock();
    if (!definition) { throw std::logic_error("ERROR: Given FrameGraph handle refers to a destroyed graph. Obtain a new handle from a live graph."); }
    if (definition->IsCompiled) { throw std::logic_error("ERROR: Unable to edit this value on a compiled graph."); }
    definition->Nodes.at(_address.Node).Description.Outputs.at(_address.Pin).RequestedMemory = value;
}

MemoryType OutputPinRef::GetStorageType() const
{
    auto definition = _definition.lock();
    if (!definition) { throw std::logic_error("ERROR: Given FrameGraph handle refers to a destroyed graph. Obtain a new handle from a live graph."); }
    return definition->Nodes.at(_address.Node).Description.Outputs.at(_address.Pin).RequestedMemory;
}

void InputPinRef::SetAccessType(MemoryType value) const
{
    auto definition = _definition.lock();
    if (!definition) { throw std::logic_error("ERROR: Given FrameGraph handle refers to a destroyed graph. Obtain a new handle from a live graph."); }
    if (definition->IsCompiled) { throw std::logic_error("ERROR: Unable to edit this value on a compiled graph"); }
    definition->Nodes.at(_address.Node).Description.Inputs.at(_address.Pin).RequestedMemory = value;
}

MemoryType InputPinRef::GetAccessType() const
{
    auto definition = _definition.lock();
    if (!definition) { throw std::logic_error("ERROR: Given FrameGraph handle refers to a destroyed graph. Obtain a new handle from a live graph."); }
    return definition->Nodes.at(_address.Node).Description.Inputs.at(_address.Pin).RequestedMemory;
}

FrameGraph::FrameGraph(GDX12Device* primary, GDX12Device* secondary)
    : _primary(primary), _secondary(secondary), _definition(std::make_shared<FrameGraphDefinition>())
{
    if (!primary) { throw std::invalid_argument("ERROR: Given FrameGraph has no primary device. Pass a valid primary GDX12Device pointer to the FrameGraph constructor."); }
    if (primary == secondary) { throw std::invalid_argument("ERROR: Given primary and secondary devices refer to the same GDX12Device. Pass a different device as "
        "secondary, or pass nullptr for a single-GPU graph."); }
    _compileData.FrameSlotCount = 0;
}

void FrameGraph::ConnectNodes(const OutputPinRef& output, const InputPinRef& input)
{
    if (_definition->IsCompiled) { throw std::logic_error("ERROR: Given FrameGraph has already been compiled."); }
    auto outputDefinition = output._definition.lock();
    auto inputDefinition = input._definition.lock();
    if (!outputDefinition || !inputDefinition) { throw std::logic_error("ERROR: Given FrameGraph handle refers to a destroyed graph."); }
    if (outputDefinition != _definition || inputDefinition != _definition)
    { throw std::invalid_argument("ERROR: Given input and output ports belong to different FrameGraphs."); }
    for (const auto& edge : _definition->Connections)
    { if (edge.Target == input._address) { throw std::invalid_argument("ERROR: " + input._address.Node + "." + input._address.Pin + ": input already has a connected source. Remove the duplicate ConnectNodes() call so this input has exactly one source."); } }

    _definition->Connections.push_back({output._address, input._address});
}

void FrameGraph::AddExecutionDependency(const NodeHandle& before, const NodeHandle& after)
{
    if (_definition->IsCompiled) { throw std::logic_error("ERROR: Given FrameGraph has already been compiled."); }
    auto beforeDefinition = before._definition.lock();
    auto afterDefinition = after._definition.lock();
    if (!beforeDefinition || !afterDefinition) { throw std::logic_error("ERROR: Given FrameGraph handle refers to a destroyed graph."); }
    if (beforeDefinition != _definition || afterDefinition != _definition) { throw std::invalid_argument("ERROR: Given execution dependency contains nodes from another FrameGraph."); }

    _definition->OrderDependencies.emplace_back(before._id, after._id);
}

OutputPinRef FrameGraph::ImportTexture(NodeId id, std::function<GDX12Texture*()> provider)
{
    if (_definition->IsCompiled) { throw std::logic_error("ERROR: Given FrameGraph has already been compiled."); }
    if (id.empty() || !provider || _definition->Nodes.count(id)) { throw std::invalid_argument("ERROR: Import ID '" + id + "' is empty, already registered, or has no provider."); }

    NodeDefinition node;
    node.Id = id;
    auto& port = node.Description.AddOutputTexture("Resource");
    port.RequestedMemory = MemoryType::Local;
    _definition->Nodes.emplace(id, std::move(node));
    _imports.emplace(id, std::move(provider));
    return OutputPinRef(_definition, {id, "Resource"});
}

void FrameGraph::SetDeviceResources(GDX12DeviceResources* primary, GDX12DeviceResources* secondary, RenderPipelineCommonData* common)
{
    if (_definition->IsCompiled) { throw std::logic_error("ERROR: Given FrameGraph has already been compiled."); }
    if (!primary || primary->Device != _primary || !common ||
        (UsesDevice(DEVICE_ROLE_SECONDARY) && (!secondary || secondary->Device != _secondary)))
    { throw std::invalid_argument("ERROR: Given device resources or frame data do not match the registered FrameGraph devices. Call "
        "SetDeviceResources() with matching primary and secondary resources and non-null frame data."); }

    _primaryResources = primary; _secondaryResources = secondary; _common = common;
}

void FrameGraph::SetTransportQueuePolicy(TransportQueuePolicy policy)
{
    if (_definition->IsCompiled) { throw std::logic_error("ERROR: Given FrameGraph has already been compiled."); }

    _compileData.QueuePolicy = policy;
}

void FrameGraph::SetFrameSlotResourcesCount(uint32_t count)
{
    if (_definition->IsCompiled) { throw std::logic_error("ERROR: Given FrameGraph has already been compiled."); }
    if (!count) { throw std::invalid_argument("ERROR: Given frame slot count is zero. Pass a frame slot count greater than zero."); }

    _compileData.FrameSlotCount = count;
}

NodeHandle FrameGraph::GetNode(const NodeId& id) const
{
    if (!_definition->Nodes.count(id)) { throw std::invalid_argument("ERROR: NodeId '" + id + "' is not registered in this FrameGraph."); }
    return NodeHandle(_definition, id);
}

bool FrameGraph::UsesDevice(EDeviceRole role) const
{
    for (const auto& node : _definition->Nodes)
    { if (node.second.DeviceRole == role) { return true; } }
    return false;
}

uint32_t FrameGraph::GetDevicePassFlags(EDeviceRole role) const
{
    uint32_t flags = 0;
    for (const auto& pass : _passes)
    {
        const auto& node = _definition->Nodes.at(pass.first);
        if (node.DeviceRole == role) { flags |= pass.second->GetFlags(); }
    }
    return flags;
}

// Keep compiled declarations, allocation decisions and execution bookkeeping together.
// CPU descriptions are retained for pass contexts, diagnostics and resource rebuilds.
struct CompiledFrameGraph::Runtime
{
    using Materialization = FrameGraphMaterialization;

    struct PreparedInput
    {
        Materialization* Source;
        const PinAddress* SourcePin;
        const ResourcePlan* Plan;
        MemoryType Access;
        FrameGraphPhysicalResource* Resource;
    };

    struct PreparedPort
    {
        FrameGraphPhysicalResource* Resource;
        D3D12_RESOURCE_STATES State;
        bool UAVBarrier;
        bool AliasOutput;
        bool ReleaseToCommon;
    };

    struct PreparedPass
    {
        const NodeDefinition* Node;
        GDX12DeviceResources* DeviceResources;
        GDX12RenderPass* Pass;
        GDX12GPUTimer* Timer;
        std::vector<const GDX12CompletionPoint*> Dependencies;
        std::vector<PreparedInput> Inputs;
        std::vector<Materialization*> Outputs;
        std::map<PinId, PassBinding> Bindings;
        std::vector<PreparedPort> Ports;
        GDX12CompletionPoint Completion;
    };

    struct PreparedImport
    {
        const std::function<GDX12Texture*()>* Provider;
        FrameGraphPhysicalResource* Resource;
        // Includes every alias binding that follows this rotating texture.
        std::vector<TextureBinding*> Bindings;
    };

    struct FrameSlotResources
    {
        std::map<PinAddress, Materialization> Resources;
        std::vector<std::variant<PreparedPass, PreparedImport>> Execution;
        std::vector<GDX12CompletionPoint> Completions;
        std::map<FrameGraphPhysicalResource*, GDX12CompletionPoint> LastUse;
        std::vector<ComPtr<ID3D12Resource>> ExternalResources;
        std::optional<uint64_t> SubmittedFrame;
        std::vector<FrameGraphDiagnostics::CopySample> CopySamples;
        std::vector<GDX12CommandQueue*> UsedQueues;
    };
    struct PassBatch
    {
        std::vector<GDX12CommandList*> Lists;
        std::vector<GDX12GPUTimer*> Timers;
    };
    GDX12DeviceResources* Primary = nullptr;
    GDX12DeviceResources* Secondary = nullptr;
    RenderPipelineCommonData* Common = nullptr;
    FrameGraphDefinition Definition;
    FrameGraphCompileData CompileData;
    FrameGraphPlan Plan;
    std::array<ComputeCopyResources, 2> ComputeCopiers;
    std::map<NodeId, std::unique_ptr<GDX12RenderPass>> Passes;
    std::array<std::vector<GDX12RenderPass*>, 2> PassesByDevice;
    std::array<bool, 2> ActiveDevices{};
    FrameGraphDiagnostics Diagnostics;
    std::map<NodeId, std::function<GDX12Texture*()>> Imports;
    std::map<PinAddress, PinAddress> Sources;
    std::vector<FrameSlotResources> FrameSlots;
    uint64_t Frame = 0;
    bool ResourceRebuildRequested = false;
    bool BatchLocalPasses = false;
    PassBatch LocalBatch;
    std::vector<std::string> Fallbacks;

    GDX12DeviceResources* GetDeviceResources(EDeviceRole role) const { return role == DEVICE_ROLE_PRIMARY ? Primary : Secondary; }
    FrameGraphSharedResources& GetSharedResources(Materialization& material) { return material.SharedAllocationAlias ? *material.SharedAllocationAlias : material.Shared; }
    uint32_t GetCurrentFrameSlotResourcesIndex() const { return uint32_t(Frame % CompileData.FrameSlotCount); }
    // Keep the latest retired frame's pass timestamps intact through submission and Present.
    uint32_t GetPassTimingSlotCount() const { return CompileData.FrameSlotCount + 1; }
    uint32_t GetPassTimingSlot(uint64_t frame) const { return uint32_t(frame % GetPassTimingSlotCount()); }

    void WaitForGPUCompletion()
    {
        Primary->Device->Flush();
        if (Secondary) { Secondary->Device->Flush(); }
    }

    void WaitForCurrentFrameSlotResources()
    {
        if (FrameSlots.empty()) { return; }
        const auto slot = GetCurrentFrameSlotResourcesIndex();
        auto& frameResources = FrameSlots[slot];
        for (const auto& point : frameResources.Completions) { GDX12CommandQueue::CPUwait(point); }
        if (frameResources.SubmittedFrame)
        {
            const auto frame = *frameResources.SubmittedFrame;
            Diagnostics.CompleteFrame(frame, slot, GetPassTimingSlot(frame), frameResources.CopySamples, frameResources.UsedQueues);
            frameResources.SubmittedFrame.reset();
            frameResources.CopySamples.clear();
            frameResources.UsedQueues.clear();
        }
        frameResources.Completions.clear();
        frameResources.ExternalResources.clear();
    }

    // Collects device capabilities, imported texture descriptions, and pass resource requirements.
    // Propagates texture size requirements through the graph to determine the render resolution.
    void ResolveResourceRequirements()
    {
        CompileData.Ports.clear();
        CompileData.ComputeCopyFormats = {};
        CompileData.RejectedComputeExports.clear();
        CompileData.RejectedComputeImports.clear();
        CompileData.RenderSize = CompileData.DisplaySize;
        for (const auto& node : Definition.Nodes)
        {
            auto* resources = GetDeviceResources(node.second.DeviceRole);
            for (const auto* ports : {&node.second.Description.Inputs, &node.second.Description.Outputs})
            {
                for (const auto& pin : *ports)
                { CompileData.Ports[{node.first, pin.first}].Capabilities = QueryFrameGraphCapabilities(resources->Device, pin.second); }
            }
        }
        for (const auto& imported : Imports)
        {
            auto* texture = imported.second();
            CompileData.Ports[{imported.first,"Resource"}].Imported = DescribeFrameGraphTexture(texture);
        }
        // Passes can query SDK resolutions or scene capacities before any graph allocation is made.
        for (auto& entry : Passes)
        {
            auto& node = Definition.Nodes.at(entry.first);
            PassResourceRequirementsContext context(GetDeviceResources(node.DeviceRole), Common, node, CompileData);
            entry.second->ResolveResourceRequirements(context);
        }
        if (Secondary && CompileData.QueuePolicy != TransportQueuePolicy::ForceCopy)
        {
            // Every concrete texture format originates at an output or an external import.
            std::set<DXGI_FORMAT> formats;
            for (const auto& [id, node] : Definition.Nodes)
            {
                for (const auto& [pin, output] : node.Description.Outputs)
                {
                    if (output.Type != ResourceType::Texture2D) { continue; }
                    const auto& imported = CompileData.Ports.at({id, pin}).Imported;
                    if (imported) { formats.insert(imported->Format); }
                    else { formats.insert(output.Formats.begin(), output.Formats.end()); }
                }
            }
            for (auto* owner : {Primary, Secondary})
            {
                for (auto format : formats)
                {
                    if (ComputeCopyResources::SupportsFormat(owner->Device, format))
                    { CompileData.ComputeCopyFormats[owner->Device->Role].insert(format); }
                }
            }
        }
        // SDK input requirements flow backwards through SameAs to the render size.
        std::optional<UInt2> renderSize;
        std::function<void(PinAddress, UInt2, std::set<PinAddress>)> require = [&](PinAddress address, UInt2 size, std::set<PinAddress> seen)
        {
            if (!seen.insert(address).second) { throw std::invalid_argument("ERROR: " + address.Node + ": extent propagation cycle. Remove cyclic Extent::SameAs() requirements and graph connections."); }
            const auto& port = Definition.GetPortDescription(address);
            const auto& required = CompileData.Ports.at(address).RequiredTextureSize;
            const auto extent = required ? Extent::Fixed(*required) : port.Size;
            if (Definition.Nodes.at(address.Node).Description.Inputs.count(address.Pin))
            {
                auto source = Sources.find(address);
                if (source == Sources.end()) { throw std::invalid_argument("ERROR: " + address.Node + "." + address.Pin + ": texture-size requirement refers to an unconnected input."); }
                require(source->second, size, std::move(seen));
            }
            else if (extent.Type == Extent::Source::Render)
            {
                if (renderSize && *renderSize != size) { throw std::invalid_argument("ERROR: SDK passes require conflicting render resolutions. Make all passes using Extent::Render() agree on one render resolution."); }
                renderSize = size;
            }
            else if (extent.Type == Extent::Source::Display && CompileData.DisplaySize != size)
            { throw std::invalid_argument("ERROR: SDK input resolution conflicts with a fixed display-size texture. Use Extent::Render() for SDK inputs that need a size different from the display resolution."); }
            else if (extent.Type == Extent::Source::Input) { require({address.Node, extent.Input}, size, std::move(seen)); }
        };
        for (const auto& port : CompileData.Ports)
        {
            const auto& size = port.second.RequiredTextureSize;
            if (Definition.Nodes.at(port.first.Node).Description.Inputs.count(port.first.Pin) && size)
            { require(port.first, *size, {}); }
        }
        if (renderSize) { CompileData.RenderSize = *renderSize; }
        Common->DownscaledWidth = CompileData.RenderSize.x;
        Common->DownscaledHeight = CompileData.RenderSize.y;
    }

    // Treat callbacks returning the same native allocation as one logical source.
    // Merged providers are assumed to keep representing the same texture after replacement.
    void MergeDuplicateTextureImports()
    {
        std::map<ID3D12Resource*,NodeId> identities;
        for (auto it = Imports.begin(); it != Imports.end();)
        {
            const auto& imported = *it;
            auto* texture = imported.second();
            if (!texture || !texture->GetResource()) { throw std::invalid_argument("ERROR: " + imported.first + ": imported texture or resource is missing. Keep the imported texture alive and make its provider return an "
                "initialized resource."); }
            auto resource = texture->GetResource()->D3DResource.Get();
            auto found = identities.emplace(resource,imported.first);
            if (found.second) { ++it; continue; }
            if (Definition.Nodes.at(imported.first).Description.Outputs.at("Resource").RequestedMemory == MemoryType::Shared)
            { throw std::invalid_argument("ERROR: " + imported.first + ": imported allocation has fixed Local storage and cannot satisfy Shared storage. Keep imported resources in "
                "MemoryType::Local and let FrameGraph create any required shared transport."); }

            Definition.Nodes.erase(imported.first);
            for (auto& connection : Definition.Connections)
            { if (connection.Source.Node == imported.first) { connection.Source.Node = found.first->second; } }

            auto& dependencies = Definition.OrderDependencies;
            dependencies.erase(std::remove_if(dependencies.begin(), dependencies.end(), [&](auto& dependency)
            {
                const bool redirected = dependency.first == imported.first || dependency.second == imported.first;
                if (dependency.first == imported.first) { dependency.first = found.first->second; }
                if (dependency.second == imported.first) { dependency.second = found.first->second; }
                return redirected && dependency.first == dependency.second;
            }), dependencies.end());
            it = Imports.erase(it);
        }
    }

    // Resolve names and physical bindings once per slot. Only fences and imported textures change each frame.
    void PrepareSlotExecution(FrameSlotResources& slot, const FrameGraphPlan& plan)
    {
        std::map<NodeId, const GDX12CompletionPoint*> completions;
        std::map<FrameGraphPhysicalResource*, PreparedImport*> imports;
        // Dependencies and import updates point into earlier steps.
        slot.Execution.reserve(plan.Order.size());
        for (const auto& id : plan.Order)
        {
            if (const auto provider = Imports.find(id); provider != Imports.end())
            {
                auto* resource = slot.Resources.at({id, "Resource"}).ProducerResource;
                auto& step = slot.Execution.emplace_back(PreparedImport{&provider->second, resource});
                imports.emplace(resource, &std::get<PreparedImport>(step));
                continue;
            }

            const auto& node = Definition.Nodes.at(id);
            auto* device = GetDeviceResources(node.DeviceRole);
            auto& step = slot.Execution.emplace_back(PreparedPass{&node, device, Passes.at(id).get(), Diagnostics.PassTimers.at(id).get()});
            auto& pass = std::get<PreparedPass>(step);
            for (const auto& dependency : plan.Dependencies.at(id))
            {
                const auto ready = completions.find(dependency);
                if (ready != completions.end()) { pass.Dependencies.push_back(ready->second); }
            }
            completions.emplace(id, &pass.Completion);

            auto bind = [&](const PinId& pin, const PortDescription& port, FrameGraphPhysicalResource& resource, bool sharedOutput)
            {
                auto& binding = pass.Bindings.emplace(pin, resource.CreatePassBinding(port.Usages)).first->second;
                if (resource.ImportedTexture) { imports.at(&resource)->Bindings.push_back(&binding.Texture); }
                pass.Ports.push_back({&resource, UsageState(port.Usages), HasUsage(port.Usages, ResourceUsage::UnorderedAccess),
                    !port.AliasInput.empty(), sharedOutput});
            };
            for (const auto& [pin, port] : node.Description.Inputs)
            {
                const PinAddress target{id, pin};
                const auto connection = Sources.find(target);
                if (connection == Sources.end()) { continue; }
                const auto& source = connection->second;
                const auto& resource = plan.Resources.at(source);
                auto& material = slot.Resources.at(source);
                const auto access = resource.Consumers.at(target);
                auto* physical = material.ProducerResource;
                if (access == MemoryType::Shared) { physical = &GetSharedResources(material).GetDeviceMapping(device->Device); }
                else if (resource.Imports[node.DeviceRole]) { physical = material.LocalCopies.at(node.DeviceRole).get(); }
                pass.Inputs.push_back({&material, &source, &resource, access, physical});
                bind(pin, port, *physical, false);
            }
            for (const auto& [pin, port] : node.Description.Outputs)
            {
                const PinAddress output{id, pin};
                auto& material = slot.Resources.at(output);
                const bool shared = plan.Resources.at(output).Storage == MemoryType::Shared;
                if (!port.AliasInput.empty())
                {
                    material.ProducerResource = pass.Bindings.at(port.AliasInput).Resource;
                    if (shared)
                    {
                        auto& source = slot.Resources.at(Sources.at({id, port.AliasInput}));
                        material.SharedAllocationAlias = &GetSharedResources(source);
                    }
                }
                pass.Outputs.push_back(&material);
                bind(pin, port, *material.ProducerResource, shared);
            }
        }
    }

    // Build a placement plan and allocate every frame slot from the resolved requirements.
    void BuildFrameSlotResources()
    {
        for (;;)
        {
            auto plan = CompileFrameGraph(Definition, CompileData);
            for (auto it = plan.Order.rbegin(); it != plan.Order.rend(); ++it)
            {
                for (auto& entry : plan.Resources)
                {
                    if (entry.first.Node != *it || entry.second.Alias.Node.empty()) { continue; }
                    auto& original = plan.Resources.at(Sources.at(entry.second.Alias));
                    const auto& alias = entry.second;
                    original.Description.Usages |= alias.Description.Usages;
                    if (alias.Storage == MemoryType::Shared)
                    {
                        for (auto role : {DEVICE_ROLE_PRIMARY, DEVICE_ROLE_SECONDARY})
                        { original.SharedTextureUsages[role] |= alias.SharedTextureUsages[role]; }
                    }
                    else
                    {
                        const auto role = Definition.Nodes.at(entry.first.Node).DeviceRole;
                        original.LocalTextureUsages[role] |= alias.LocalTextureUsages[role];
                    }
                }
            }
            std::vector<FrameSlotResources> newFrameSlots(CompileData.FrameSlotCount);
            bool retry = false;
            for (auto& frameResources : newFrameSlots)
            {
                for (const auto& entry : plan.Resources)
                {
                    const auto& resource = entry.second;
                    auto& material = frameResources.Resources[entry.first];
                    const bool alias = !resource.Alias.Node.empty();
                    auto* owner = GetDeviceResources(Definition.Nodes.at(entry.first.Node).DeviceRole);
                    auto* other = owner == Primary ? Secondary : Primary;
                    if (Imports.count(entry.first.Node))
                    { material.LocalResource = WrapFrameGraphImport(owner, Imports.at(entry.first.Node)(), resource.Description); material.ProducerResource = material.LocalResource.get(); }
                    else if (!alias && resource.Storage == MemoryType::Local)
                    {
                        auto description = resource.Description;
                        description.Usages |= resource.LocalTextureUsages[owner->Device->Role];
                        material.LocalResource = CreateFrameGraphNative(owner, description);
                        material.ProducerResource = material.LocalResource.get();
                    }
                    if ((!alias && resource.Storage == MemoryType::Shared) || resource.Export)
                    {
                        const HRESULT result = TryCreateFrameGraphShared(owner, other ? other : owner, resource, material.Shared);
                        if (FAILED(result))
                        {
                            if (result != E_INVALIDARG && result != DXGI_ERROR_UNSUPPORTED) { ThrowIfFailed(result); }
                            // Reconsider only Auto choices; fixed memory requirements remain constraints.
                            bool narrowed = false;
                            const auto policy = Definition.Nodes.at(entry.first.Node).Description.Outputs.at(entry.first.Pin).RequestedMemory;
                            if (resource.Storage == MemoryType::Shared && policy == MemoryType::Auto
                                && CompileData.Ports.at(entry.first).Capabilities.Shared)
                            { CompileData.Ports.at(entry.first).Capabilities.Shared = false; narrowed = true; }
                            if (!narrowed && resource.Export.Method == TransferMethod::ComputeCopy)
                            {
                                // Retry without the shared UAV flag. Shader import may still work.
                                narrowed = CompileData.RejectedComputeExports.insert(entry.first).second;
                            }
                            if (!narrowed && other && resource.Imports[other->Device->Role].Method == TransferMethod::ComputeCopy)
                            {
                                // Try a direct shared read before falling back to a footprint buffer.
                                narrowed = CompileData.RejectedComputeImports.insert(entry.first).second;
                            }
                            if (!narrowed)
                            {
                                for (const auto& [target, binding] : resource.Consumers)
                                {
                                    const auto access = Definition.GetPortDescription(target).RequestedMemory;
                                    if (binding == MemoryType::Shared && access == MemoryType::Auto
                                        && CompileData.Ports.at(target).Capabilities.Shared)
                                    { CompileData.Ports.at(target).Capabilities.Shared = false; narrowed = true; }
                                }
                            }
                            if (!narrowed) { ThrowIfFailed(result); }
                            Fallbacks.push_back(entry.first.Node + "." + entry.first.Pin + ": Auto excluded a shared candidate rejected by actual resource creation");
                            retry = true;
                            break;
                        }
                        if (resource.Storage == MemoryType::Shared) { material.ProducerResource = &material.Shared.GetDeviceMapping(owner->Device); }
                        if (resource.Export.Method == TransferMethod::ComputeCopy) { ComputeCopiers[owner->Device->Role].Prepare(owner->Device, resource.Description.Format); }
                    }
                    // Multiple inputs on one GPU share a single local replica of this logical output.
                    for (auto device : {DEVICE_ROLE_PRIMARY, DEVICE_ROLE_SECONDARY})
                    {
                        const auto& transfer = resource.Imports[device];
                        if (!transfer) { continue; }
                        auto replica = resource.Description;
                        const bool compute = transfer.Method == TransferMethod::ComputeCopy;
                        if (compute) { replica.Usages = resource.LocalTextureUsages[device]; }
                        else { replica.Usages |= resource.LocalTextureUsages[device]; }
                        try
                        {
                            material.LocalCopies.emplace(device, CreateFrameGraphNative(GetDeviceResources(device), replica));
                        }
                        catch (const DxException& error)
                        {
                            if (!compute || (error.ErrorCode != E_INVALIDARG && error.ErrorCode != DXGI_ERROR_UNSUPPORTED)) { throw; }
                            CompileData.RejectedComputeImports.insert(entry.first);
                            Fallbacks.push_back(entry.first.Node + "." + entry.first.Pin + ": local ComputeCopy UAV unavailable");
                            retry = true;
                            break;
                        }
                        if (compute) { ComputeCopiers[device].Prepare(GetDeviceResources(device)->Device, replica.Format); }
                    }
                    if (retry) { break; }
                }
                if (retry) { break; }
            }
            if (retry) { continue; }
            for (auto& slot : newFrameSlots) { PrepareSlotExecution(slot, plan); }
            FrameSlots = std::move(newFrameSlots);
            Diagnostics.LastCompletedFrame.reset();
            Plan = std::move(plan);
            // Explicit order dependencies can require a pass to finish even without a resource barrier.
            BatchLocalPasses = !Secondary && Definition.OrderDependencies.empty() &&
                std::all_of(Plan.Resources.begin(), Plan.Resources.end(), [](const auto& entry)
            {
                const auto& resource = entry.second;
                return resource.Storage == MemoryType::Local && !resource.Export &&
                    std::all_of(resource.Consumers.begin(), resource.Consumers.end(), [](const auto& consumer)
                    { return consumer.second == MemoryType::Local; });
            });
            if (BatchLocalPasses)
            {
                LocalBatch.Lists.reserve(Passes.size());
                LocalBatch.Timers.reserve(Passes.size());
            }
            return;
        }
    }

    GDX12CommandList* GetFrameCommandList(GDX12CommandQueue* queue, FrameSlotResources& frameResources)
    {
        auto* commands = queue->GetCommandList();
        if (std::find(frameResources.UsedQueues.begin(), frameResources.UsedQueues.end(), queue) == frameResources.UsedQueues.end())
        {
            auto [timer, inserted] = Diagnostics.QueueTimers.try_emplace(queue);
            if (inserted) { timer->second.Initialize(queue->GetCompletionPoint().Device, queue, CompileData.FrameSlotCount); }
            timer->second.RecordBegin(commands, GetCurrentFrameSlotResourcesIndex());
            frameResources.UsedQueues.push_back(queue);
        }
        return commands;
    }

    // Update the end timestamp in each existing submission. The last submission leaves the final timestamp.
    GDX12CompletionPoint SubmitCommandList(GDX12CommandQueue* queue, GDX12CommandList* commands, FrameSlotResources& frameResources)
    {
        Diagnostics.QueueTimers.at(queue).RecordEndAndResolve(commands, GetCurrentFrameSlotResourcesIndex());
        queue->ExecuteCommandList(commands);
        auto point = queue->GetCompletionPoint();
        frameResources.Completions.push_back(point);
        return point;
    }

    void SubmitLocalBatch(GDX12CommandQueue* queue, FrameSlotResources& frameResources)
    {
        if (LocalBatch.Lists.empty()) { return; }
        Diagnostics.QueueTimers.at(queue).RecordEndAndResolve(LocalBatch.Lists.back(), GetCurrentFrameSlotResourcesIndex());
        queue->ExecuteCommandLists(LocalBatch.Lists);
        const auto point = queue->GetCompletionPoint();
        frameResources.Completions.push_back(point);
        const auto timingSlot = GetPassTimingSlot(Frame);
        for (auto* timer : LocalBatch.Timers) { timer->OnCommandListExecuted(point.Value, timingSlot); }
        LocalBatch.Lists.clear();
        LocalBatch.Timers.clear();
    }

    // Copy between two representations on one device; shared mappings carry data across adapters.
    GDX12CompletionPoint TransferResourceData(FrameGraphPhysicalResource& source, FrameGraphPhysicalResource& target,
        const GDX12CompletionPoint& ready, ResourceTransfer transfer, FrameSlotResources& frameResources,
        const PinAddress& sourcePin, bool exporting)
    {
        const bool useCopy = transfer.Method == TransferMethod::CopyQueue;
        const bool compute = transfer.Method == TransferMethod::ComputeCopy;
        const auto scope = transfer.Scope;
        auto* device = source.DeviceResources->Device;
        auto* direct = device->GetCommandQueue();
        auto* queue = useCopy ? device->GetCopyCommandQueue() : direct;
        auto dependency = ready;
        direct->GPUWait(frameResources.LastUse[&source]);
        direct->GPUWait(frameResources.LastUse[&target]);
        // DIRECT releases the resources to COMMON before COPY takes ownership of the transfer.
        if (useCopy)
        {
            direct->GPUWait(ready);
            auto* release = GetFrameCommandList(direct, frameResources);
            TransitionFrameGraphResource(release, source, D3D12_RESOURCE_STATE_COMMON);
            TransitionFrameGraphResource(release, target, D3D12_RESOURCE_STATE_COMMON);
            dependency = SubmitCommandList(direct, release, frameResources);
        }
        // Queue waits order GPU work without waiting for completion on the CPU.
        queue->GPUWait(dependency);
        auto* commands = GetFrameCommandList(queue, frameResources);
        std::string event = exporting ? "Export " : "Import ";
        if (compute) { event += "ComputeCopy "; }
        commands->BeginPixEvent(event + sourcePin.Node + "." + sourcePin.Pin, Colors::Orange);
        if (!useCopy)
        {
            TransitionFrameGraphResource(commands, source, compute ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE);
            TransitionFrameGraphResource(commands, target, compute ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST);
        }
        // A busy timestamp slot must not delay transport.
        auto* timer = scope == CopyScope::Local ? nullptr : &Diagnostics.GetTransferTimer(queue, sourcePin, exporting, CompileData.FrameSlotCount);
        const bool timed = timer && timer->Start(commands, false);
        auto statistics = compute ? ComputeCopiers[device->Role].Dispatch(commands, source, target)
                                  : CopyFrameGraphResource(commands, source, target);
        if (timed) { timer->Stop(commands); }
        // COPY uses implicit promotion from COMMON; resources decay back after its submission completes.
        if (!useCopy)
        {
            TransitionFrameGraphResource(commands, source, D3D12_RESOURCE_STATE_COMMON);
            TransitionFrameGraphResource(commands, target, D3D12_RESOURCE_STATE_COMMON);
        }
        commands->EndPixEvent();
        auto completion = SubmitCommandList(queue, commands, frameResources);
        if (timer)
        {
            if (timed) { timer->OnCommandListExecuted(completion.Value); }
            Diagnostics.SubmittedCopyCommands += statistics.Commands;
            Diagnostics.SubmittedCopyBytes += statistics.Bytes;
            const uint64_t bytes = scope == CopyScope::CrossGPU ? statistics.Bytes : 0;
            frameResources.CopySamples.push_back({timer, device->Role, timed ? completion.Value : 0, exporting, bytes});
        }
        frameResources.LastUse[&source] = completion;
        frameResources.LastUse[&target] = completion;
        return completion;
    }

    // Execute each planned transfer once for this resource version and return its readiness fence.
    GDX12CompletionPoint ResolveInputResource(const PreparedInput& input, EDeviceRole role, FrameSlotResources& frameResources)
    {
        auto& material = *input.Source;
        const auto& resource = *input.Plan;
        const auto& replica = resource.Imports[role];
        if (input.Access == MemoryType::Local && !replica) { return material.ProducerReady; }
        auto& sharedSet = GetSharedResources(material);
        if (!material.SharedReady)
        {
            if (resource.Export)
            {
                auto& shared = sharedSet.GetDeviceMapping(material.ProducerResource->DeviceResources->Device);
                material.SharedReady = TransferResourceData(*material.ProducerResource, shared, material.ProducerReady,
                    resource.Export, frameResources, *input.SourcePin, true);
            }
            else { material.SharedReady = material.ProducerReady; }
        }
        if (input.Access == MemoryType::Shared) { return material.SharedReady; }
        auto& ready = material.LocalCopyReady[role];
        if (!ready)
        {
            auto& shared = sharedSet.GetDeviceMapping(GetDeviceResources(role)->Device);
            ready = TransferResourceData(shared, *input.Resource, material.SharedReady,
                replica, frameResources, *input.SourcePin, false);
        }
        return ready;
    }

    void RebuildFrameSlotResources()
    {
        // Resize may have drained the queues and released the slots before replacing the swap chain buffers.
        if (!FrameSlots.empty()) { WaitForGPUCompletion(); }
        ResolveResourceRequirements();
        BuildFrameSlotResources();
        for (auto& passEntry : Passes)
        {
            const auto& nodeDefinition = Definition.Nodes.at(passEntry.first);
            PassInitContext initContext(GetDeviceResources(nodeDefinition.DeviceRole), Common, nodeDefinition, Plan, Sources);
            passEntry.second->OnResourcesRecreated(initContext);
        }
        ResourceRebuildRequested = false;
    }

    // Keep idle slot allocations while discarding readiness from their previous frame.
    void ResetFrameSlotReadiness(FrameSlotResources& frameResources)
    {
        frameResources.LastUse.clear();
        for (auto& resourceEntry : frameResources.Resources)
        {
            resourceEntry.second.SharedReady = {};
            resourceEntry.second.ProducerReady = {};
            resourceEntry.second.LocalCopyReady = {};
        }
    }

    // Resolve the current external texture and retain its native allocation for this frame.
    void RefreshImportedTexture(const PreparedImport& imported, FrameSlotResources& frameResources)
    {
        auto* importedTexture = (*imported.Provider)();
        imported.Resource->ImportedTexture = importedTexture;
        for (auto* binding : imported.Bindings) { binding->Value = importedTexture; }
        frameResources.ExternalResources.push_back(importedTexture->GetResource()->D3DResource);
    }

    // Wait for external scene data, retain its allocation and prepare states outside graph ports.
    void PreparePassExternalResources(GDX12RenderPass& renderPass, PassExecutionContext& executionContext,
        FrameSlotResources& frameResources)
    {
        auto* commandList = executionContext.GetCommandList();
        auto* commandQueue = executionContext.GetDevice()->GetCommandQueue();
        constexpr auto externalReadStates = D3D12_RESOURCE_STATE_GENERIC_READ | D3D12_RESOURCE_STATE_DEPTH_READ;
        for (const auto& externalUse : renderPass.GetExternalResources(executionContext))
        {
            if (!externalUse.Resource || !externalUse.Resource->D3DResource) { continue; }
            if (BatchLocalPasses)
            {
                const bool waitOnAnotherQueue = externalUse.Ready.Value && externalUse.Ready.Queue != commandQueue;
                const bool isReadOnly = externalUse.State != D3D12_RESOURCE_STATE_COMMON &&
                    (externalUse.State & ~externalReadStates) == 0;
                // Keep earlier passes ahead of the wait or a write with no graph resource edge.
                if (waitOnAnotherQueue || !isReadOnly) { SubmitLocalBatch(commandQueue, frameResources); }
            }
            commandQueue->GPUWait(externalUse.Ready);
            frameResources.ExternalResources.push_back(externalUse.Resource->D3DResource);
            D3D12_HEAP_PROPERTIES heapProperties = {};
            D3D12_HEAP_FLAGS heapFlags = {};
            ThrowIfFailed(externalUse.Resource->D3DResource->GetHeapProperties(&heapProperties, &heapFlags));
            // Upload heaps remain in GENERIC_READ and cannot use ordinary state transitions.
            if (heapProperties.Type == D3D12_HEAP_TYPE_UPLOAD) { externalUse.Resource->SetCurrentState(D3D12_RESOURCE_STATE_GENERIC_READ); }
            else { commandList->ResourceBarrier({externalUse.Resource->GetTransitionBarrier(externalUse.State)}); }
        }
    }

    void ApplyPortState(GDX12CommandList* commands, const PreparedPort& port)
    {
        TransitionFrameGraphResource(commands, *port.Resource, port.State);
        if (port.UAVBarrier) { FrameGraphUAVBarrier(commands, *port.Resource); }
    }

    // Record in plan order, preserving separate command-list state for each pass.
    void RecordPass(PreparedPass& pass, FrameSlotResources& frameResources)
    {
        auto* deviceResources = pass.DeviceResources;
        deviceResources->CurrFrameConstantsIndex = GetCurrentFrameSlotResourcesIndex();
        auto* commandQueue = deviceResources->Device->GetCommandQueue();
        if (!BatchLocalPasses)
        {
            for (const auto* dependency : pass.Dependencies) { commandQueue->GPUWait(*dependency); }
            for (const auto& input : pass.Inputs)
            {
                commandQueue->GPUWait(ResolveInputResource(input, pass.Node->DeviceRole, frameResources));
                commandQueue->GPUWait(frameResources.LastUse[input.Resource]);
            }
        }
        auto* commandList = GetFrameCommandList(commandQueue, frameResources);
        auto* passTimer = pass.Timer;
        const auto timingSlot = GetPassTimingSlot(Frame);

        passTimer->RecordBegin(commandList, timingSlot);
        PassInitContext initContext(deviceResources, Common, *pass.Node, Plan, Sources);
        PassExecutionContext executionContext(initContext, commandList, Frame, GetCurrentFrameSlotResourcesIndex(), pass.Bindings);
        PreparePassExternalResources(*pass.Pass, executionContext, frameResources);
        for (const auto& port : pass.Ports)
        { if (!port.AliasOutput) { ApplyPortState(commandList, port); } }
        pass.Pass->Execute(executionContext);
        // Alias outputs take their final state (e.g. PRESENT) after the pass has used their inputs.
        for (const auto& port : pass.Ports) { ApplyPortState(commandList, port); }
        for (const auto& port : pass.Ports)
        { if (port.ReleaseToCommon) { TransitionFrameGraphResource(commandList, *port.Resource, D3D12_RESOURCE_STATE_COMMON); } }
        passTimer->RecordEndAndResolve(commandList, timingSlot);

        if (BatchLocalPasses)
        {
            LocalBatch.Lists.push_back(commandList);
            LocalBatch.Timers.push_back(passTimer);
            return;
        }

        pass.Completion = SubmitCommandList(commandQueue, commandList, frameResources);
        passTimer->OnCommandListExecuted(pass.Completion.Value, timingSlot);
        for (const auto& port : pass.Ports) { frameResources.LastUse[port.Resource] = pass.Completion; }
        for (auto* output : pass.Outputs) { output->ProducerReady = pass.Completion; }
    }

    // Submit one frame; presentation and a final GPU wait remain the caller's responsibility.
    void ExecuteFrame()
    {
        WaitForCurrentFrameSlotResources();
        if (ResourceRebuildRequested) { RebuildFrameSlotResources(); }
        auto& frameResources = FrameSlots[GetCurrentFrameSlotResourcesIndex()];
        ResetFrameSlotReadiness(frameResources);

        for (auto& step : frameResources.Execution)
        {
            if (auto* pass = std::get_if<PreparedPass>(&step)) { RecordPass(*pass, frameResources); }
            else { RefreshImportedTexture(std::get<PreparedImport>(step), frameResources); }
        }
        if (BatchLocalPasses) { SubmitLocalBatch(Primary->Device->GetCommandQueue(), frameResources); }
        frameResources.SubmittedFrame = Frame;
        ++Frame;
    }
};

// Resolve external metadata, create the runtime's allocation plan and initialize each pass.
// The builder freezes only after success; pass ownership is transferred before allocation starts.
std::unique_ptr<CompiledFrameGraph> FrameGraph::Compile()
{
    if (_definition->IsCompiled) { throw std::logic_error("ERROR: Given FrameGraph has already been compiled."); }

    auto runtime = std::make_unique<CompiledFrameGraph::Runtime>();
    runtime->Primary = _primaryResources;
    runtime->Secondary = UsesDevice(DEVICE_ROLE_SECONDARY) ? _secondaryResources : nullptr;
    runtime->Common = _common; runtime->Definition = *_definition;
    runtime->CompileData = _compileData;
    runtime->Imports = _imports;
    for (const auto& imported : runtime->Imports)
    {
        auto* texture = imported.second();
        if (!texture || (texture->GetDevice() != _primary && texture->GetDevice() != _secondary))
        { throw std::invalid_argument("ERROR: " + imported.first + ": imported target must belong to a registered device."); }

        const auto description = DescribeFrameGraphTexture(texture);
        auto& node = runtime->Definition.Nodes.at(imported.first);
        node.DeviceRole = texture->GetDevice() == _primary ? DEVICE_ROLE_PRIMARY : DEVICE_ROLE_SECONDARY;
        auto& port = node.Description.Outputs.at("Resource");
        port.Type = description.Type;
        port.Formats = {description.Format};
        port.Usages = description.Usages;
        if (node.DeviceRole == DEVICE_ROLE_SECONDARY) { runtime->Secondary = _secondaryResources; }
    }

    runtime->CompileData.DisplaySize = {_common->WindowWidth, _common->WindowHeight};
    if (!runtime->CompileData.FrameSlotCount) { runtime->CompileData.FrameSlotCount = static_cast<uint32_t>(_primaryResources->FrameConstants.size()); }

    // Runtime owns the pass instances from this point
    runtime->Passes = std::move(_passes);
    runtime->MergeDuplicateTextureImports();
    for (const auto& [id, node] : runtime->Definition.Nodes) { runtime->ActiveDevices[node.DeviceRole] = true; }
    for (const auto& edge : runtime->Definition.Connections) { runtime->Sources[edge.Target] = edge.Source; }
    runtime->ResolveResourceRequirements();
    for (const auto& [id, pass] : runtime->Passes)
    {
        const auto role = runtime->Definition.Nodes.at(id).DeviceRole;
        runtime->PassesByDevice[role].push_back(pass.get());
        auto* device = runtime->GetDeviceResources(role)->Device;
        auto timer = std::make_unique<GDX12GPUTimer>();
        timer->Initialize(device, device->GetCommandQueue(), runtime->GetPassTimingSlotCount());
        runtime->Diagnostics.PassTimers.emplace(id, std::move(timer));
    }
    runtime->BuildFrameSlotResources();

    for (auto& entry : runtime->Passes)
    {
        const auto& node = runtime->Definition.Nodes.at(entry.first);
        PassInitContext context(runtime->GetDeviceResources(node.DeviceRole), _common, node, runtime->Plan, runtime->Sources);
        entry.second->Initialize(context);
    }
    auto graph = std::unique_ptr<CompiledFrameGraph>(new CompiledFrameGraph(std::move(runtime)));
    _definition->IsCompiled = true;
    return graph;
}

CompiledFrameGraph::CompiledFrameGraph(std::unique_ptr<Runtime> runtime) : _runtime(std::move(runtime)) {}
CompiledFrameGraph::~CompiledFrameGraph()
{
    const HRESULT primaryResult = _runtime->Primary->Device->TryFlush();
    const HRESULT secondaryResult = _runtime->Secondary ? _runtime->Secondary->Device->TryFlush() : S_OK;
    if (FAILED(primaryResult) || FAILED(secondaryResult))
    { OutputDebugStringA("ERROR: GPU synchronization failed during FrameGraph destruction. Check the device removal reason.\n"); }
    _runtime->Passes.clear();
}

void CompiledFrameGraph::ExecuteFrame() { _runtime->ExecuteFrame(); }
void CompiledFrameGraph::RequestFrameSlotResourcesRebuild() { _runtime->ResourceRebuildRequested = true; }
void CompiledFrameGraph::WaitForCurrentFrameSlotResources() { _runtime->WaitForCurrentFrameSlotResources(); }
void CompiledFrameGraph::WaitForGPUCompletion() { _runtime->WaitForGPUCompletion(); }
void CompiledFrameGraph::ReleaseFrameSlotResourcesForResize()
{
    _runtime->WaitForGPUCompletion();
    _runtime->FrameSlots.clear();
    _runtime->Diagnostics.LastCompletedFrame.reset();
}
uint32_t CompiledFrameGraph::GetCurrentFrameSlotResourcesIndex() const { return _runtime->GetCurrentFrameSlotResourcesIndex(); }
const FrameGraphPlan& CompiledFrameGraph::GetExecutionPlan() const { return _runtime->Plan; }
FrameGraphPerFrameMetrics CompiledFrameGraph::GetLatestFrameMetrics() const
{
    return _runtime->Diagnostics.GetLatestFrameMetrics(_runtime->Plan, _runtime->Definition);
}

std::string CompiledFrameGraph::GetDebugReport() const
{
    const auto* allocations = _runtime->FrameSlots.empty() ? nullptr : &_runtime->FrameSlots.front().Resources;
    return _runtime->Diagnostics.GetDebugReport(_runtime->Plan, _runtime->Definition, _runtime->CompileData,
        _runtime->Secondary != nullptr, _runtime->Fallbacks, allocations);
}

GDX12RenderPass& CompiledFrameGraph::FindPass(const NodeId& id) const
{
    auto pass = _runtime->Passes.find(id);
    if (pass == _runtime->Passes.end()) { throw std::invalid_argument("ERROR: Node '" + id + "' does not name a render pass in this FrameGraph. Use the NodeId of a render pass added to this graph."); }
    return *pass->second;
}

bool CompiledFrameGraph::UsesDevice(EDeviceRole role) const
{
    return static_cast<size_t>(role) < _runtime->ActiveDevices.size() && _runtime->ActiveDevices[role];
}

const GDX12Device* CompiledFrameGraph::GetActiveDevice(EDeviceRole role) const
{
    return UsesDevice(role) ? _runtime->GetDeviceResources(role)->Device : nullptr;
}

uint32_t CompiledFrameGraph::GetDevicePassFlags(EDeviceRole role) const
{
    uint32_t flags = 0;
    if (!UsesDevice(role)) { return flags; }
    for (auto* pass : _runtime->PassesByDevice[role]) { flags |= pass->GetFlags(); }
    return flags;
}

void CompiledFrameGraph::ResizeFrameSlotResources(UInt2 display)
{
    if (!display.x || !display.y) { throw std::invalid_argument("ERROR: Given display width or height is zero. Call ResizeFrameSlotResources() with non-zero display width and height."); }
    _runtime->CompileData.DisplaySize = display;
    _runtime->RebuildFrameSlotResources();
}
