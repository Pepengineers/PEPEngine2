#include "Engine.RendererDX12/GDX12RenderGraph.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12Texture.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.RendererDX12/GDX12GPUTimer.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <algorithm>
#include <ostream>
#include <iomanip>
#include <queue>
#include <stdexcept>

using namespace GDX12Submission;

GDX12RenderGraph::GDX12RenderGraph(GDX12Device* device) : _device(device) {}
GDX12RenderGraph::~GDX12RenderGraph() = default;

const GDX12RenderGraph::Statistics& GDX12RenderGraph::GetStatistics() const
{
    double total = 0;
    bool resolved = false;
    for (const auto& [pass, timer] : _computeTimers)
        if (_nodes[pass].LastAsync)
        {
            const double time = timer->GetTimeMS();
            if (time < 0) { _statistics.ComputeTimeMS = -1; return _statistics; }
            total += time;
            resolved = true;
        }
    // Sum of COMPUTE pass durations, not a calibrated cross-queue interval.
    _statistics.ComputeTimeMS = resolved ? total : -1;
    return _statistics;
}

void GDX12RenderGraph::CheckResource(const Resource id) const
{
    if (id >= _resources.size()) throw std::out_of_range("Render graph: invalid resource");
}

void GDX12RenderGraph::CheckPass(const Pass id) const
{
    if (id >= _nodes.size()) throw std::out_of_range("Render graph: invalid pass");
}

GDX12RenderGraph::Resource GDX12RenderGraph::Create(std::string name, GDX12Texture* texture, const bool transient)
{
    if (transient && (!texture || !texture->IsInitialized() || !texture->CanShareGraphResource()))
        throw std::invalid_argument("Render graph: transient texture must be initialized and engine-owned: " + name);
    for (const auto& resource : _resources)
        if (texture && resource.Texture == texture)
            throw std::invalid_argument("Render graph: register each texture only once: " + name);
    _compiled = false;
    ResourceInfo resource;
    resource.Name = std::move(name);
    resource.Texture = texture;
    resource.Transient = transient;
    _resources.push_back(std::move(resource));
    return static_cast<Resource>(_resources.size() - 1);
}

GDX12RenderGraph::Resource GDX12RenderGraph::Import(std::string name, GDX12Texture* texture)
{
    const auto id = Create(std::move(name), texture);
    _resources[id].Imported = true;
    return id;
}

void GDX12RenderGraph::ExpandRead(const Resource id, std::vector<Resource>& result) const
{
    if (std::find(result.begin(), result.end(), id) != result.end()) return;
    result.push_back(id);
    for (auto fallback : _resources[id].Fallbacks) ExpandRead(fallback, result);
}

void GDX12RenderGraph::AddFallback(const Resource output, const Resource input)
{
    CheckResource(output); CheckResource(input);
    std::vector<Resource> reachable;
    ExpandRead(input, reachable);
    if (std::find(reachable.begin(), reachable.end(), output) != reachable.end())
        throw std::invalid_argument("Render graph: fallback cycle");
    _resources[output].Fallbacks.push_back(input);
    _compiled = false;
}

GDX12RenderGraph::Pass GDX12RenderGraph::AddPass(GDX12RenderPass& pass)
{
    for (const auto& node : _nodes)
        if (node.Instance == &pass) throw std::invalid_argument("Render graph: pass already enqueued");
    auto resources = _resources;
    auto links = _links;
    auto shared = _shared;
    Node node;
    node.Instance = &pass;
    GDX12RenderGraphBuilder builder(*this, node);
    try
    {
        pass.RecordRenderGraph(builder);
        if (node.Name.empty()) throw std::invalid_argument("Render graph: pass must declare a name");
    }
    catch (...)
    {
        _resources = std::move(resources);
        _links = std::move(links);
        _shared = std::move(shared);
        throw;
    }
    _nodes.push_back(std::move(node));
    _compiled = false;
    return static_cast<Pass>(_nodes.size() - 1);
}

void GDX12RenderGraph::ImportLink(std::string name, IRenderPassLink* link)
{
    if (!link || _links.count(link)) throw std::invalid_argument("Render graph: invalid or duplicate imported link");
    const auto resource = Import(std::move(name));
    _resources[resource].Link = link;
    _links.emplace(link, resource);
}

void GDX12RenderGraph::ImportShared(SharedResource resource, std::string name)
{
    if (_shared.count(resource)) throw std::invalid_argument("Render graph: duplicate shared import");
    const auto id = Import(std::move(name));
    _resources[id].LogicalToken = true;
    _shared.emplace(resource, id);
}

void GDX12RenderGraphBuilder::SetName(std::string name) { _node.Name = std::move(name); }
void GDX12RenderGraphBuilder::HasSideEffects() { _node.SideEffect = true; }
void GDX12RenderGraphBuilder::AsyncCompute() { _node.AsyncCompute = true; }

void GDX12RenderGraphBuilder::SetPlacement(const Placement location)
{
    if (!location.IsValid() || _node.HasPlacement)
        throw std::invalid_argument("Render graph: invalid or duplicate placement");
    _node.HasPlacement = true;
    _node.Location = location;
}

void GDX12RenderGraphBuilder::DeclareCompleteAccesses() { _node.CompleteAccesses = true; }

void GDX12RenderGraphBuilder::UseVersion(const Graph::Resource resource, const Graph::Mode mode,
    const TransferVersion version, const D3D12_RESOURCE_STATES state)
{
    if (!version.IsValid()) throw std::invalid_argument("Render graph: invalid transfer version");
    for (const auto& access : _node.Accesses)
        if (access.Id == resource)
            throw std::invalid_argument("Render graph: duplicate versioned access");
    Use(resource, mode, state);
    _node.Accesses.back().Version = version;
}

void GDX12RenderGraphBuilder::Use(const Graph::Resource resource, const Graph::Mode mode, const D3D12_RESOURCE_STATES state)
{
    _graph.CheckResource(resource);
    if (mode != Graph::Mode::Read && mode != Graph::Mode::Write && mode != Graph::Mode::ReadWrite)
        throw std::invalid_argument("Render graph: invalid access mode");
    for (const auto& access : _node.Accesses)
        if (access.Id == resource)
        {
            if (access.Usage == Graph::Mode::Read && mode == Graph::Mode::Read && access.State == state) return;
            throw std::invalid_argument("Render graph: conflicting access; declare ReadWrite once: " + _node.Name);
        }
    _node.Accesses.push_back({ resource, mode, state });
}

GDX12RenderGraph::Resource GDX12RenderGraphBuilder::Find(IRenderPassLink* link) const
{
    const auto found = _graph._links.find(link);
    if (!link || found == _graph._links.end())
        throw std::invalid_argument("Render graph: input has no producer or import: " + _node.Name);
    return found->second;
}

void GDX12RenderGraphBuilder::Read(IRenderPassLink* link, const D3D12_RESOURCE_STATES state)
{ Use(Find(link), Graph::Mode::Read, state); }

void GDX12RenderGraphBuilder::ReadInputs(const std::vector<IRenderPassLink*>& inputs, const D3D12_RESOURCE_STATES state)
{ for (auto* input : inputs) Read(input, state); }

void GDX12RenderGraphBuilder::ReadWrite(IRenderPassLink* link, const D3D12_RESOURCE_STATES state)
{ Use(Find(link), Graph::Mode::ReadWrite, state); }

void GDX12RenderGraphBuilder::Write(IRenderPassLink* link, std::string name, const Lifetime lifetime,
    const D3D12_RESOURCE_STATES state, GDX12Texture* storage)
{
    if (!link) throw std::invalid_argument("Render graph: null output: " + _node.Name);
    auto found = _graph._links.find(link);
    if (found == _graph._links.end())
    {
        auto* texture = lifetime == Lifetime::Dynamic ? nullptr : (storage ? storage : link->GetTexture());
        const auto id = _graph.Create(std::move(name), texture, lifetime == Lifetime::Transient);
        _graph._resources[id].Link = link;
        found = _graph._links.emplace(link, id).first;
    }
    Use(found->second, Graph::Mode::Write, state);
}

void GDX12RenderGraphBuilder::Read(const SharedResource resource)
{
    const auto found = _graph._shared.find(resource);
    if (found == _graph._shared.end()) throw std::invalid_argument("Render graph: shared input has no producer: " + _node.Name);
    Use(found->second, Graph::Mode::Read);
}

void GDX12RenderGraphBuilder::Write(SharedResource resource, std::string name)
{
    auto found = _graph._shared.find(resource);
    if (found == _graph._shared.end())
    {
        const auto id = _graph.Create(std::move(name));
        _graph._resources[id].LogicalToken = true;
        found = _graph._shared.emplace(resource, id).first;
    }
    Use(found->second, Graph::Mode::Write);
}

void GDX12RenderGraphBuilder::Fallback(IRenderPassLink* output, IRenderPassLink* input)
{ _graph.AddFallback(Find(output), Find(input)); }

void GDX12RenderGraphBuilder::Export(IRenderPassLink* link) { _graph.Export(Find(link)); }

void GDX12RenderGraph::DependsOn(const Pass pass, const Pass prerequisite)
{
    CheckPass(pass); CheckPass(prerequisite);
    _nodes[pass].ExplicitDependencies.push_back(prerequisite);
    _compiled = false;
}

void GDX12RenderGraph::Export(const Resource resource)
{
    CheckResource(resource);
    _resources[resource].Exported = true;
    _compiled = false;
}

void GDX12RenderGraph::RegisterResource(const Resource resource, const ResourceIdentity identity, const bool crossAdapter)
{
    CheckResource(resource);
    if (identity.Id == ResourceId::Invalid || identity.Adapter == AdapterId::Invalid ||
        identity.ResourceEpoch == Epoch::Invalid ||
        ((crossAdapter || identity.Transfer != TransferId::Invalid) && identity.Allocation == AllocationId::Invalid))
        throw std::invalid_argument("Render graph: invalid resource identity or missing cross-adapter allocation");
    for (const auto& entry : _resources)
        if (entry.Registered && entry.Identity.Id == identity.Id)
            throw std::invalid_argument("Render graph: duplicate resource view identity");
    auto& entry = _resources[resource];
    if (entry.Registered || entry.LogicalToken)
        throw std::invalid_argument("Render graph: resource already registered or is a pipeline token");
    entry.Registered = true;
    entry.CrossAdapter = crossAdapter;
    entry.Identity = identity;
    _compiled = false;
}

void GDX12RenderGraph::ImportVersion(const Resource resource, const TransferVersion version)
{
    CheckResource(resource);
    auto& entry = _resources[resource];
    if (!entry.Imported || !entry.Registered || !version.IsValid() ||
        version.Transfer != entry.Identity.Transfer || version.VersionEpoch != entry.Identity.ResourceEpoch)
        throw std::invalid_argument("Render graph: invalid imported version");
    entry.ImportedVersion = version;
    _compiled = false;
}

const GDX12RenderGraph::CompiledPlan& GDX12RenderGraph::GetCompiledPlan() const
{
    if (!_compiled) throw std::logic_error("Render graph: plan is not compiled");
    return _plan;
}

void GDX12RenderGraph::Compile(const bool reuseTextures) { CompileInternal(reuseTextures, true); }
void GDX12RenderGraph::CompilePlan() { CompileInternal(false, false); }

void GDX12RenderGraph::CompileInternal(bool reuseTextures, const bool allocateTextures)
{
    _compiled = false;
    _resourcesReady = false;
    _plan = {};
    _order.clear();
    _statistics = {};
    std::vector<Pass> writers(_resources.size(), UINT_MAX);
    std::vector<std::vector<Pass>> readers(_resources.size());
    struct AllocationUse { Pass Writer = UINT_MAX; std::vector<Pass> Readers; TransferVersion Version{}; };
    std::unordered_map<AllocationId, AllocationUse> allocations;
    std::vector<TransferVersion> versions;
    for (const auto& resource : _resources) versions.push_back(resource.ImportedVersion);
    for (Pass p = 0; p < _nodes.size(); ++p)
    {
        auto& node = _nodes[p];
        node.Dependencies = node.ExplicitDependencies;
        if (node.HasPlacement)
        {
            if (!node.Location.IsValid() || !node.CompleteAccesses ||
                !node.Instance->SupportsGraphQueue(node.Location.Queue) || !node.Instance->GetBufferAccesses().empty())
                throw std::logic_error("Render graph: incomplete or unsupported placement contract: " + node.Name);
            if (node.AsyncCompute && node.Location.Queue != QueueKind::Compute)
                throw std::logic_error("Render graph: conflicting legacy async and explicit placement");
        }
        for (const auto& access : node.Accesses)
        {
            const auto& resource = _resources[access.Id];
            if (node.HasPlacement && !resource.LogicalToken)
            {
                if (!resource.Registered || resource.Identity.Adapter != node.Location.Adapter || access.State == UnmanagedState)
                    throw std::logic_error("Render graph: unmanaged or wrong-device access: " + resource.Name);
                if (node.Location.Queue == QueueKind::Copy &&
                    !((access.Usage == Mode::Read && access.State == D3D12_RESOURCE_STATE_COPY_SOURCE) ||
                      (access.Usage == Mode::Write && access.State == D3D12_RESOURCE_STATE_COPY_DEST)))
                    throw std::logic_error("Render graph: illegal COPY access: " + resource.Name);
                const auto computeStates = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                    D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_COPY_DEST;
                if (node.Location.Queue == QueueKind::Compute && (access.State & ~computeStates) != 0)
                    throw std::logic_error("Render graph: illegal COMPUTE state: " + resource.Name);
                const auto writeStates = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                    D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_STREAM_OUT |
                    D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST;
                const auto writeState = access.State & writeStates;
                if ((access.Usage != Mode::Read && writeState == 0) ||
                    (access.Usage == Mode::Read && (writeState & ~D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0) ||
                    (writeState != 0 && (access.State != writeState ||
                        (static_cast<UINT>(writeState) & (static_cast<UINT>(writeState) - 1)) != 0)))
                    throw std::logic_error("Render graph: resource state conflicts with access mode: " + resource.Name);
            }
            if (resource.Registered && !node.HasPlacement)
                throw std::logic_error("Render graph: registered physical view requires explicit placement");
            if (access.Version.IsValid())
            {
                if (!resource.Registered || access.Version.Transfer != resource.Identity.Transfer ||
                    access.Version.VersionEpoch != resource.Identity.ResourceEpoch)
                    throw std::logic_error("Render graph: version does not match resource: " + resource.Name);
            }
            else if (resource.Registered && resource.Identity.Transfer != TransferId::Invalid)
                throw std::logic_error("Render graph: transfer access requires an exact version");
        }
        const auto depend = [&](const Pass producer)
        {
            if (producer != UINT_MAX && producer != p) node.Dependencies.push_back(producer);
        };
        // Backing-allocation hazards ignore device, stream and epoch: none retires old readers.
        // Merge aliases in a single pass before updating the history (no self-dependencies).
        std::unordered_map<AllocationId, Mode> allocationModes;
        std::unordered_map<AllocationId, TransferVersion> writtenVersions;
        for (const auto& access : node.Accesses)
        {
            std::vector<Resource> used;
            if (access.Usage != Mode::Write) ExpandRead(access.Id, used);
            else used.push_back(access.Id);
            const auto writtenAllocation = _resources[access.Id].Identity.Allocation;
            if (access.Usage != Mode::Read && writtenAllocation != AllocationId::Invalid)
            {
                const auto inserted = writtenVersions.emplace(writtenAllocation, access.Version);
                if (!inserted.second && !(inserted.first->second == access.Version))
                    throw std::logic_error("Render graph: incompatible versions written to one allocation in a pass: " + node.Name);
            }
            for (auto id : used)
            {
                const auto& resource = _resources[id];
                if (node.HasPlacement && !resource.LogicalToken &&
                    (!resource.Registered || resource.Identity.Adapter != node.Location.Adapter))
                    throw std::logic_error("Render graph: unmanaged or wrong-device fallback");
                const auto allocation = resource.Identity.Allocation;
                if (allocation != AllocationId::Invalid)
                {
                    const auto mode = id == access.Id ? access.Usage : Mode::Read;
                    auto result = allocationModes.emplace(allocation, mode);
                    if (!result.second && result.first->second != mode) result.first->second = Mode::ReadWrite;
                }
            }
            if (access.Usage != Mode::Write && access.Version.IsValid())
                for (auto id : used)
                {
                    const auto& resource = _resources[id];
                    auto current = versions[id];
                    const auto allocation = resource.Identity.Allocation;
                    if (allocation != AllocationId::Invalid && allocations[allocation].Writer != UINT_MAX)
                        current = allocations[allocation].Version;
                    if (resource.Identity.Transfer != access.Version.Transfer ||
                        resource.Identity.ResourceEpoch != access.Version.VersionEpoch || !(current == access.Version))
                        throw std::logic_error("Render graph: version has no matching producer/import (including fallback): " + node.Name);
                }
        }
        std::unordered_map<AllocationId, Pass> priorAllocationWriters;
        for (const auto& [allocation, mode] : allocationModes)
        {
            auto& history = allocations[allocation];
            priorAllocationWriters[allocation] = history.Writer;
            depend(history.Writer);
            if (mode != Mode::Read) for (auto reader : history.Readers) depend(reader);
            if (mode != Mode::Read) { history.Readers.clear(); history.Writer = p; history.Version = {}; }
            else history.Readers.push_back(p);
        }
        for (const auto& access : node.Accesses)
            if (access.Usage != Mode::Read)
            {
                versions[access.Id] = access.Version;
                const auto allocation = _resources[access.Id].Identity.Allocation;
                if (allocation != AllocationId::Invalid) allocations[allocation].Version = access.Version;
            }
        // Reads precede writes, including conditional forwarding through other outputs.
        for (const auto& access : node.Accesses)
            if (access.Usage != Mode::Write)
            {
                std::vector<Resource> inputs;
                ExpandRead(access.Id, inputs);
                for (auto id : inputs)
                {
                    const auto allocation = _resources[id].Identity.Allocation;
                    const bool aliasWriter = allocation != AllocationId::Invalid &&
                        priorAllocationWriters.count(allocation) && priorAllocationWriters.at(allocation) != UINT_MAX;
                    if (writers[id] == UINT_MAX && !_resources[id].Imported && !aliasWriter)
                        throw std::logic_error("Render graph: " + node.Name + " reads before first write: " + _resources[id].Name);
                    depend(writers[id]);
                    readers[id].push_back(p);
                }
            }
        for (const auto& access : node.Accesses)
            if (access.Usage != Mode::Read)
            {
                depend(writers[access.Id]);
                for (auto reader : readers[access.Id]) depend(reader);
                readers[access.Id].clear();
                writers[access.Id] = p;
            }
        std::sort(node.Dependencies.begin(), node.Dependencies.end());
        node.Dependencies.erase(std::unique(node.Dependencies.begin(), node.Dependencies.end()), node.Dependencies.end());
    }
    std::vector<UINT> indegree(_nodes.size());
    std::vector<std::vector<Pass>> dependents(_nodes.size());
    std::priority_queue<Pass, std::vector<Pass>, std::greater<Pass>> ready;
    for (Pass p = 0; p < _nodes.size(); ++p)
    {
        indegree[p] = static_cast<UINT>(_nodes[p].Dependencies.size());
        for (auto predecessor : _nodes[p].Dependencies) dependents[predecessor].push_back(p);
        if (!indegree[p]) ready.push(p);
    }
    std::vector<Pass> sorted;
    while (!ready.empty())
    {
        auto p = ready.top(); ready.pop();
        sorted.push_back(p);
        for (auto next : dependents[p]) if (--indegree[next] == 0) ready.push(next);
    }
    if (sorted.size() != _nodes.size())
    {
        std::string message = "Render graph dependency cycle:";
        for (Pass p = 0; p < _nodes.size(); ++p) if (indegree[p]) message += " " + _nodes[p].Name;
        throw std::logic_error(message);
    }
    std::vector<bool> live(_nodes.size());
    std::function<void(Pass)> retain = [&](const Pass p)
    {
        if (p == UINT_MAX || live[p]) return;
        live[p] = true;
        for (auto predecessor : _nodes[p].Dependencies) retain(predecessor);
    };
    for (Pass p = 0; p < _nodes.size(); ++p)
    {
        if (_nodes[p].SideEffect) retain(p);
        for (auto access : _nodes[p].Accesses)
            if (access.Usage != Mode::Read && _resources[access.Id].Imported) retain(p);
    }
    for (Resource r = 0; r < _resources.size(); ++r)
        if (_resources[r].Exported)
        {
            std::vector<Resource> outputs;
            ExpandRead(r, outputs);
            for (auto id : outputs)
            {
                auto writer = writers[id];
                const auto allocation = _resources[id].Identity.Allocation;
                if (allocation != AllocationId::Invalid && allocations[allocation].Writer != UINT_MAX)
                    writer = allocations[allocation].Writer;
                if (writer == UINT_MAX && !_resources[id].Imported)
                    throw std::logic_error("Render graph: exported resource has no writer: " + _resources[id].Name);
                retain(writer);
            }
        }
    for (auto p : sorted) if (live[p]) _order.push_back(p);
    BuildPlan();
    _statistics.Passes = static_cast<UINT>(_order.size());
    _statistics.CulledPasses = static_cast<UINT>(_nodes.size() - _order.size());
    for (auto& r : _resources) { r.First = UINT_MAX; r.Last = 0; r.Slot = UINT_MAX; }
    for (UINT i = 0; i < _order.size(); ++i)
        for (auto access : _nodes[_order[i]].Accesses)
        {
            std::vector<Resource> used;
            if (access.Usage != Mode::Write) ExpandRead(access.Id, used);
            else used.push_back(access.Id);
            for (auto id : used)
            {
                auto& r = _resources[id];
                r.First = (std::min)(r.First, i);
                r.Last = (std::max)(r.Last, i);
            }
        }
    for (Resource r = 0; r < _resources.size(); ++r)
        if (_resources[r].Exported)
        {
            std::vector<Resource> outputs;
            ExpandRead(r, outputs);
            for (auto id : outputs) _resources[id].Last = static_cast<UINT>(_order.size());
        }
    for (auto p : _order)
        if (_nodes[p].AsyncCompute || _nodes[p].HasPlacement)
            for (const auto& access : _nodes[p].Accesses)
            {
                std::vector<Resource> used;
                ExpandRead(access.Id, used);
                for (auto id : used)
                {
                    _resources[id].First = 0;
                    _resources[id].Last = static_cast<UINT>(_order.size());
                }
            }
    // Explicit plans own per-device lifetimes in P2b; never pool/rebind them on the legacy device.
    if (!allocateTextures || _plan.RequiresNewExecutor)
    {
        _compiled = true;
        return;
    }
    std::vector<Resource> textures;
    for (Resource r = 0; r < _resources.size(); ++r)
        if (_resources[r].Transient && _resources[r].First != UINT_MAX) textures.push_back(r);
    std::stable_sort(textures.begin(), textures.end(), [&](const Resource a, const Resource b) { return _resources[a].First < _resources[b].First; });
    if (!textures.empty())
    {
        if (!_device) throw std::logic_error("Render graph: pooling requires a device");
        _device->GetCommandQueue()->Flush();
    }
    _slots.clear();
    for (auto id : textures)
    {
        auto& resource = _resources[id];
        const auto desc = resource.Texture->GetResource()->D3DResource->GetDesc();
        const UINT64 bytes = _device->GetDevice()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        _statistics.LogicalBytes += bytes;
        ++_statistics.TransientTextures;
        if (reuseTextures)
            for (UINT slot = 0; slot < _slots.size(); ++slot)
                if (_slots[slot].Last < resource.First &&
                    resource.Texture->CanShareGraphResource(*_resources[_slots[slot].Description].Texture))
                { resource.Slot = slot; break; }
        if (resource.Slot == UINT_MAX)
        {
            resource.Slot = static_cast<UINT>(_slots.size());
            auto physical = resource.Texture->_resource;
            const bool alreadyAssigned = std::any_of(_slots.begin(), _slots.end(), [&](const Slot& slot)
            { return slot.ResourceObject->D3DResource.Get() == physical->D3DResource.Get(); });
            if (alreadyAssigned) physical = resource.Texture->CreateGraphResource();
            _slots.push_back({ id, resource.Last, std::move(physical) });
            _statistics.PhysicalBytes += bytes;
        }
        _slots[resource.Slot].Last = resource.Last;
    }
    // Choose distinct slots before rebinding; recompiling a previously pooled graph may need to split a slot.
    for (auto id : textures) _resources[id].Texture->BindGraphResource(_slots[_resources[id].Slot].ResourceObject);
    _statistics.PhysicalTextures = static_cast<UINT>(_slots.size());
    _resourcesReady = true;
    _compiled = true;
}

void GDX12RenderGraph::BuildPlan()
{
    std::vector<bool> live(_nodes.size());
    for (auto p : _order) live[p] = true;
    std::vector<std::pair<Pass, Pass>> edges;
    for (auto p : _order)
        for (auto producer : _nodes[p].Dependencies) edges.emplace_back(producer, p);
    // Explicit queues have declared enqueue order. A backward wait cannot jump FIFO.
    // Legacy passes keep the previous topological scheduling contract.
    std::vector<std::pair<Placement, Pass>> tails;
    for (auto p : _order)
        if (!_nodes[p].HasPlacement)
        {
            const Placement location{ AdapterId::Invalid, _nodes[p].AsyncCompute ? QueueKind::Compute : QueueKind::Direct };
            auto tail = std::find_if(tails.begin(), tails.end(), [&](const auto& entry) { return entry.first == location; });
            if (tail == tails.end()) tails.emplace_back(location, p);
            else { edges.emplace_back(tail->second, p); tail->second = p; }
        }
    for (Pass p = 0; p < _nodes.size(); ++p)
        if (live[p] && _nodes[p].HasPlacement)
        {
            const auto location = _nodes[p].Location;
            auto tail = std::find_if(tails.begin(), tails.end(), [&](const auto& entry) { return entry.first == location; });
            if (tail == tails.end()) tails.emplace_back(location, p);
            else { edges.emplace_back(tail->second, p); tail->second = p; }
        }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    std::vector<UINT> indegree(_nodes.size());
    std::vector<std::vector<Pass>> next(_nodes.size());
    for (const auto& [producer, consumer] : edges)
    {
        next[producer].push_back(consumer);
        ++indegree[consumer];
    }
    std::priority_queue<Pass, std::vector<Pass>, std::greater<Pass>> ready;
    for (auto p : _order) if (!indegree[p]) ready.push(p);
    std::vector<Pass> sorted;
    while (!ready.empty())
    {
        const auto p = ready.top(); ready.pop(); sorted.push_back(p);
        for (auto consumer : next[p]) if (--indegree[consumer] == 0) ready.push(consumer);
    }
    if (sorted.size() != _order.size())
        throw std::logic_error("Render graph: expanded wait/FIFO dependency cycle");
    _order = std::move(sorted);
    std::vector<PlannedBatchId> batches(_nodes.size(), PlannedBatchId::Invalid);
    std::vector<bool> written(_resources.size());
    std::unordered_map<AllocationId, bool> allocationWritten;
    for (Resource r = 0; r < _resources.size(); ++r)
    {
        const auto& entry = _resources[r];
        _plan.Views.push_back({ r, entry.Identity, entry.Imported, entry.CrossAdapter, entry.LogicalToken, entry.Fallbacks });
    }
    for (auto p : _order)
    {
        const auto& node = _nodes[p];
        const auto id = static_cast<PlannedBatchId>(_plan.Batches.size());
        batches[p] = id;
        Placement location = node.Location;
        if (!node.HasPlacement) location.Queue = node.AsyncCompute ? QueueKind::Compute : QueueKind::Direct;
        // One pass per batch preserves every possible wait boundary; P2b can coalesce safely.
        _plan.Batches.push_back({ id, location, !node.HasPlacement, { p }, node.Accesses });
        _plan.RequiresNewExecutor |= node.HasPlacement;
        for (const auto& access : node.Accesses)
            if (access.Usage != Mode::Write && access.Version.IsValid())
            {
                std::vector<Resource> inputs;
                ExpandRead(access.Id, inputs);
                for (auto view : inputs)
                {
                    const auto allocation = _resources[view].Identity.Allocation;
                    if (written[view] || (allocation != AllocationId::Invalid && allocationWritten[allocation])) continue;
                    const bool duplicate = std::any_of(_plan.Inputs.begin(), _plan.Inputs.end(), [&](const VersionInput& input)
                    { return input.View == view && input.Version == access.Version && input.Consumer == id; });
                    if (!duplicate) _plan.Inputs.push_back({ view, access.Version, id });
                }
            }
        for (const auto& access : node.Accesses)
            if (access.Usage != Mode::Read)
            {
                written[access.Id] = true;
                const auto allocation = _resources[access.Id].Identity.Allocation;
                if (allocation != AllocationId::Invalid) allocationWritten[allocation] = true;
            }
    }
    for (const auto& [producer, consumer] : edges)
        _plan.Dependencies.push_back({ batches[producer], batches[consumer] });
    std::sort(_plan.Dependencies.begin(), _plan.Dependencies.end(), [](const auto& a, const auto& b)
    { return a.Producer == b.Producer ? a.Consumer < b.Consumer : a.Producer < b.Producer; });
}

GDX12CommandList* GDX12RenderGraph::Execute(GDX12CommandList* commands, GDX12CommandQueue* computeQueue)
{
    GDX12_SCOPED_CPU_EVENT("Graph recording");
    if (!_compiled) throw std::logic_error("Render graph must be compiled before execution");
    if (_plan.RequiresNewExecutor)
        throw std::logic_error("Render graph: explicit placement/version plan requires P2b executor; legacy Execute supports primary DIRECT/COMPUTE only");
    if (!_resourcesReady) throw std::logic_error("Render graph: CompilePlan does not prepare native resources; call Compile for legacy Execute");
    for (const auto& resource : _resources)
        if (resource.Slot != UINT_MAX && resource.Texture->GetResource() != _slots[resource.Slot].ResourceObject.get())
            throw std::logic_error("Render graph texture changed; recompile after resize: " + resource.Name);
    if (computeQueue && (!_device || computeQueue->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE))
        throw std::invalid_argument("Render graph requires a COMPUTE queue on its device");
    auto* directQueue = _device ? _device->GetCommandQueue() : nullptr;
    struct TextureAccess { GDX12Resource* Resource; Mode Usage; };
    struct Pending
    {
        Pass Id; UINT64 Fence;
        std::vector<BufferAccess> Buffers;
        std::vector<TextureAccess> Textures;
    };
    std::vector<Pending> pending;
    std::vector<GDX12Resource*> ownedReads;
    _statistics.AsyncPasses = _statistics.AsyncJoins = 0;
    _statistics.ForkFence = _statistics.ComputeFence = 0;
    _statistics.JoinBefore.clear();
    _statistics.ComputeTimeMS = -1;
    for (auto& node : _nodes)
    {
        node.LastAsync = false;
        node.LastForkFence = node.LastComputeFence = 0;
        node.LastJoinBefore.clear();
    }
    const auto heapType = [](GDX12Resource* resource)
    {
        D3D12_HEAP_PROPERTIES properties = {};
        ThrowIfFailed(resource->D3DResource->GetHeapProperties(&properties, nullptr));
        return properties.Type;
    };
    const auto sharedBufferRead = [&](const BufferAccess& access)
    {
        return access.Usage == Mode::Read && access.State == D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER &&
            heapType(access.Resource) == D3D12_HEAP_TYPE_DEFAULT;
    };
    const auto releaseBuffers = [&](GDX12CommandList* list, const std::vector<BufferAccess>& buffers)
    {
        for (const auto& access : buffers)
            if (heapType(access.Resource) == D3D12_HEAP_TYPE_DEFAULT && !sharedBufferRead(access))
                list->ResourceBarrier({ access.Resource->GetCommonBarrier() });
    };
    const auto textureFor = [&](const Access& access) -> GDX12Texture*
    {
        const auto& entry = _resources[access.Id];
        return entry.Link && (access.Usage == Mode::Read || !entry.Texture) ? entry.Link->GetTexture() : entry.Texture;
    };
    const auto join = [&](size_t count, const std::string& before)
    {
        if (!count) return;
        directQueue->ExecuteCommandList(commands);
        directQueue->WaitForQueue(*computeQueue, pending[count - 1].Fence);
        commands = directQueue->GetCommandList();
        _statistics.JoinBefore = before;
        ++_statistics.AsyncJoins;
        for (size_t i = 0; i < count; ++i) _nodes[pending[i].Id].LastJoinBefore = before;
        pending.erase(pending.begin(), pending.begin() + count);
        for (auto it = ownedReads.begin(); it != ownedReads.end();)
        {
            bool retained = false;
            for (const auto& producer : pending)
            {
                for (const auto& access : producer.Textures)
                    retained |= access.Resource == *it && access.Usage == Mode::Read;
                for (const auto& access : producer.Buffers)
                    retained |= access.Resource == *it && sharedBufferRead(access);
            }
            if (retained) ++it;
            else { (*it)->EndConcurrentRead(); it = ownedReads.erase(it); }
        }
    };
    try
    {
        for (UINT i = 0; i < _order.size(); ++i)
        {
            const auto p = _order[i];
            auto& node = _nodes[p];
            const auto buffers = node.Instance->GetBufferAccesses();
            node.LastAsync = computeQueue && node.AsyncCompute && node.Instance->IsAsyncComputeEnabled();
            node.LastBuffers.clear();
            for (const auto& access : buffers)
                node.LastBuffers.emplace_back(reinterpret_cast<uintptr_t>(access.Resource->D3DResource.Get()), access.Usage);
            size_t required = 0;
            for (size_t j = 0; j < pending.size(); ++j)
            {
                const auto& producer = pending[j];
                bool conflict = !node.LastAsync && std::find(node.Dependencies.begin(), node.Dependencies.end(), producer.Id) != node.Dependencies.end();
                for (const auto& access : buffers)
                    for (const auto& active : producer.Buffers)
                        if (access.Resource->D3DResource.Get() == active.Resource->D3DResource.Get())
                        {
                            const bool shared = access.Resource == active.Resource && sharedBufferRead(access) && sharedBufferRead(active);
                            if (!node.LastAsync)
                                conflict |= access.Usage != Mode::Read || active.Usage != Mode::Read ||
                                    (heapType(access.Resource) != D3D12_HEAP_TYPE_UPLOAD && !shared);
                            else if (sharedBufferRead(access) || sharedBufferRead(active))
                                conflict |= !shared;
                        }
                for (const auto& access : node.Accesses)
                    if (auto* texture = textureFor(access))
                        for (const auto& active : producer.Textures)
                            if (texture->GetResource()->D3DResource.Get() == active.Resource->D3DResource.Get())
                            {
                                if (active.Usage != Mode::Read)
                                    // Read layouts are published on DIRECT, so a prior writer must finish first.
                                    conflict |= !node.LastAsync || access.Usage == Mode::Read;
                                else
                                    conflict |= texture->GetResource() != active.Resource || access.Usage != Mode::Read ||
                                        access.State == UnmanagedState || access.State == D3D12_RESOURCE_STATE_COMMON ||
                                        (access.State & active.Resource->GetCurrentState()) != access.State;
                            }
                if (conflict) required = j + 1;
            }
            join(required, node.Name);
            for (const auto& resource : _resources)
                if (resource.Slot != UINT_MAX && resource.First == i)
                {
                    auto* physical = resource.Texture->GetResource();
                    if (physical->D3DResource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
                        commands->ResourceBarrier({ physical->GetUAVBarrier() });
                }
            for (auto access : node.Accesses)
            {
                auto* texture = textureFor(access);
                if (!texture || access.State == UnmanagedState) continue;
                auto* resource = texture->GetResource();
                if (node.LastAsync && access.Usage != Mode::Read)
                {
                    if (access.State != D3D12_RESOURCE_STATE_UNORDERED_ACCESS ||
                        !(resource->D3DResource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
                        throw std::logic_error("Async texture writes require an explicit UAV output: " + node.Name);
                    commands->ResourceBarrier({ resource->GetCommonBarrier() });
                    continue;
                }
                if (node.LastAsync)
                {
                    if (access.State == D3D12_RESOURCE_STATE_COMMON)
                        throw std::logic_error("Async texture reads require an explicit non-COMMON state: " + node.Name);
                    // Publish a shared read layout on DIRECT; neither reader may narrow it before join.
                    access.State |= D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                    if (resource->D3DResource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
                        access.State |= D3D12_RESOURCE_STATE_DEPTH_READ;
                }
                commands->ResourceBarrier({ resource->GetTransitionBarrier(access.State) });
            }
            const auto record = [&](GDX12CommandList* list)
            {
                GDX12_SCOPED_EVENT(list, node.Name.c_str());
                node.Instance->Execute(list);
            };
            if (!node.LastAsync) { record(commands); continue; }
            std::vector<TextureAccess> textures;
            for (const auto& access : node.Accesses)
                if (auto* texture = textureFor(access))
                {
                    if (access.State == UnmanagedState)
                        throw std::logic_error("Async textures require an explicit state: " + node.Name);
                    auto* resource = texture->GetResource();
                    textures.push_back({ resource, access.Usage });
                    if (access.Usage != Mode::Read) continue;
                    if (std::find(ownedReads.begin(), ownedReads.end(), resource) == ownedReads.end())
                    {
                        resource->BeginConcurrentRead();
                        ownedReads.push_back(resource);
                    }
                }
            for (const auto& access : buffers)
                if (sharedBufferRead(access))
                {
                    commands->ResourceBarrier({ access.Resource->GetTransitionBarrier(access.State) });
                    if (std::find(ownedReads.begin(), ownedReads.end(), access.Resource) == ownedReads.end())
                    {
                        access.Resource->BeginConcurrentRead();
                        ownedReads.push_back(access.Resource);
                    }
                }
            releaseBuffers(commands, buffers);
            auto& timer = _computeTimers[p];
            if (!timer) timer = std::make_unique<GDX12GPUTimer>(_device, computeQueue);
            directQueue->ExecuteCommandList(commands);
            computeQueue->WaitForQueue(*directQueue, commands->FenceValue);
            _statistics.ForkFence = commands->FenceValue;
            node.LastForkFence = commands->FenceValue;
            commands = directQueue->GetCommandList();
            auto* compute = computeQueue->GetCommandList();
            timer->Start(compute);
            for (const auto& access : textures)
                if (access.Usage != Mode::Read)
                    compute->ResourceBarrier({ access.Resource->GetUnorderedAccessBarrier() });
            record(compute);
            for (const auto& access : textures)
                if (access.Usage != Mode::Read)
                    compute->ResourceBarrier({ access.Resource->GetCommonBarrier() });
            releaseBuffers(compute, buffers);
            timer->Stop(compute);
            computeQueue->ExecuteCommandList(compute);
            const auto computeFence = compute->FenceValue;
            _statistics.ComputeFence = computeFence;
            node.LastComputeFence = computeFence;
            timer->OnCommandListExecuted(computeFence);
            pending.push_back({ p, computeFence, buffers, std::move(textures) });
            ++_statistics.AsyncPasses;
        }
        join(pending.size(), "frame retirement");
    }
    catch (...)
    {
        if (computeQueue) computeQueue->Flush();
        if (directQueue)
        {
            directQueue->Flush();
        }
        for (auto* texture : ownedReads) texture->EndConcurrentRead();
        throw;
    }
    return commands;
}

void GDX12RenderGraph::WriteDot(std::ostream& output) const
{
    if (!_compiled) throw std::logic_error("Render graph must be compiled before export");
    output << "digraph RenderGraph {\n";
    if (_statistics.AsyncPasses)
        output << "  label=" << std::quoted("DIRECT fork fence=" + std::to_string(_statistics.ForkFence) +
            "; COMPUTE fence=" + std::to_string(_statistics.ComputeFence) + "; join before " + _statistics.JoinBefore) << ";\n";
    for (auto p : _order)
    {
        std::string queueLabel;
        if (_nodes[p].HasPlacement)
        {
            const auto location = _nodes[p].Location;
            queueLabel = " [adapter=" + std::to_string(static_cast<std::uint64_t>(location.Adapter)) + " " +
                (location.Queue == QueueKind::Copy ? "COPY" : (location.Queue == QueueKind::Compute ? "COMPUTE" : "DIRECT")) + "]";
        }
        else
            queueLabel = _nodes[p].LastAsync ? " [COMPUTE; fork=" + std::to_string(_nodes[p].LastForkFence) +
                "; signal=" + std::to_string(_nodes[p].LastComputeFence) + "; join before " + _nodes[p].LastJoinBefore + "]" :
                (_nodes[p].AsyncCompute ? " [DIRECT; COMPUTE eligible]" : " [DIRECT]");
        output << "  p" << p << " [label=" << std::quoted(_nodes[p].Name + queueLabel) << "];\n";
        for (const auto& buffer : _nodes[p].LastBuffers)
            output << "  p" << p << " -> b" << buffer.first << " [label=" << std::quoted(
                buffer.second == Mode::Read ? "read" : (buffer.second == Mode::Write ? "write" : "read/write")) << "];\n";
    }
    for (const auto& edge : _plan.Dependencies)
        output << "  p" << _plan.Batches[static_cast<UINT>(edge.Producer)].Passes.front() << " -> p" <<
            _plan.Batches[static_cast<UINT>(edge.Consumer)].Passes.front() << ";\n";
    output << "}\n";
}
