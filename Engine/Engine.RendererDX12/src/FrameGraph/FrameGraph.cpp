#include "Engine.RendererDX12/FrameGraph/FrameGraph.h"
#include "Engine.RendererDX12/FrameGraph/FrameGraphResources.h"
#include "Engine.RendererDX12/GDX12GPUTimer.h"

#include <algorithm>
#include <set>
#include <sstream>

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
void FrameGraph::SetFrameSlotCount(uint32_t count)
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

struct CompiledFrameGraph::Runtime
{
    struct Materialization
    {
        std::unique_ptr<FrameGraphPhysicalResource> LocalResource;
        FrameGraphPhysicalResource* ProducerResource = nullptr;
        FrameGraphSharedResources Shared;
        FrameGraphSharedResources* SharedAllocationAlias = nullptr;
        std::map<EDeviceRole, std::unique_ptr<FrameGraphPhysicalResource>> LocalCopies;
        GDX12CompletionPoint ProducerReady;
        GDX12CompletionPoint SharedReady;
        std::map<EDeviceRole, GDX12CompletionPoint> LocalCopyReady;
    };
    struct Slot
    {
        std::map<PinAddress, Materialization> Resources;
        std::vector<GDX12CompletionPoint> Completions;
        std::map<FrameGraphPhysicalResource*, GDX12CompletionPoint> LastUse;
        std::vector<ComPtr<ID3D12Resource>> ExternalResources;
    };
    GDX12DeviceResources* Primary = nullptr;
    GDX12DeviceResources* Secondary = nullptr;
    RenderPipelineCommonData* Common = nullptr;
    FrameGraphDefinition Definition;
    FrameGraphCompileData CompileData;
    FrameGraphPlan Plan;
    std::map<NodeId, std::unique_ptr<GDX12RenderPass>> Passes;
    std::map<NodeId, std::unique_ptr<GDX12GPUTimer>> Timers;
    std::map<NodeId, std::function<GDX12Texture*()>> Imports;
    std::map<PinAddress, PinAddress> Sources;
    std::vector<Slot> Slots;
    uint64_t Frame = 0;
    uint64_t Generation = 0;
    uint64_t CopyBytes = 0;
    uint64_t CopyCommands = 0;
    bool ResourceRebuildRequested = false;
    std::vector<std::string> Fallbacks;

    GDX12DeviceResources* GetDeviceResources(EDeviceRole role) const { return role == DEVICE_ROLE_PRIMARY ? Primary : Secondary; }
    FrameGraphSharedResources& GetSharedResources(Materialization& material) { return material.SharedAllocationAlias ? *material.SharedAllocationAlias : material.Shared; }
    uint32_t GetCurrentFrameSlotIndex() const { return uint32_t(Frame % CompileData.FrameSlotCount); }
    void WaitForGPUCompletion()
    {
        Primary->Device->Flush();
        if (Secondary) { Secondary->Device->Flush(); }
    }
    void WaitForCurrentFrameSlot()
    {
        if (Slots.empty()) { return; }
        auto& slot = Slots[GetCurrentFrameSlotIndex()];
        for (const auto& point : slot.Completions) { GDX12CommandQueue::CPUwait(point); }
        slot.Completions.clear();
        slot.ExternalResources.clear();
    }

    // Collects device capabilities, imported texture descriptions, and pass resource requirements.
    // Propagates texture size requirements through the graph to determine the render resolution.
    void ResolveResourceRequirements()
    {
        CompileData.Ports.clear();
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
        for (auto& entry : Passes)
        {
            auto& node = Definition.Nodes.at(entry.first);
            PassResourceRequirementsContext context(GetDeviceResources(node.DeviceRole), Common, node, CompileData);
            entry.second->ResolveResourceRequirements(context);
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

    void BuildResources()
    {
        for (;;)
        {
            auto plan = CompileFrameGraph(Definition, CompileData);
            // All versions of an in-place allocation need the union of their views.
            for (auto it = plan.Order.rbegin(); it != plan.Order.rend(); ++it)
            {
                for (auto& entry : plan.Resources)
                {
                    if (entry.first.Node != *it || entry.second.Alias.Node.empty()) { continue; }
                    auto original = Sources.at(entry.second.Alias);
                    plan.Resources.at(original).Description.Usages |= entry.second.Description.Usages;
                }
            }
            std::vector<Slot> slots(CompileData.FrameSlotCount);
            bool retry = false;
            for (auto& slot : slots)
            {
                for (const auto& entry : plan.Resources)
                {
                    const auto& resource = entry.second;
                    auto& material = slot.Resources[entry.first];
                    if (!resource.Alias.Node.empty()) { continue; }
                    auto* owner = GetDeviceResources(Definition.Nodes.at(entry.first.Node).DeviceRole);
                    auto* other = owner == Primary ? Secondary : Primary;
                    if (Imports.count(entry.first.Node))
                    { material.LocalResource = WrapFrameGraphImport(owner, Imports.at(entry.first.Node)(), resource.Description); material.ProducerResource = material.LocalResource.get(); }
                    else if (resource.Storage == MemoryType::Local)
                    { material.LocalResource = CreateFrameGraphNative(owner, resource.Description); material.ProducerResource = material.LocalResource.get(); }
                    if (resource.Storage == MemoryType::Shared || resource.Export)
                    {
                        const HRESULT result = TryCreateFrameGraphShared(owner, other ? other : owner, resource.Description, resource.BufferPayload, material.Shared);
                        if (FAILED(result))
                        {
                            bool narrowed = false;
                            const auto policy = Definition.Nodes.at(entry.first.Node).Description.Outputs.at(entry.first.Pin).RequestedMemory;
                            if (resource.Storage == MemoryType::Shared && policy == MemoryType::Auto
                                && CompileData.Ports.at(entry.first).Capabilities.Shared)
                            { CompileData.Ports.at(entry.first).Capabilities.Shared = false; narrowed = true; }
                            for (const auto& [target, binding] : resource.Consumers)
                            {
                                const auto access = Definition.Nodes.at(target.Node).Description.Inputs.at(target.Pin).RequestedMemory;
                                if (binding == MemoryType::Shared && access == MemoryType::Auto
                                    && CompileData.Ports.at(target).Capabilities.Shared)
                                { CompileData.Ports.at(target).Capabilities.Shared = false; narrowed = true; }
                            }
                            if (!narrowed) { ThrowIfFailed(result); }
                            Fallbacks.push_back(entry.first.Node + "." + entry.first.Pin + ": Auto excluded a shared candidate rejected by actual resource creation");
                            retry = true;
                            break;
                        }
                        if (resource.Storage == MemoryType::Shared) { material.ProducerResource = &material.Shared.GetDeviceMapping(owner->Device); }
                    }
                    for (const auto& [target, access] : resource.Consumers)
                    {
                        const auto& device = Definition.Nodes.at(target.Node).DeviceRole;
                        if (NeedsFrameGraphLocalCopy(resource.Storage, access, GetDeviceResources(device) != owner) && !material.LocalCopies.count(device))
                        { material.LocalCopies.emplace(device, CreateFrameGraphNative(GetDeviceResources(device), resource.Description)); }
                    }
                }
                if (retry) { break; }
            }
            if (retry) { continue; }
            Slots = std::move(slots);
            Plan = std::move(plan);
            ++Generation;
            return;
        }
    }

    GDX12CompletionPoint SubmitCommandList(GDX12CommandQueue* queue, GDX12CommandList* commands, Slot& slot)
    {
        queue->ExecuteCommandList(commands);
        auto point = queue->GetCompletionPoint();
        slot.Completions.push_back(point);
        return point;
    }

    GDX12CompletionPoint TransferResourceData(FrameGraphPhysicalResource& source, FrameGraphPhysicalResource& target,
        const GDX12CompletionPoint& ready, bool useCopy, Slot& slot, const std::string& label)
    {
        auto* direct = source.DeviceResources->Device->GetCommandQueue();
        auto* queue = useCopy ? source.DeviceResources->Device->GetCopyCommandQueue() : direct;
        auto dependency = ready;
        direct->GPUWait(slot.LastUse[&source]);
        direct->GPUWait(slot.LastUse[&target]);
        if (useCopy)
        {
            direct->GPUWait(ready);
            auto* release = direct->GetCommandList();
            TransitionFrameGraphResource(release, source, D3D12_RESOURCE_STATE_COMMON);
            TransitionFrameGraphResource(release, target, D3D12_RESOURCE_STATE_COMMON);
            dependency = SubmitCommandList(direct, release, slot);
        }
        queue->GPUWait(dependency);
        auto* commands = queue->GetCommandList();
        commands->BeginPixEvent(label, Colors::Orange);
        if (!useCopy)
        {
            TransitionFrameGraphResource(commands, source, D3D12_RESOURCE_STATE_COPY_SOURCE);
            TransitionFrameGraphResource(commands, target, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        auto statistics = CopyFrameGraphResource(commands, source, target);
        CopyBytes += statistics.Bytes; CopyCommands += statistics.Commands;
        // COPY submissions promote from COMMON and decay back at ExecuteCommandLists.
        if (!useCopy)
        {
            TransitionFrameGraphResource(commands, source, D3D12_RESOURCE_STATE_COMMON);
            TransitionFrameGraphResource(commands, target, D3D12_RESOURCE_STATE_COMMON);
        }
        commands->EndPixEvent();
        auto completion = SubmitCommandList(queue, commands, slot);
        slot.LastUse[&source] = completion;
        slot.LastUse[&target] = completion;
        return completion;
    }

    std::pair<FrameGraphPhysicalResource*, GDX12CompletionPoint> ResolveInputResource(const PinAddress& target, Slot& slot)
    {
        auto source = Sources.at(target);
        auto& material = slot.Resources.at(source);
        const auto& resource = Plan.Resources.at(source);
        const auto access = resource.Consumers.at(target);
        auto* owner = GetDeviceResources(Definition.Nodes.at(target.Node).DeviceRole);
        const bool remote = Definition.Nodes.at(source.Node).DeviceRole != Definition.Nodes.at(target.Node).DeviceRole;
        if (access == MemoryType::Local && !NeedsFrameGraphLocalCopy(resource.Storage, access, remote))
        { return {material.ProducerResource, material.ProducerReady}; }
        auto& sharedSet = GetSharedResources(material);
        if (!material.SharedReady)
        {
            if (resource.Export)
            {
                if (!sharedSet.Producer)
                { ThrowIfFailed(TryCreateFrameGraphShared(material.ProducerResource->DeviceResources, owner, resource.Description, resource.BufferPayload, sharedSet)); }
                auto* shared = &sharedSet.GetDeviceMapping(material.ProducerResource->DeviceResources->Device);
                material.SharedReady = TransferResourceData(*material.ProducerResource, *shared, material.ProducerReady, Plan.UseCopyQueue, slot, "Export " + source.Node + "." + source.Pin);
            }
            else { material.SharedReady = material.ProducerReady; }
        }
        auto* shared = &sharedSet.GetDeviceMapping(owner->Device);
        if (access == MemoryType::Shared) { return {shared, material.SharedReady}; }
        auto device = Definition.Nodes.at(target.Node).DeviceRole;
        if (!material.LocalCopyReady.count(device))
        {
            if (!material.LocalCopies.count(device)) { material.LocalCopies[device] = CreateFrameGraphNative(owner, resource.Description); }
            material.LocalCopyReady[device] = TransferResourceData(*shared, *material.LocalCopies.at(device), material.SharedReady, Plan.UseCopyQueue, slot, "Import " + source.Node + "." + source.Pin);
        }
        return {material.LocalCopies.at(device).get(), material.LocalCopyReady.at(device)};
    }

    void ExecuteFrame()
    {
        WaitForCurrentFrameSlot();

        if (ResourceRebuildRequested)
        {
            WaitForGPUCompletion();
            ResolveResourceRequirements();
            BuildResources();
            for (auto& entry : Passes)
            {
                const auto& node = Definition.Nodes.at(entry.first);
                PassInitContext init(GetDeviceResources(node.DeviceRole), Common, node, Plan, Sources);
                entry.second->OnResourcesRecreated(init);
            }
            ResourceRebuildRequested = false;
        }

        auto& slot = Slots[GetCurrentFrameSlotIndex()];
        slot.LastUse.clear();
        for (auto& entry : slot.Resources)
        {
            entry.second.SharedReady = {};
            entry.second.ProducerReady = {};
            entry.second.LocalCopyReady.clear();
        }

        std::map<NodeId, GDX12CompletionPoint> nodeCompletions;
        for (const auto& id : Plan.Order)
        {
            const auto& node = Definition.Nodes.at(id);
            if (Imports.count(id))
            {
                auto* texture = Imports.at(id)();
                auto& material = slot.Resources.at({id,"Resource"});
                material.LocalResource->ImportedTexture = texture;
                slot.ExternalResources.push_back(texture->GetResource()->D3DResource);
                material.ProducerResource = material.LocalResource.get();
                continue;
            }
            auto* resources = GetDeviceResources(node.DeviceRole);
            resources->CurrFrameConstantsIndex = GetCurrentFrameSlotIndex();
            auto* queue = resources->Device->GetCommandQueue();
            for (const auto& dependency : Plan.Dependencies.at(id))
            { queue->GPUWait(nodeCompletions[dependency]); }
            const auto& description = node.Description;
            std::map<PinId, PassBinding> bindings;
            for (const auto& pin : description.Inputs)
            {
                if (!Sources.count({id,pin.first})) { continue; }
                auto binding = ResolveInputResource({id,pin.first}, slot);
                queue->GPUWait(binding.second);
                queue->GPUWait(slot.LastUse[binding.first]);
                bindings[pin.first] = binding.first->CreatePassBinding(pin.second.Usages);
            }
            for (const auto& pin : description.Outputs)
            {
                auto& material = slot.Resources.at({id,pin.first});
                if (!pin.second.AliasInput.empty())
                {
                    material.ProducerResource = bindings.at(pin.second.AliasInput).Resource;
                    if (Plan.Resources.at({id,pin.first}).Storage == MemoryType::Shared)
                    { material.SharedAllocationAlias = &GetSharedResources(slot.Resources.at(Sources.at({id,pin.second.AliasInput}))); }
                }
                bindings[pin.first] = material.ProducerResource->CreatePassBinding(pin.second.Usages);
            }
            auto* commands = queue->GetCommandList();
            commands->BeginPixEvent(id, Colors::CornflowerBlue);
            Timers.at(id)->Start(commands);
            PassInitContext init(resources, Common, node, Plan, Sources);
            PassExecutionContext execution(init, commands, Frame, GetCurrentFrameSlotIndex(), bindings);
            for (const auto& external : Passes.at(id)->GetExternalResources(execution))
            {
                if (!external.Resource || !external.Resource->D3DResource) { continue; }
                queue->GPUWait(external.Ready);
                slot.ExternalResources.push_back(external.Resource->D3DResource);
                D3D12_HEAP_PROPERTIES heap = {};
                D3D12_HEAP_FLAGS flags = {};
                ThrowIfFailed(external.Resource->D3DResource->GetHeapProperties(&heap, &flags));
                if (heap.Type == D3D12_HEAP_TYPE_UPLOAD) { external.Resource->SetCurrentState(D3D12_RESOURCE_STATE_GENERIC_READ); }
                else { commands->ResourceBarrier({external.Resource->GetTransitionBarrier(external.State)}); }
            }
            for (const auto* ports : {&description.Inputs, &description.Outputs})
            {
                for (const auto& pin : *ports)
                {
                    if (!bindings.count(pin.first) || (ports == &description.Outputs && !pin.second.AliasInput.empty())) { continue; }
                    TransitionFrameGraphResource(commands, *bindings.at(pin.first).Resource, UsageState(pin.second.Usages));
                    if (HasUsage(pin.second.Usages, ResourceUsage::UnorderedAccess)) { FrameGraphUAVBarrier(commands, *bindings.at(pin.first).Resource); }
                }
            }
            Passes.at(id)->Execute(execution);
            for (bool input : {true, false})
            {
                for (const auto& pin : input ? description.Inputs : description.Outputs)
                {
                    if (!bindings.count(pin.first)) { continue; }
                    TransitionFrameGraphResource(commands, *bindings.at(pin.first).Resource, UsageState(pin.second.Usages));
                    if (HasUsage(pin.second.Usages, ResourceUsage::UnorderedAccess)) { FrameGraphUAVBarrier(commands, *bindings.at(pin.first).Resource); }
                }
            }
            // Cross-adapter handoffs leave each device mapping coherent in COMMON.
            for (const auto& pin : description.Outputs)
            {
                if (!bindings.count(pin.first)) { continue; }
                if (Plan.Resources.at({id,pin.first}).Storage == MemoryType::Shared)
                { TransitionFrameGraphResource(commands, *bindings.at(pin.first).Resource, D3D12_RESOURCE_STATE_COMMON); }
            }
            Timers.at(id)->Stop(commands);
            commands->EndPixEvent();
            auto completion = SubmitCommandList(queue,commands,slot);
            Timers.at(id)->OnCommandListExecuted(completion.Value);
            nodeCompletions[id] = completion;
            for (const auto& binding : bindings) { slot.LastUse[binding.second.Resource] = completion; }
            for (const auto& pin : description.Outputs)
            { slot.Resources.at({id,pin.first}).ProducerReady = completion; }
        }
        ++Frame;
    }
};

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

    runtime->Passes = std::move(_passes);
    runtime->MergeDuplicateTextureImports();
    for (const auto& edge : runtime->Definition.Connections) { runtime->Sources[edge.Target] = edge.Source; }
    runtime->ResolveResourceRequirements();
    runtime->BuildResources();
    for (auto& entry : runtime->Passes)
    {
        const auto& node = runtime->Definition.Nodes.at(entry.first);
        PassInitContext context(runtime->GetDeviceResources(node.DeviceRole), _common, node, runtime->Plan, runtime->Sources);
        entry.second->Initialize(context);
        runtime->Timers[entry.first] = std::make_unique<GDX12GPUTimer>(runtime->GetDeviceResources(node.DeviceRole)->Device);
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
void CompiledFrameGraph::RequestResourceRebuild() { _runtime->ResourceRebuildRequested = true; }
void CompiledFrameGraph::WaitForCurrentFrameSlot() { _runtime->WaitForCurrentFrameSlot(); }
void CompiledFrameGraph::WaitForGPUCompletion() { _runtime->WaitForGPUCompletion(); }
void CompiledFrameGraph::ReleaseFrameResourcesForResize()
{
    _runtime->WaitForGPUCompletion();
    _runtime->Slots.clear();
}
uint32_t CompiledFrameGraph::GetCurrentFrameSlotIndex() const { return _runtime->GetCurrentFrameSlotIndex(); }
const FrameGraphPlan& CompiledFrameGraph::GetExecutionPlan() const { return _runtime->Plan; }
std::string CompiledFrameGraph::GetDebugReport() const
{
    std::ostringstream out;
    out << _runtime->Plan.GetDebugReport(_runtime->Definition) << "generation=" << _runtime->Generation << " submitted copy commands=" << _runtime->CopyCommands << " bytes=" << _runtime->CopyBytes << '\n';
    for (const auto& fallback : _runtime->Fallbacks) { out << fallback << '\n'; }
    for (const auto& node : _runtime->Definition.Nodes)
    {
        out << node.first << " device=" << GetDeviceRoleName(node.second.DeviceRole);
        auto timer = _runtime->Timers.find(node.first);
        if (timer != _runtime->Timers.end()) { out << " DIRECT ms=" << timer->second->GetTimeMS(); }
        out << '\n';
        for (const auto& pin : node.second.Description.Outputs)
        { out << "  " << pin.first << " requested storage=" << GetMemoryTypeName(pin.second.RequestedMemory) << '\n'; }
        for (const auto& pin : node.second.Description.Inputs)
        { out << "  " << pin.first << " requested access=" << GetMemoryTypeName(pin.second.RequestedMemory) << '\n'; }
    }
    if (!_runtime->Slots.empty())
    {
        for (const auto& resource : _runtime->Slots.front().Resources)
        {
            const auto& material = resource.second;
            out << "allocation " << resource.first.Node << '.' << resource.first.Pin << " slots=" << _runtime->Slots.size();
            if (material.LocalResource) { out << " native bytes=" << material.LocalResource->AllocationBytes; }
            for (const auto& shared : material.Shared.Allocations) { out << " shared bytes=" << shared->GetHeapSize(); }
            for (const auto& replica : material.LocalCopies) { out << " replica=" << GetDeviceRoleName(replica.first) << " bytes=" << replica.second->AllocationBytes; }
            out << '\n';
            for (const auto* mapping : {material.Shared.Producer.get(), material.Shared.Consumer.get()})
            {
                if (!mapping) { continue; }
                for (size_t i=0;i<mapping->Footprints.size();++i)
                {
                    const auto& footprint = mapping->Footprints[i];
                    out << "  subresource=" << i << " offset=" << footprint.Offset << " pitch=" << footprint.Footprint.RowPitch
                        << " extent=" << footprint.Footprint.Width << 'x' << footprint.Footprint.Height << '\n';
                }
            }
        }
    }
    return out.str();
}
GDX12RenderPass& CompiledFrameGraph::FindPass(const NodeId& id) const
{
    auto pass = _runtime->Passes.find(id);
    if (pass == _runtime->Passes.end()) { throw std::invalid_argument("ERROR: Node '" + id + "' does not name a render pass in this FrameGraph. Use the NodeId of a render pass added to this graph."); }
    return *pass->second;
}
bool CompiledFrameGraph::UsesDevice(EDeviceRole role) const
{
    for (const auto& node : _runtime->Definition.Nodes)
    { if (node.second.DeviceRole == role) { return true; } }
    return false;
}
uint32_t CompiledFrameGraph::GetDevicePassFlags(EDeviceRole role) const
{
    uint32_t flags = 0;
    for (const auto& entry : _runtime->Passes)
    { if (_runtime->Definition.Nodes.at(entry.first).DeviceRole == role) { flags |= entry.second->GetFlags(); } }
    return flags;
}
void CompiledFrameGraph::ResizeResources(UInt2 display)
{
    if (!display.x || !display.y) { throw std::invalid_argument("ERROR: Given display width or height is zero. Call ResizeResources() with non-zero display width and height."); }
    _runtime->WaitForGPUCompletion();
    _runtime->CompileData.DisplaySize = display;
    _runtime->ResolveResourceRequirements();
    _runtime->BuildResources();
    for (auto& entry : _runtime->Passes)
    {
        const auto& node = _runtime->Definition.Nodes.at(entry.first);
        PassInitContext context(_runtime->GetDeviceResources(node.DeviceRole), _runtime->Common, node, _runtime->Plan, _runtime->Sources);
        entry.second->OnResourcesRecreated(context);
    }
    _runtime->ResourceRebuildRequested = false;
}
