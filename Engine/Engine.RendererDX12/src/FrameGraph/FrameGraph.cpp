#include "Engine.RendererDX12/FrameGraph/FrameGraph.h"
#include "Engine.RendererDX12/FrameGraph/FrameGraphResources.h"
#include "Engine.RendererDX12/GDX12GPUTimer.h"

#include <algorithm>
#include <iomanip>
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
    // Physical representations of one logical output in one frame slot.
    // Several logical versions can reference the same allocation through AliasInput.
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

    enum class CopyScope { Local, CrossGPU, CrossGPUAfterExport };

    struct CopySample
    {
        GDX12GPUTimer* Timer;
        EDeviceRole Device;
        UINT64 FenceValue;
        bool Exporting;
        uint64_t ResourceBytes;
    };

    struct CopyTiming
    {
        double TimeMS = 0.0;
        size_t Copies = 0;
        size_t Measured = 0;

        void Add(double ms)
        {
            ++Copies;
            if (ms >= 0.0) { TimeMS += ms; ++Measured; }
        }

        double GetCompleteTimeMS() const { return Measured == Copies ? TimeMS : -1.0; }
    };

    struct QueueTimestampSample
    {
        GDX12CommandQueue* Queue = nullptr;
        UINT64 Frequency = 0;
        GDX12GPUTimer::TimestampRange Timestamps{};
    };

    struct CompletedFrameTiming
    {
        uint64_t FrameNumber;
        std::array<CopyTiming, 2> CopyTimes{};
        FrameGraphCrossGPUCopyStats CrossGPUCopies;
        // Two devices, each with a DIRECT and a COPY queue.
        std::array<QueueTimestampSample, 4> QueueSamples{};
    };

    // GDX12FrameConstants's brother of FrameGraph
    // created for each NumFrameConstants
    struct FrameSlotResources
    {
        std::map<PinAddress, Materialization> Resources;
        std::vector<GDX12CompletionPoint> Completions;
        std::map<FrameGraphPhysicalResource*, GDX12CompletionPoint> LastUse;
        std::vector<ComPtr<ID3D12Resource>> ExternalResources;
        std::optional<uint64_t> SubmittedFrame;
        std::vector<CopySample> CopySamples;
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
    std::map<NodeId, std::unique_ptr<GDX12RenderPass>> Passes;
    std::map<NodeId, std::unique_ptr<GDX12GPUTimer>> Timers;
    std::map<GDX12CommandQueue*, GDX12GPUTimer> FrameQueueTimers;
    // The pair identifies the source pin and whether this is its export or import.
    std::map<GDX12CommandQueue*, std::map<std::pair<PinAddress, bool>, GDX12GPUTimer>> TransportTimers;
    std::map<NodeId, std::function<GDX12Texture*()>> Imports;
    std::map<PinAddress, PinAddress> Sources;
    std::vector<FrameSlotResources> FrameSlots;
    std::optional<CompletedFrameTiming> LastCompletedFrameTiming;
    uint64_t Frame = 0;
    uint64_t Generation = 0;
    uint64_t CopyBytes = 0;
    uint64_t CopyCommands = 0;
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
            CompletedFrameTiming timing{*frameResources.SubmittedFrame};
            CopyTiming exports, imports;
            for (const auto& sample : frameResources.CopySamples)
            {
                const double ms = sample.Timer->GetTimeMS(sample.FenceValue);
                timing.CopyTimes[sample.Device == DEVICE_ROLE_PRIMARY ? 0 : 1].Add(ms);
                (sample.Exporting ? exports : imports).Add(ms);
                if (sample.ResourceBytes)
                {
                    ++timing.CrossGPUCopies.ResourceCount;
                    timing.CrossGPUCopies.Bytes += sample.ResourceBytes;
                }
            }
            timing.CrossGPUCopies.ExportMS = exports.GetCompleteTimeMS();
            timing.CrossGPUCopies.ImportMS = imports.GetCompleteTimeMS();
            for (size_t i = 0; i < frameResources.UsedQueues.size(); ++i)
            {
                auto* queue = frameResources.UsedQueues[i];
                const auto& timer = FrameQueueTimers.at(queue);
                timing.QueueSamples[i].Queue = queue;
                if (timer.IsAvailable())
                {
                    timing.QueueSamples[i].Frequency = timer.GetTimestampFrequency();
                    timing.QueueSamples[i].Timestamps = timer.ReadResolvedTimestamps(slot);
                }
            }
            LastCompletedFrameTiming = timing;
            frameResources.SubmittedFrame.reset();
            frameResources.CopySamples.clear();
            frameResources.UsedQueues.clear();
        }
        frameResources.Completions.clear();
        frameResources.ExternalResources.clear();
    }

    struct GPUTimeSpan
    {
        double Begin = (std::numeric_limits<double>::max)();
        double End = (std::numeric_limits<double>::lowest)();
        bool Used = false;
        bool Complete = true;
        double ExcludedGap = 0.0;

        double DurationMS() const
        {
            if (!Used || !Complete) { return -1.0; }
            return End - Begin - ExcludedGap;
        }
    };

    std::array<GPUTimeSpan, 2> CalculateGPUFrameSpans(const CompletedFrameTiming& frame) const
    {
        LARGE_INTEGER qpcFrequency{}, qpcOrigin{};
        QueryPerformanceFrequency(&qpcFrequency);
        QueryPerformanceCounter(&qpcOrigin);
        std::array<GPUTimeSpan, 2> spans;
        for (const auto& sample : frame.QueueSamples)
        {
            if (!sample.Queue) { continue; }
            const auto role = sample.Queue->GetCompletionPoint().Device->Role;
            auto& span = spans[role == DEVICE_ROLE_PRIMARY ? 0 : 1];
            span.Used = true;
            UINT64 gpuNow = 0, cpuNow = 0;
            if (!sample.Frequency || sample.Timestamps.End < sample.Timestamps.Begin || qpcFrequency.QuadPart <= 0 ||
                FAILED(sample.Queue->GetCommandQueue()->GetClockCalibration(&gpuNow, &cpuNow)))
            {
                span.Complete = false;
                continue;
            }

            // Compare timestamps from different queues on the common QPC timeline.
            const double calibrationMS = (static_cast<double>(cpuNow) - qpcOrigin.QuadPart) * 1000.0 / qpcFrequency.QuadPart;
            const auto toMS = [&](UINT64 ticks)
            {
                const double delta = ticks >= gpuNow ? static_cast<double>(ticks - gpuNow) : -static_cast<double>(gpuNow - ticks);
                return calibrationMS + delta * 1000.0 / sample.Frequency;
            };
            span.Begin = (std::min)(span.Begin, toMS(sample.Timestamps.Begin));
            span.End = (std::max)(span.End, toMS(sample.Timestamps.End));
        }

        return spans;
    }

    void WriteGPUFrameTiming(std::ostream& out) const
    {
        if (!LastCompletedFrameTiming) { out << "\n[GPU frame timings]\n    Pending\n"; return; }

        const auto& frame = *LastCompletedFrameTiming;
        const auto [primary, secondary] = CalculateGPUFrameSpans(frame);
        const auto writeSpan = [&](const char* name, const GPUTimeSpan& span)
        {
            out << "    " << name << ": ";
            if (!span.Complete) { out << "unavailable (incomplete queue timestamps)"; }
            else if (span.Used) { out << span.DurationMS() << " ms"; }
            else { out << "idle"; }
            out << '\n';
        };

        out << "\n[GPU frame " << frame.FrameNumber << " timings]\n";
        if (!Secondary)
        {
            writeSpan("total-frame", primary);
            return;
        }

        writeSpan("primary", primary);
        writeSpan("secondary", secondary);
        GPUTimeSpan total{
            (std::min)(primary.Begin, secondary.Begin), (std::max)(primary.End, secondary.End),
            primary.Used || secondary.Used, primary.Complete && secondary.Complete};
        if (total.Complete && primary.Used && secondary.Used)
        {
            // Count overlapping GPU spans once and exclude any gap between them.
            total.ExcludedGap = (std::max)(0.0,
                (std::max)(primary.Begin, secondary.Begin) - (std::min)(primary.End, secondary.End));
        }
        writeSpan("total-frame", total);
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
        // Passes can query SDK resolutions or scene capacities before any graph allocation is made.
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
                    auto original = Sources.at(entry.second.Alias);
                    plan.Resources.at(original).Description.Usages |= entry.second.Description.Usages;
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
                            // Reconsider only Auto choices; fixed memory requirements remain constraints.
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
                    // Multiple inputs on one GPU share a single local replica of this logical output.
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
            FrameSlots = std::move(newFrameSlots);
            LastCompletedFrameTiming.reset();
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
            ++Generation;
            return;
        }
    }

    GDX12CommandList* GetFrameCommandList(GDX12CommandQueue* queue, FrameSlotResources& frameResources)
    {
        auto* commands = queue->GetCommandList();
        if (std::find(frameResources.UsedQueues.begin(), frameResources.UsedQueues.end(), queue) == frameResources.UsedQueues.end())
        {
            auto [timer, inserted] = FrameQueueTimers.try_emplace(queue);
            if (inserted) { timer->second.Initialize(queue->GetCompletionPoint().Device, queue, CompileData.FrameSlotCount); }
            timer->second.RecordBegin(commands, GetCurrentFrameSlotResourcesIndex());
            frameResources.UsedQueues.push_back(queue);
        }
        return commands;
    }

    // Update the end timestamp in each existing submission. The last submission leaves the final timestamp.
    GDX12CompletionPoint SubmitCommandList(GDX12CommandQueue* queue, GDX12CommandList* commands, FrameSlotResources& frameResources)
    {
        FrameQueueTimers.at(queue).RecordEndAndResolve(commands, GetCurrentFrameSlotResourcesIndex());
        queue->ExecuteCommandList(commands);
        auto point = queue->GetCompletionPoint();
        frameResources.Completions.push_back(point);
        return point;
    }

    void SubmitLocalBatch(GDX12CommandQueue* queue, FrameSlotResources& frameResources)
    {
        if (LocalBatch.Lists.empty()) { return; }
        FrameQueueTimers.at(queue).RecordEndAndResolve(LocalBatch.Lists.back(), GetCurrentFrameSlotResourcesIndex());
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
        const GDX12CompletionPoint& ready, bool useCopy, FrameSlotResources& frameResources,
        const PinAddress& sourcePin, bool exporting, CopyScope scope)
    {
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
        commands->BeginPixEvent((exporting ? "Export " : "Import ") + sourcePin.Node + "." + sourcePin.Pin, Colors::Orange);
        if (!useCopy)
        {
            TransitionFrameGraphResource(commands, source, D3D12_RESOURCE_STATE_COPY_SOURCE);
            TransitionFrameGraphResource(commands, target, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        // Time the copy commands on their own queue. A busy readback slot must not delay transport.
        auto [timer, inserted] = TransportTimers[queue].try_emplace(std::make_pair(sourcePin, exporting));
        if (inserted) { timer->second.Initialize(device, queue, CompileData.FrameSlotCount); }
        const bool timed = timer->second.Start(commands, false);
        auto statistics = CopyFrameGraphResource(commands, source, target);
        if (timed) { timer->second.Stop(commands); }
        CopyBytes += statistics.Bytes; CopyCommands += statistics.Commands;
        // COPY uses implicit promotion from COMMON; resources decay back after its submission completes.
        if (!useCopy)
        {
            TransitionFrameGraphResource(commands, source, D3D12_RESOURCE_STATE_COMMON);
            TransitionFrameGraphResource(commands, target, D3D12_RESOURCE_STATE_COMMON);
        }
        commands->EndPixEvent();
        auto completion = SubmitCommandList(queue, commands, frameResources);
        if (timed) { timer->second.OnCommandListExecuted(completion.Value); }
        if (scope != CopyScope::Local)
        {
            const uint64_t bytes = scope == CopyScope::CrossGPU ? statistics.Bytes : 0;
            frameResources.CopySamples.push_back({&timer->second, device->Role, timed ? completion.Value : 0, exporting, bytes});
        }
        frameResources.LastUse[&source] = completion;
        frameResources.LastUse[&target] = completion;
        return completion;
    }

    // Resolve an input to a device-visible representation and the fence that makes its data ready.
    std::pair<FrameGraphPhysicalResource*, GDX12CompletionPoint> ResolveInputResource(const PinAddress& target, FrameSlotResources& frameResources)
    {
        auto source = Sources.at(target);
        auto& material = frameResources.Resources.at(source);
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
                const auto producerRole = Definition.Nodes.at(source.Node).DeviceRole;
                const bool hasRemoteConsumer = std::any_of(resource.Consumers.begin(), resource.Consumers.end(), [&](const auto& consumer)
                { return Definition.Nodes.at(consumer.first.Node).DeviceRole != producerRole; });
                material.SharedReady = TransferResourceData(*material.ProducerResource, *shared, material.ProducerReady,
                    Plan.UseCopyQueue, frameResources, source, true, hasRemoteConsumer ? CopyScope::CrossGPU : CopyScope::Local);
            }
            else { material.SharedReady = material.ProducerReady; }
        }
        auto* shared = &sharedSet.GetDeviceMapping(owner->Device);
        if (access == MemoryType::Shared) { return {shared, material.SharedReady}; }
        auto device = Definition.Nodes.at(target.Node).DeviceRole;
        if (!material.LocalCopyReady.count(device))
        {
            if (!material.LocalCopies.count(device)) { material.LocalCopies[device] = CreateFrameGraphNative(owner, resource.Description); }
            auto scope = CopyScope::Local;
            // Count the resource once, even when transport uses both an export and an import.
            if (remote) { scope = resource.Export ? CopyScope::CrossGPUAfterExport : CopyScope::CrossGPU; }
            material.LocalCopyReady[device] = TransferResourceData(*shared, *material.LocalCopies.at(device), material.SharedReady,
                Plan.UseCopyQueue, frameResources, source, false, scope);
        }
        return {material.LocalCopies.at(device).get(), material.LocalCopyReady.at(device)};
    }

    // Coalesce pending rebuilds before the frame takes references into the replacement slots.
    void RebuildRequestedFrameSlotResources()
    {
        if (!ResourceRebuildRequested) { return; }
        WaitForGPUCompletion();
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
            resourceEntry.second.LocalCopyReady.clear();
        }
    }

    // Resolve the current external texture and retain its native allocation for this frame.
    void RefreshImportedTexture(const NodeId& nodeId, FrameSlotResources& frameResources)
    {
        // Refresh rotating back buffers or replaced textures without recreating the source node
        auto* importedTexture = Imports.at(nodeId)();
        auto& resourceInstance = frameResources.Resources.at({nodeId,"Resource"});
        resourceInstance.LocalResource->ImportedTexture = importedTexture;
        frameResources.ExternalResources.push_back(importedTexture->GetResource()->D3DResource);
        resourceInstance.ProducerResource = resourceInstance.LocalResource.get();
    }

    // Resolve inputs, wait for their data and publish alias outputs using the same physical allocation.
    std::map<PinId, PassBinding> PreparePassBindings(const NodeDefinition& nodeDefinition,
        GDX12CommandQueue* commandQueue, FrameSlotResources& frameResources)
    {
        const auto& passDescription = nodeDefinition.Description;
        std::map<PinId, PassBinding> portBindings;
        for (const auto& portEntry : passDescription.Inputs)
        {
            if (!Sources.count({nodeDefinition.Id,portEntry.first})) { continue; }
            auto inputBinding = ResolveInputResource({nodeDefinition.Id,portEntry.first}, frameResources);
            commandQueue->GPUWait(inputBinding.second);
            commandQueue->GPUWait(frameResources.LastUse[inputBinding.first]);
            portBindings[portEntry.first] = inputBinding.first->CreatePassBinding(portEntry.second.Usages);
        }
        for (const auto& portEntry : passDescription.Outputs)
        {
            auto& resourceInstance = frameResources.Resources.at({nodeDefinition.Id,portEntry.first});
            if (!portEntry.second.AliasInput.empty())
            {
                resourceInstance.ProducerResource = portBindings.at(portEntry.second.AliasInput).Resource;
                if (Plan.Resources.at({nodeDefinition.Id,portEntry.first}).Storage == MemoryType::Shared)
                { resourceInstance.SharedAllocationAlias = &GetSharedResources(frameResources.Resources.at(Sources.at({nodeDefinition.Id,portEntry.second.AliasInput}))); }
            }
            portBindings[portEntry.first] = resourceInstance.ProducerResource->CreatePassBinding(portEntry.second.Usages);
        }
        return portBindings;
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

    // Prepare input and output states, leaving alias outputs in their input state until execution.
    void PreparePassResourceStates(const PassDescription& passDescription, const std::map<PinId, PassBinding>& portBindings,
        GDX12CommandList* commandList)
    {
        for (const auto* portList : {&passDescription.Inputs, &passDescription.Outputs})
        {
            for (const auto& portEntry : *portList)
            {
                if (!portBindings.count(portEntry.first) || (portList == &passDescription.Outputs && !portEntry.second.AliasInput.empty())) { continue; }
                TransitionFrameGraphResource(commandList, *portBindings.at(portEntry.first).Resource, UsageState(portEntry.second.Usages));
                if (HasUsage(portEntry.second.Usages, ResourceUsage::UnorderedAccess)) { FrameGraphUAVBarrier(commandList, *portBindings.at(portEntry.first).Resource); }
            }
        }
    }

    // Restore declared states, publish alias output states and release shared outputs to COMMON.
    void RestorePassResourceStates(const NodeDefinition& nodeDefinition, const std::map<PinId, PassBinding>& portBindings,
        GDX12CommandList* commandList)
    {
        const auto& passDescription = nodeDefinition.Description;
        // Restore declared states after pass-local changes, then apply alias output states.
        // This is where an Output pass can leave the same input back buffer in PRESENT.
        for (bool isInputPort : {true, false})
        {
            for (const auto& portEntry : isInputPort ? passDescription.Inputs : passDescription.Outputs)
            {
                if (!portBindings.count(portEntry.first)) { continue; }
                TransitionFrameGraphResource(commandList, *portBindings.at(portEntry.first).Resource, UsageState(portEntry.second.Usages));
                if (HasUsage(portEntry.second.Usages, ResourceUsage::UnorderedAccess)) { FrameGraphUAVBarrier(commandList, *portBindings.at(portEntry.first).Resource); }
            }
        }
        // Cross-adapter handoffs leave each device mapping coherent in COMMON.
        for (const auto& portEntry : passDescription.Outputs)
        {
            if (!portBindings.count(portEntry.first)) { continue; }
            if (Plan.Resources.at({nodeDefinition.Id,portEntry.first}).Storage == MemoryType::Shared)
            { TransitionFrameGraphResource(commandList, *portBindings.at(portEntry.first).Resource, D3D12_RESOURCE_STATE_COMMON); }
        }
    }

    // Publish the submission point for node dependencies, physical last use and output readiness.
    void TrackPassCompletion(const NodeDefinition& nodeDefinition, const std::map<PinId, PassBinding>& portBindings,
        const GDX12CompletionPoint& completionPoint, FrameSlotResources& frameResources,
        std::map<NodeId, GDX12CompletionPoint>& nodeCompletions)
    {
        nodeCompletions[nodeDefinition.Id] = completionPoint;
        for (const auto& portBinding : portBindings) { frameResources.LastUse[portBinding.second.Resource] = completionPoint; }
        for (const auto& outputEntry : nodeDefinition.Description.Outputs)
        { frameResources.Resources.at({nodeDefinition.Id,outputEntry.first}).ProducerReady = completionPoint; }
    }

    // Record in plan order, preserving separate command-list state for each pass.
    void RecordPass(const NodeDefinition& nodeDefinition, FrameSlotResources& frameResources,
        std::map<NodeId, GDX12CompletionPoint>& nodeCompletions)
    {
        auto* deviceResources = GetDeviceResources(nodeDefinition.DeviceRole);
        deviceResources->CurrFrameConstantsIndex = GetCurrentFrameSlotResourcesIndex();
        auto* commandQueue = deviceResources->Device->GetCommandQueue();
        if (!BatchLocalPasses)
        {
            for (const auto& dependencyId : Plan.Dependencies.at(nodeDefinition.Id))
            { commandQueue->GPUWait(nodeCompletions[dependencyId]); }
        }
        auto portBindings = PreparePassBindings(nodeDefinition, commandQueue, frameResources);
        auto* commandList = GetFrameCommandList(commandQueue, frameResources);
        auto* renderPass = Passes.at(nodeDefinition.Id).get();
        auto* passTimer = Timers.at(nodeDefinition.Id).get();
        const auto timingSlot = GetPassTimingSlot(Frame);

        passTimer->RecordBegin(commandList, timingSlot);
        PassInitContext initContext(deviceResources, Common, nodeDefinition, Plan, Sources);
        PassExecutionContext executionContext(initContext, commandList, Frame, GetCurrentFrameSlotResourcesIndex(), portBindings);

        PreparePassExternalResources(*renderPass, executionContext, frameResources);
        PreparePassResourceStates(nodeDefinition.Description, portBindings, commandList);
        renderPass->Execute(executionContext);
        RestorePassResourceStates(nodeDefinition, portBindings, commandList);
        passTimer->RecordEndAndResolve(commandList, timingSlot);

        if (BatchLocalPasses)
        {
            LocalBatch.Lists.push_back(commandList);
            LocalBatch.Timers.push_back(passTimer);
            return;
        }

        const auto completionPoint = SubmitCommandList(commandQueue, commandList, frameResources);
        passTimer->OnCommandListExecuted(completionPoint.Value, timingSlot);
        TrackPassCompletion(nodeDefinition, portBindings, completionPoint, frameResources, nodeCompletions);
    }

    // Submit one frame; presentation and a final GPU wait remain the caller's responsibility.
    void ExecuteFrame()
    {
        WaitForCurrentFrameSlotResources();
        RebuildRequestedFrameSlotResources();
        auto& frameResources = FrameSlots[GetCurrentFrameSlotResourcesIndex()];
        ResetFrameSlotReadiness(frameResources);

        std::map<NodeId, GDX12CompletionPoint> nodeCompletions;
        for (const auto& nodeId : Plan.Order)
        {
            const auto& nodeDefinition = Definition.Nodes.at(nodeId);
            if (Imports.count(nodeId))
            {
                RefreshImportedTexture(nodeId, frameResources);
                continue;
            }
            RecordPass(nodeDefinition, frameResources, nodeCompletions);
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
    for (const auto& edge : runtime->Definition.Connections) { runtime->Sources[edge.Target] = edge.Source; }
    runtime->ResolveResourceRequirements();
    runtime->BuildFrameSlotResources();

    for (auto& entry : runtime->Passes)
    {
        const auto& node = runtime->Definition.Nodes.at(entry.first);
        PassInitContext context(runtime->GetDeviceResources(node.DeviceRole), _common, node, runtime->Plan, runtime->Sources);
        entry.second->Initialize(context);
        auto* device = runtime->GetDeviceResources(node.DeviceRole)->Device;
        auto timer = std::make_unique<GDX12GPUTimer>();
        timer->Initialize(device, device->GetCommandQueue(), runtime->GetPassTimingSlotCount());
        runtime->Timers[entry.first] = std::move(timer);
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
    _runtime->LastCompletedFrameTiming.reset();
}
uint32_t CompiledFrameGraph::GetCurrentFrameSlotResourcesIndex() const { return _runtime->GetCurrentFrameSlotResourcesIndex(); }
const FrameGraphPlan& CompiledFrameGraph::GetExecutionPlan() const { return _runtime->Plan; }
FrameGraphPerFrameMetrics CompiledFrameGraph::GetLatestFrameMetrics() const
{
    const auto& frame = _runtime->LastCompletedFrameTiming;
    if (!frame) { return {}; }
    const auto [primary, secondary] = _runtime->CalculateGPUFrameSpans(*frame);
    FrameGraphPerFrameMetrics metrics{primary.DurationMS(), secondary.DurationMS(), frame->CrossGPUCopies};
    const auto timingSlot = _runtime->GetPassTimingSlot(frame->FrameNumber);
    metrics.PassTimings.reserve(_runtime->Timers.size());
    for (const auto& id : _runtime->Plan.Order)
    {
        const auto timer = _runtime->Timers.find(id);
        if (timer == _runtime->Timers.end()) { continue; }
        const auto role = _runtime->Definition.Nodes.at(id).DeviceRole;
        metrics.PassTimings.push_back({id, role, timer->second->ReadResolvedTimeMS(timingSlot)});
    }
    return metrics;
}

std::string CompiledFrameGraph::GetDebugReport() const
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(6);
    out << "\nFRAME GRAPH INFO:";
    _runtime->WriteGPUFrameTiming(out);
    if (_runtime->Secondary)
    {
        out << "\n[Cross-GPU copies]\n"
            << "  Planned per frame: " << _runtime->Plan.CopyCount
            << " | Transport: " << (_runtime->Plan.UseCopyQueue ? "COPY" : "DIRECT") << '\n'
            << "  Submitted so far: " << _runtime->CopyCommands << " commands, " << _runtime->CopyBytes << " bytes\n";
        if (const auto& timing = _runtime->LastCompletedFrameTiming)
        {
            out << "  Frame " << timing->FrameNumber << " totals:\n";
            for (const auto role : {DEVICE_ROLE_PRIMARY, DEVICE_ROLE_SECONDARY})
            {
                const auto& device = timing->CopyTimes[role == DEVICE_ROLE_PRIMARY ? 0 : 1];
                out << "    " << GetDeviceRoleName(role) << ": ";
                if (device.Copies && !device.Measured) { out << "unavailable"; }
                else { out << device.TimeMS << " ms"; }
                if (device.Measured < device.Copies)
                {
                    out << " | WARNING: " << (device.Copies - device.Measured) << '/' << device.Copies
                        << " timers were unable to return a result. Total timings are incomplete.";
                }
                out << '\n';
            }
        }
        else { out << "  Frame totals: pending\n"; }

        if (!_runtime->TransportTimers.empty()) { out << "  Latest individual transfers:\n"; }
        for (auto& [queue, timers] : _runtime->TransportTimers)
        {
            const auto role = queue->GetCompletionPoint().Device->Role;
            const char* queueName = queue->GetType() == D3D12_COMMAND_LIST_TYPE_COPY ? "COPY" : "DIRECT";
            for (auto& [key, timer] : timers)
            {
                out << "    " << (key.second ? "Export " : "Import ") << key.first.Node << '.' << key.first.Pin
                    << " | " << GetDeviceRoleName(role) << '/' << queueName << " | ";
                const double ms = timer.GetTimeMS();
                if (!timer.IsAvailable()) { out << "unsupported"; }
                else if (ms < 0.0) { out << "pending"; }
                else { out << ms << " ms"; }
                out << '\n';
            }
        }
    }
    out << "\n[GPU pass timings]\n";
    for (const auto& id : _runtime->Plan.Order)
    {
        auto timer = _runtime->Timers.find(id);
        if (timer == _runtime->Timers.end()) { continue; }
        out << "  " << id << " | " << GetDeviceRoleName(_runtime->Definition.Nodes.at(id).DeviceRole) << "/DIRECT | ";
        const double ms = timer->second->GetTimeMS();
        if (ms < 0.0) { out << "pending"; }
        else { out << ms << " ms"; }
        out << '\n';
    }

    out << '\n' << _runtime->Plan.GetDebugReport(_runtime->Definition, _runtime->CompileData);

    bool hasPortOverrides = false;
    for (const auto& node : _runtime->Definition.Nodes)
    {
        for (const auto& pin : node.second.Description.Outputs)
        {
            const auto storage = pin.second.RequestedMemory;
            if (storage == MemoryType::Auto || (storage == MemoryType::Local && _runtime->Imports.count(node.first))) { continue; }
            if (!hasPortOverrides) { out << "\n[Port overrides]\n"; hasPortOverrides = true; }
            out << "  " << node.first << '.' << pin.first << " storage=" << GetMemoryTypeName(storage) << '\n';
        }
        for (const auto& pin : node.second.Description.Inputs)
        {
            if (pin.second.RequestedMemory == MemoryType::Auto) { continue; }
            if (!hasPortOverrides) { out << "\n[Port overrides]\n"; hasPortOverrides = true; }
            out << "  " << node.first << '.' << pin.first << " access=" << GetMemoryTypeName(pin.second.RequestedMemory) << '\n';
        }
    }
    if (!_runtime->Fallbacks.empty()) { out << "\n[Placement fallbacks]\n"; }
    for (const auto& fallback : _runtime->Fallbacks) { out << "  " << fallback << '\n'; }

    if (!_runtime->FrameSlots.empty())
    {
        out << "\n[Allocations per slot]\n";
        for (const auto& resource : _runtime->FrameSlots.front().Resources)
        {
            const auto& material = resource.second;
            if (!material.LocalResource && material.Shared.Allocations.empty() && material.LocalCopies.empty()) { continue; }
            out << "  " << resource.first.Node << '.' << resource.first.Pin;
            if (material.LocalResource) { out << " native bytes=" << material.LocalResource->AllocationBytes; }
            for (const auto& shared : material.Shared.Allocations) { out << " shared bytes=" << shared->GetHeapSize(); }
            for (const auto& replica : material.LocalCopies) { out << " replica=" << GetDeviceRoleName(replica.first) << " bytes=" << replica.second->AllocationBytes; }
            out << '\n';
            for (const auto* mapping : {material.Shared.Producer.get(), material.Shared.Consumer.get()})
            {
                if (!mapping) { continue; }
                for (size_t i = 0; i < mapping->Footprints.size(); ++i)
                {
                    const auto& footprint = mapping->Footprints[i];
                    out << "    subresource=" << i << " offset=" << footprint.Offset << " pitch=" << footprint.Footprint.RowPitch
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

const GDX12Device* CompiledFrameGraph::GetActiveDevice(EDeviceRole role) const
{
    return UsesDevice(role) ? _runtime->GetDeviceResources(role)->Device : nullptr;
}

uint32_t CompiledFrameGraph::GetDevicePassFlags(EDeviceRole role) const
{
    uint32_t flags = 0;
    for (const auto& entry : _runtime->Passes)
    { if (_runtime->Definition.Nodes.at(entry.first).DeviceRole == role) { flags |= entry.second->GetFlags(); } }
    return flags;
}

void CompiledFrameGraph::ResizeFrameSlotResources(UInt2 display)
{
    if (!display.x || !display.y) { throw std::invalid_argument("ERROR: Given display width or height is zero. Call ResizeFrameSlotResources() with non-zero display width and height."); }
    _runtime->WaitForGPUCompletion();
    _runtime->CompileData.DisplaySize = display;
    _runtime->ResolveResourceRequirements();
    _runtime->BuildFrameSlotResources();
    for (auto& entry : _runtime->Passes)
    {
        const auto& node = _runtime->Definition.Nodes.at(entry.first);
        PassInitContext context(_runtime->GetDeviceResources(node.DeviceRole), _runtime->Common, node, _runtime->Plan, _runtime->Sources);
        entry.second->OnResourcesRecreated(context);
    }
 
    _runtime->ResourceRebuildRequested = false;
}
