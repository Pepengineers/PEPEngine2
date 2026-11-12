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

namespace
{
    bool AccessStateMatches(GDX12RenderGraph::Mode mode, D3D12_RESOURCE_STATES state)
    {
        using Mode = GDX12RenderGraph::Mode;
        const auto writeStates = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
            D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_STREAM_OUT |
            D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST;
        const auto write = state & writeStates;
        if (!write) return mode == Mode::Read;
        if (mode == Mode::Read && write != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) return false;
        const auto bits = static_cast<UINT>(write);
        return state == write && (bits & (bits - 1)) == 0;
    }
}

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
    if (texture)
        for (const auto& resource : _resources)
            if (resource.Texture == texture)
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
    std::unordered_map<Resource, size_t> accessIndices;
    GDX12RenderGraphBuilder builder(*this, node, accessIndices);
    try
    {
        pass.RecordRenderGraph(builder);
        if (node.Name.empty()) throw std::invalid_argument("Render graph: pass must declare a name");
    }
    catch (...)
    {
        for (size_t i = 0; i < _resources.size(); ++i)
            if (_resources[i].Registered && (i >= resources.size() || !resources[i].Registered))
                _registeredResourceIds.erase(_resources[i].Identity.Id);
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
    if (_accessIndices.count(resource))
        throw std::invalid_argument("Render graph: duplicate versioned access");
    Use(resource, mode, state);
    _node.Accesses.back().Version = version;
}

void GDX12RenderGraphBuilder::Use(const Graph::Resource resource, const Graph::Mode mode, const D3D12_RESOURCE_STATES state)
{
    _graph.CheckResource(resource);
    if (mode != Graph::Mode::Read && mode != Graph::Mode::Write && mode != Graph::Mode::ReadWrite)
        throw std::invalid_argument("Render graph: invalid access mode");
    const auto [entry, inserted] = _accessIndices.try_emplace(resource, _node.Accesses.size());
    if (!inserted)
    {
        const auto& access = _node.Accesses[entry->second];
        if (access.Usage == Graph::Mode::Read && mode == Graph::Mode::Read && access.State == state) return;
        throw std::invalid_argument("Render graph: conflicting access; declare ReadWrite once: " + _node.Name);
    }
    try { _node.Accesses.push_back({ resource, mode, state }); }
    catch (...) { _accessIndices.erase(entry); throw; }
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
    if (_registeredResourceIds.count(identity.Id))
        throw std::invalid_argument("Render graph: duplicate resource view identity");
    auto& entry = _resources[resource];
    if (entry.Registered || entry.LogicalToken)
        throw std::invalid_argument("Render graph: resource already registered or is a pipeline token");
    _registeredResourceIds.insert(identity.Id);
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

void GDX12RenderGraph::SetLegacySplit(GDX12RenderPass& replaced, std::vector<IRenderPassLink*> inputs,
    std::vector<IRenderPassLink*> outputs, ResolveLegacyExternalViews resolveExternal)
{
    TryResolveLegacyExternalViews tryResolve;
    if (resolveExternal)
        tryResolve = [resolve = std::move(resolveExternal)]() -> std::optional<std::vector<LegacyExternalView>>
        {
            auto views = resolve();
            if (views.empty()) return std::nullopt;
            return views;
        };
    SetOptionalLegacySplit(replaced, std::move(inputs), std::move(outputs), std::move(tryResolve));
}

void GDX12RenderGraph::SetOptionalLegacySplit(GDX12RenderPass& replaced, std::vector<IRenderPassLink*> inputs,
    std::vector<IRenderPassLink*> outputs, TryResolveLegacyExternalViews resolveExternal)
{
    if (_executingSplit) throw std::logic_error("Cannot change legacy splits during execution");
    const auto node = std::find_if(_nodes.begin(), _nodes.end(), [&](const Node& entry) { return entry.Instance == &replaced; });
    if (node == _nodes.end() || node->HasPlacement || (outputs.empty() && !resolveExternal))
        throw std::invalid_argument("Legacy split requires a graph-owned legacy pass and a physical boundary");
    const auto resolve = [&](const std::vector<IRenderPassLink*>& links, bool write)
    {
        std::vector<Resource> result;
        for (auto* link : links)
        {
            const auto entry = _links.find(link);
            if (!link || entry == _links.end() || std::find(result.begin(), result.end(), entry->second) != result.end())
                throw std::invalid_argument("Legacy split has an unknown or duplicate boundary link");
            if (std::none_of(node->Accesses.begin(), node->Accesses.end(), [&](const Access& access)
                { return access.Id == entry->second && access.Usage != (write ? Mode::Read : Mode::Write); }))
                throw std::invalid_argument("Legacy split link is missing a matching access declaration");
            result.push_back(entry->second);
        }
        return result;
    };
    LegacySplit split{ resolve(inputs, false), resolve(outputs, true), std::move(resolveExternal), {} };
    for (const auto& access : node->Accesses)
        if (!_resources[access.Id].LogicalToken)
        {
            if ((access.Usage != Mode::Write && std::find(split.Inputs.begin(), split.Inputs.end(), access.Id) == split.Inputs.end()) ||
                (access.Usage != Mode::Read && std::find(split.Outputs.begin(), split.Outputs.end(), access.Id) == split.Outputs.end()))
                throw std::invalid_argument("Legacy split omits a declared physical access");
        }
    _legacySplits.insert_or_assign(static_cast<Pass>(node - _nodes.begin()), std::move(split));
    _compiled = false;
}

void GDX12RenderGraph::SetLegacyPrefixBefore(GDX12RenderPass& reader, std::function<bool()> enabled)
{
    if (_executingSplit) throw std::logic_error("Cannot change legacy splits during execution");
    const auto node = std::find_if(_nodes.begin(), _nodes.end(), [&](const Node& entry) { return entry.Instance == &reader; });
    if (node == _nodes.end() || node->HasPlacement || !enabled)
        throw std::invalid_argument("Legacy prefix requires a graph-owned reader and an enable predicate");
    _legacySplits.insert_or_assign(static_cast<Pass>(node - _nodes.begin()), LegacySplit{{}, {}, {}, std::move(enabled)});
    _compiled = false;
}

void GDX12RenderGraph::Compile(const bool reuseTextures) { CompileInternal(reuseTextures, true); }
void GDX12RenderGraph::CompilePlan() { CompileInternal(false, false); }

void GDX12RenderGraph::CompileInternal(bool reuseTextures, const bool allocateTextures)
{
    if (_executingSplit) throw std::logic_error("Cannot compile a graph during legacy split execution");
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
                if (!AccessStateMatches(access.Usage, access.State))
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
    std::vector<Resource> pinned;
    for (const auto& [pass, split] : _legacySplits)
    {
        if (_plan.RequiresNewExecutor || !live[pass])
            throw std::logic_error("Legacy split requires a live pass in a legacy-only graph");
        for (auto id : split.Inputs) ExpandRead(id, pinned);
        for (auto id : split.Outputs) ExpandRead(id, pinned);
    }
    for (auto id : pinned)
    {
        _resources[id].First = 0;
        _resources[id].Last = static_cast<UINT>(_order.size());
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

namespace
{
    bool SameIdentity(const ResourceIdentity& a, const ResourceIdentity& b)
    {
        return a.Id == b.Id && a.Adapter == b.Adapter && a.ResourceEpoch == b.ResourceEpoch &&
            a.Transfer == b.Transfer && a.Allocation == b.Allocation;
    }

    bool SameBacking(const ResourceIdentity& a, const ResourceIdentity& b)
    {
        return a.Id == b.Id || (a.Allocation != AllocationId::Invalid && a.Allocation == b.Allocation);
    }

    void CheckNativeOwner(ID3D12DeviceChild* native, GDX12Device& device)
    {
        ComPtr<IUnknown> owner, expected;
        ThrowIfFailed(native->GetDevice(IID_PPV_ARGS(&owner)));
        ThrowIfFailed(device.GetDevice().As(&expected));
        if (owner.Get() != expected.Get())
            throw std::invalid_argument("Graph executor: native object belongs to another device");
    }

    FenceProgress CheckProgress(FenceProgress progress)
    {
        if (progress.CompletedValue == (std::numeric_limits<uint64_t>::max)())
            ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
        return progress;
    }

    struct PlacedView
    {
        GDX12RenderGraph::ViewBinding Binding{};
        GDX12ResourceHandle Handle;
        D3D12_RESOURCE_DESC Desc{};
        D3D12_HEAP_TYPE Heap = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_STATES Entry = D3D12_RESOURCE_STATE_COMMON;
        bool CrossAdapter = false;
    };

    void CheckAlias(const PlacedView& a, const PlacedView& b)
    {
        const auto& x = a.Binding.Identity;
        const auto& y = b.Binding.Identity;
        const bool sameNative = a.Handle.Get() == b.Handle.Get();
        if ((sameNative || x.Id == y.Id) && !(sameNative && SameIdentity(x, y)))
            throw std::invalid_argument("Graph executor: inconsistent native view identity");
        if (!SameBacking(x, y) || sameNative) return;
        if (x.Adapter == y.Adapter)
            throw std::invalid_argument("Graph executor: one native view per device/backing is required");
        if (!a.CrossAdapter || !b.CrossAdapter || a.Desc.Width != b.Desc.Width)
            throw std::invalid_argument("Graph executor: incompatible cross-adapter alias views");
    }

    class PlacedViewIndex
    {
    public:
        void Add(const PlacedView& view)
        {
            const auto check = [&](auto& index, auto key)
            {
                const auto [entry, inserted] = index.try_emplace(key, &view);
                if (!inserted) CheckAlias(*entry->second, view);
            };
            const auto& identity = view.Binding.Identity;
            check(_byId, identity.Id);
            check(_byNative, view.Handle.Get());
            if (identity.Allocation != AllocationId::Invalid)
            {
                // The backing representative checks shared compatibility. A separate
                // device representative catches same-device aliases even across that pair.
                check(_byAllocation, identity.Allocation);
                check(_byAdapter[identity.Adapter], identity.Allocation);
            }
        }

        const PlacedView* Backing(const PlacedView& view) const
        {
            const auto& identity = view.Binding.Identity;
            // Equal IDs have already been checked for identical allocation identity.
            return identity.Allocation == AllocationId::Invalid
                ? _byId.at(identity.Id) : _byAllocation.at(identity.Allocation);
        }

    private:
        std::unordered_map<ResourceId, const PlacedView*> _byId;
        std::unordered_map<ID3D12Resource*, const PlacedView*> _byNative;
        std::unordered_map<AllocationId, const PlacedView*> _byAllocation;
        std::unordered_map<AdapterId, std::unordered_map<AllocationId, const PlacedView*>> _byAdapter;
    };

    void CheckNativeAccess(const PlacedView& view, const GDX12RenderGraph::Access& access)
    {
        const auto state = access.State;
        const auto flags = view.Desc.Flags;
        const UINT ordinaryStates = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER |
            D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_DEPTH_WRITE |
            D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_STREAM_OUT |
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_COPY_SOURCE |
            D3D12_RESOURCE_STATE_RESOLVE_DEST | D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        const auto bufferStates = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER |
            D3D12_RESOURCE_STATE_STREAM_OUT | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        if (state == D3D12_RESOURCE_STATE_COMMON || (static_cast<UINT>(state) & ~ordinaryStates) != 0 ||
            ((state & bufferStates) && view.Desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) ||
            ((state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) && !(flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) ||
            ((state & D3D12_RESOURCE_STATE_RENDER_TARGET) && !(flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) ||
            ((state & (D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ)) && !(flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) ||
            ((state & D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) && (flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)))
            throw std::invalid_argument("Graph executor: resource flags do not support the declared state");
        if (view.Heap == D3D12_HEAP_TYPE_UPLOAD &&
            (access.Usage != GDX12RenderGraph::Mode::Read || (state & view.Entry) != state))
            throw std::invalid_argument("Graph executor: upload buffers are fixed GENERIC_READ");
        if (view.Heap == D3D12_HEAP_TYPE_READBACK &&
            (access.Usage != GDX12RenderGraph::Mode::Write || state != D3D12_RESOURCE_STATE_COPY_DEST))
            throw std::invalid_argument("Graph executor: readback buffers are fixed COPY_DEST");
        if (view.CrossAdapter && state != D3D12_RESOURCE_STATE_COPY_SOURCE && state != D3D12_RESOURCE_STATE_COPY_DEST)
            throw std::invalid_argument("Graph executor: shared backing views are copy-only");
    }

    void CheckWaitPath(GDX12CommandQueue& producer, GDX12CommandQueue& consumer)
    {
        if (&producer == &consumer) return;
        if (producer.GetIdentity().Location.Adapter != consumer.GetIdentity().Location.Adapter)
            consumer.ImportTimeline(producer);
        CheckProgress(consumer.GetCompletionProgress(producer.GetTimelineIdentity()));
    }

    void AddIssuedWait(std::vector<CompletionPoint>& waits, CompletionPoint point)
    {
        for (auto& previous : waits)
            if (previous.Timeline() == point.Timeline())
            {
                if (previous.Value() < point.Value()) previous = point;
                return;
            }
        waits.push_back(point);
    }
}

struct GDX12RenderGraph::PlanExecutor::State
{
    struct Batch
    {
        QueueBinding Binding{};
        TimelineId Timeline;
        ComPtr<ID3D12Fence> Fence;
        std::vector<Access> Uses;
        std::vector<size_t> Dependencies;
        std::vector<CompletionPoint> Waits;
        std::unique_ptr<GDX12CommandList> Commands;
        CompletionPoint Point;
        bool Submitted = false;

        bool Completed() const
        {
            return !Submitted || (Point.IsValid() && Point.IsReachedBy(
                CheckProgress({ Timeline, Fence->GetCompletedValue() })));
        }
    };
    struct Invocation
    {
        std::vector<PlacedView> Views;
        std::vector<Batch> Batches;
        std::vector<std::shared_ptr<const void>> Owners;
    };

    explicit State(size_t maxInFlight) : Capacity(maxInFlight) { Pending.reserve(maxInFlight); }
    size_t Capacity;
    bool Failed = false;
    std::vector<Invocation> Pending;
};

ID3D12Resource* GDX12RenderGraph::BoundResources::Get(Resource view) const
{
    for (const auto& entry : _views)
        if (entry.first == view) return entry.second;
    throw std::invalid_argument("Graph recorder requested an undeclared physical view");
}

void GDX12RenderPass::RecordPlacedCommands(GDX12CommandList*, const GDX12RenderGraph::BoundResources&)
{
    throw std::logic_error("Explicit placement requires a placed command recorder");
}

GDX12RenderGraph::PlanExecutor::PlanExecutor(size_t maxInFlight)
{
    if (!maxInFlight) throw std::invalid_argument("Graph executor requires nonzero in-flight capacity");
    _state = std::make_unique<State>(maxInFlight);
}

GDX12RenderGraph::PlanExecutor::~PlanExecutor()
{
    try { Drain(); }
    catch (...)
    {
        // shortcut: retain unresolved GPU owners on fatal teardown until process exit, until device-loss recovery owns them.
        OutputDebugStringA("Graph executor could not prove GPU retirement during teardown; retaining native owners.\n");
        _state.release();
    }
}

size_t GDX12RenderGraph::PlanExecutor::PendingCount() const { return _state->Pending.size(); }
bool GDX12RenderGraph::PlanExecutor::IsSubmissionFaulted() const { return _state->Failed; }

void GDX12RenderGraph::PlanExecutor::CollectCompleted()
{
    auto& pending = _state->Pending;
    pending.erase(std::remove_if(pending.begin(), pending.end(), [](const State::Invocation& invocation)
    {
        return std::all_of(invocation.Batches.begin(), invocation.Batches.end(),
            [](const State::Batch& batch) { return batch.Completed(); });
    }), pending.end());
}

void GDX12RenderGraph::PlanExecutor::Drain()
{
    // Observe all dependencies while draining so a failed remote producer cannot hide behind a local wait.
    while (!_state->Pending.empty())
    {
        for (auto& invocation : _state->Pending)
            for (auto& batch : invocation.Batches)
                if (batch.Submitted && !batch.Completed())
                {
                    auto& queue = *batch.Binding.Queue;
                    if (!(queue.GetTimelineIdentity() == batch.Timeline))
                        throw std::logic_error("Graph executor: queue reset before retirement");
                    CheckProgress({ batch.Timeline, batch.Fence->GetCompletedValue() });
                    for (const auto& wait : batch.Waits) CheckProgress(queue.GetCompletionProgress(wait.Timeline()));
                    if (!batch.Point.IsValid()) batch.Point = queue.SignalCompletion();
                }
        CollectCompleted();
        if (!_state->Pending.empty()) Sleep(1);
    }
}

std::optional<GDX12RenderGraph::ExecutionReceipt> GDX12RenderGraph::PlanExecutor::TrySubmit(
    const GDX12RenderGraph& graph, const ExecutionBindings& bindings)
{
    if (_state->Failed) throw std::logic_error("Graph executor stopped after a submission failure");
    const auto& plan = graph.GetCompiledPlan();
    if (!plan.RequiresNewExecutor ||
        std::any_of(plan.Batches.begin(), plan.Batches.end(), [](const PlannedBatch& b) { return b.LegacyPrimary; }))
        throw std::invalid_argument("Graph executor requires an entirely explicitly placed plan");
    CollectCompleted();
    if (_state->Pending.size() == _state->Capacity) return std::nullopt;

    State::Invocation work;
    work.Batches.resize(plan.Batches.size());
    work.Views.resize(plan.Views.size());
    work.Owners = bindings.KeepAlive;
    std::vector<Placement> placements;
    std::unordered_map<AdapterId, GDX12Device*> devices;
    for (const auto& binding : bindings.Queues)
    {
        if (!binding.Device || !binding.Device->IsInitialized() || !binding.Queue ||
            !binding.Queue->GetCommandQueue() || !binding.Queue->GetFence())
            throw std::invalid_argument("Graph executor: missing live queue/device binding");
        auto& queue = *binding.Queue;
        const auto location = queue.GetIdentity().Location;
        if (!queue.GetTimelineIdentity().IsValid() || location.Adapter != binding.Device->GetSubmissionAdapterId() ||
            std::find(placements.begin(), placements.end(), location) != placements.end() ||
            queue.GetCommandQueue()->GetDesc().Type != queue.GetType())
            throw std::invalid_argument("Graph executor: invalid or duplicate queue placement");
        const auto inserted = devices.emplace(location.Adapter, binding.Device);
        if (inserted.first->second != binding.Device)
            throw std::invalid_argument("Graph executor: one device per adapter incarnation");
        CheckNativeOwner(queue.GetCommandQueue().Get(), *binding.Device);
        CheckProgress(queue.GetCompletionProgress());
        ThrowIfFailed(binding.Device->GetDevice()->GetDeviceRemovedReason());
        placements.push_back(location);
    }

    PlacedViewIndex viewIndex;
    for (const auto& previous : _state->Pending)
        for (const auto& view : previous.Views)
            if (view.Handle) viewIndex.Add(view);

    for (const auto& binding : bindings.Views)
    {
        if (binding.View >= plan.Views.size() || !binding.Native || !binding.Native->D3DResource ||
            work.Views[binding.View].Handle)
            throw std::invalid_argument("Graph executor: invalid or duplicate physical view binding");
        const auto& planned = plan.Views[binding.View];
        const auto& id = binding.Identity;
        const auto device = devices.find(id.Adapter);
        if (planned.LogicalToken || id.Id == ResourceId::Invalid || !SameIdentity(id, planned.Identity) ||
            device == devices.end())
            throw std::invalid_argument("Graph executor: physical view identity or epoch differs from the plan");
        auto& view = work.Views[binding.View];
        view.Binding = binding;
        view.Handle = binding.Native->D3DResource;
        view.Desc = view.Handle->GetDesc();
        CheckNativeOwner(view.Handle.Get(), *device->second);
        D3D12_HEAP_PROPERTIES heap{};
        D3D12_HEAP_FLAGS flags{};
        ThrowIfFailed(view.Handle->GetHeapProperties(&heap, &flags));
        view.Heap = heap.Type;
        view.CrossAdapter = (view.Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER) != 0;
        if (view.CrossAdapter != planned.CrossAdapter || (view.CrossAdapter &&
            (!(flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER) || heap.Type != D3D12_HEAP_TYPE_DEFAULT ||
                view.Desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)))
            throw std::invalid_argument("Graph executor: cross-adapter binding requires a shared buffer view");
        if (heap.Type == D3D12_HEAP_TYPE_UPLOAD) view.Entry = D3D12_RESOURCE_STATE_GENERIC_READ;
        else if (heap.Type == D3D12_HEAP_TYPE_READBACK) view.Entry = D3D12_RESOURCE_STATE_COPY_DEST;
        else if (heap.Type != D3D12_HEAP_TYPE_DEFAULT)
            throw std::invalid_argument("Graph executor: unsupported heap type");
        if (binding.Native->GetCurrentState() != view.Entry)
            throw std::invalid_argument("Graph executor: " + graph._resources[binding.View].Name + " view " +
                std::to_string(binding.View) + " was not released to entry state " + std::to_string(view.Entry) +
                "; actual state " + std::to_string(binding.Native->GetCurrentState()) +
                ", width " + std::to_string(view.Desc.Width));
        viewIndex.Add(view);
    }

    static constexpr size_t NoIndex = (std::numeric_limits<size_t>::max)();
    std::vector<size_t> useIndices(work.Views.size(), NoIndex);
    for (size_t i = 0; i < plan.Batches.size(); ++i)
    {
        auto& batch = work.Batches[i];
        const auto& planned = plan.Batches[i];
        const auto location = std::find(placements.begin(), placements.end(), planned.Location);
        if (location == placements.end() || planned.Passes.size() != 1 || static_cast<size_t>(planned.Id) != i)
            throw std::invalid_argument("Graph executor: unsupported batch or unavailable queue");
        const auto& node = graph._nodes[planned.Passes.front()];
        if (!node.Instance->SupportsGraphQueue(planned.Location.Queue) || !node.Instance->GetBufferAccesses().empty())
            throw std::invalid_argument("Graph executor: pass changed its placement/access contract");
        batch.Binding = bindings.Queues[location - placements.begin()];
        batch.Timeline = batch.Binding.Queue->GetTimelineIdentity();
        batch.Fence = batch.Binding.Queue->GetFence();
        for (const auto& access : planned.Accesses)
        {
            std::vector<Resource> used;
            if (access.Usage == Mode::Write) used.push_back(access.Id);
            else graph.ExpandRead(access.Id, used);
            for (auto id : used)
            {
                if (plan.Views[id].LogicalToken) continue;
                const auto& view = work.Views[id];
                if (!view.Handle || view.Binding.Identity.Adapter != planned.Location.Adapter)
                    throw std::invalid_argument("Graph executor: missing or wrong-device physical view");
                const Access use{ id, id == access.Id ? access.Usage : Mode::Read, access.State, access.Version };
                CheckNativeAccess(view, use);
                auto& index = useIndices[id];
                if (index == NoIndex)
                {
                    index = batch.Uses.size();
                    batch.Uses.push_back(use);
                }
                else
                {
                    auto& prior = batch.Uses[index];
                    if (prior.State != use.State || !(prior.Version == use.Version))
                        throw std::invalid_argument("Graph executor: incompatible simultaneous declarations of one view");
                    if (prior.Usage != use.Usage) prior.Usage = Mode::ReadWrite;
                }
            }
        }
        for (const auto& use : batch.Uses) useIndices[use.Id] = NoIndex;
    }

    const auto addDependency = [&](size_t from, size_t to)
    {
        if (from >= to || to >= work.Batches.size())
            throw std::invalid_argument("Graph executor: dependency violates topological/FIFO order");
        auto& source = work.Batches[from];
        auto& target = work.Batches[to];
        if (source.Binding.Queue == target.Binding.Queue ||
            std::find(target.Dependencies.begin(), target.Dependencies.end(), from) != target.Dependencies.end()) return;
        CheckWaitPath(*source.Binding.Queue, *target.Binding.Queue);
        target.Dependencies.push_back(from);
    };
    for (const auto& edge : plan.Dependencies)
        addDependency(static_cast<size_t>(edge.Producer), static_cast<size_t>(edge.Consumer));

    struct BackingUses
    {
        size_t LastBatch = NoIndex;
        std::vector<const State::Batch*> PendingBatches;
        const Access* LastPendingWrite = nullptr;
    };
    std::unordered_map<const PlacedView*, BackingUses> backingUses;
    for (const auto& invocation : _state->Pending)
        for (const auto& batch : invocation.Batches)
            for (const auto& use : batch.Uses)
            {
                auto& history = backingUses[viewIndex.Backing(invocation.Views[use.Id])];
                if (history.PendingBatches.empty() || history.PendingBatches.back() != &batch)
                    history.PendingBatches.push_back(&batch);
                if (use.Usage != Mode::Read) history.LastPendingWrite = &use;
            }

    // COMMON handoffs also order readers whose whole-resource layout transitions could overlap.
    for (size_t i = 0; i < work.Batches.size(); ++i)
    {
        auto& batch = work.Batches[i];
        std::unordered_set<const State::Batch*> checkedPending;
        for (const auto& use : batch.Uses)
        {
            auto& history = backingUses[viewIndex.Backing(work.Views[use.Id])];
            if (history.LastBatch != i)
            {
                if (history.LastBatch != NoIndex) addDependency(history.LastBatch, i);
                history.LastBatch = i;
            }
            for (const auto* prior : history.PendingBatches)
                if (prior->Binding.Queue != batch.Binding.Queue && checkedPending.insert(prior).second &&
                    !prior->Completed())
                {
                    CheckWaitPath(*prior->Binding.Queue, *batch.Binding.Queue);
                    AddIssuedWait(batch.Waits, prior->Point);
                }
        }
    }

    std::vector<const ExactInputBinding*> inputs(plan.Views.size());
    for (const auto& input : bindings.ExactInputs)
    {
        if (input.View >= inputs.size() || inputs[input.View] || !input.Reservation || !input.Ready.IsValid() ||
            !input.Version.IsValid() || !work.Views[input.View].Handle)
            throw std::invalid_argument("Graph executor: invalid exact input binding");
        const auto& view = work.Views[input.View];
        const auto& identity = view.Binding.Identity;
        const auto location = input.Ready.Timeline().Producer.Location;
        if (location.Adapter != identity.Adapter)
        {
            const auto& source = input.SharedSource;
            if (!view.CrossAdapter || !input.SharedProducer ||
                !(input.SharedProducer->GetTimelineIdentity() == input.Ready.Timeline()) ||
                location.Queue != QueueKind::Copy || source.Adapter != location.Adapter ||
                source.Id == ResourceId::Invalid || source.Id == identity.Id || source.Allocation != identity.Allocation ||
                source.ResourceEpoch != identity.ResourceEpoch || source.Transfer != identity.Transfer)
                throw std::invalid_argument("Graph executor: foreign exact point lacks shared-export provenance");
        }
        else if (input.SharedProducer || input.SharedSource.Id != ResourceId::Invalid)
            throw std::invalid_argument("Graph executor: local exact input cannot carry a remote export binding");
        const auto history = backingUses.find(viewIndex.Backing(view));
        const auto* lastWrite = history == backingUses.end() ? nullptr : history->second.LastPendingWrite;
        if (lastWrite && !(lastWrite->Version == input.Version))
            throw std::invalid_argument("Graph executor: exact input conflicts with an already queued allocation writer");
        inputs[input.View] = &input;
        work.Owners.push_back(input.Reservation);
    }
    std::vector<bool> expectedInputs(plan.Views.size());
    for (const auto& expected : plan.Inputs)
    {
        const auto* input = inputs[expected.View];
        if (!input || !(input->Version == expected.Version))
            throw std::invalid_argument("Graph executor: missing or stale exact input version");
        expectedInputs[expected.View] = true;
        auto& batch = work.Batches[static_cast<size_t>(expected.Consumer)];
        if (input->SharedProducer) CheckWaitPath(*input->SharedProducer, *batch.Binding.Queue);
        const auto progress = CheckProgress(batch.Binding.Queue->GetCompletionProgress(input->Ready.Timeline()));
        if (!(batch.Timeline == input->Ready.Timeline()) && !input->Ready.IsReachedBy(progress))
            AddIssuedWait(batch.Waits, input->Ready);
    }
    for (const auto& input : bindings.ExactInputs)
        if (!expectedInputs[input.View])
            throw std::invalid_argument("Graph executor: unexpected exact input binding");

    for (const auto& binding : bindings.Queues)
    {
        auto& queue = *binding.Queue;
        const auto batches = std::count_if(work.Batches.begin(), work.Batches.end(),
            [&](const State::Batch& batch) { return batch.Binding.Queue == &queue; });
        if (queue.FenceValue != queue.GetLastCompletionPoint().Value() ||
            queue.FenceValue > (std::numeric_limits<uint64_t>::max)() - 1 - static_cast<uint64_t>(batches))
            throw std::invalid_argument("Graph executor: queue fence values unavailable");
    }

    // Own the allocators up front so invalid recordings can be discarded without queue bookkeeping.
    try
    {
        for (size_t i = 0; i < work.Batches.size(); ++i)
        {
            auto& batch = work.Batches[i];
            batch.Commands = std::make_unique<GDX12CommandList>(batch.Binding.Device, batch.Binding.Queue->GetType());
            auto* native = batch.Commands->GetCommandList().Get();
            // COPY uses implicit promotion from COMMON and decays at the submission boundary.
            const bool explicitTransitions = batch.Binding.Queue->GetType() != D3D12_COMMAND_LIST_TYPE_COPY;
            BoundResources resources;
            for (const auto& use : batch.Uses)
            {
                const auto& view = work.Views[use.Id];
                resources._views.emplace_back(use.Id, view.Handle.Get());
                if (explicitTransitions && view.Heap == D3D12_HEAP_TYPE_DEFAULT && view.Entry != use.State)
                {
                    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(view.Handle.Get(), view.Entry, use.State);
                    native->ResourceBarrier(1, &barrier);
                }
            }
            graph._nodes[plan.Batches[i].Passes.front()].Instance->RecordPlacedCommands(batch.Commands.get(), resources);
            for (const auto& use : batch.Uses)
            {
                const auto& view = work.Views[use.Id];
                if (explicitTransitions && view.Heap == D3D12_HEAP_TYPE_DEFAULT && view.Entry != use.State)
                {
                    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(view.Handle.Get(), use.State, view.Entry);
                    native->ResourceBarrier(1, &barrier);
                }
            }
            ThrowIfFailed(native->Close());
        }
        for (const auto& view : work.Views)
            if (view.Handle && (view.Binding.Native->D3DResource.Get() != view.Handle.Get() ||
                view.Binding.Native->GetCurrentState() != view.Entry))
                throw std::logic_error("Graph recorder mutated a bound resource or its boundary state");
    }
    catch (...)
    {
        for (const auto& view : work.Views)
            if (view.Handle && (view.Binding.Native->D3DResource.Get() != view.Handle.Get() ||
                view.Binding.Native->GetCurrentState() != view.Entry))
            {
                view.Binding.Native->D3DResource = view.Handle;
                view.Binding.Native->EndConcurrentRead();
                view.Binding.Native->SetCurrentState(view.Entry);
            }
        throw;
    }

    ExecutionReceipt receipt;
    receipt.Batches.resize(work.Batches.size());
    _state->Pending.push_back(std::move(work));
    auto& pending = _state->Pending.back();
    try
    {
        for (size_t i = 0; i < pending.Batches.size(); ++i)
        {
            auto& batch = pending.Batches[i];
            auto& queue = *batch.Binding.Queue;
            for (const auto& wait : batch.Waits) queue.WaitFor(wait);
            for (auto producer : batch.Dependencies) queue.WaitFor(receipt.Batches[producer]);
            ID3D12CommandList* native = batch.Commands->GetCommandList().Get();
            queue.GetCommandQueue()->ExecuteCommandLists(1, &native);
            batch.Submitted = true;
            batch.Point = queue.SignalCompletion();
            receipt.Batches[i] = batch.Point;
        }
    }
    catch (...)
    {
        _state->Failed = true;
        throw;
    }
    return std::optional<ExecutionReceipt>{ std::move(receipt) };
}

void GDX12RenderGraph::CheckLegacyExecution(GDX12CommandQueue* computeQueue) const
{
    if (_executingSplit) throw std::logic_error("Cannot recursively execute a legacy split graph");
    if (!_compiled) throw std::logic_error("Render graph must be compiled before execution");
    if (_plan.RequiresNewExecutor)
        throw std::logic_error("Render graph: explicit placement/version plan requires PlanExecutor; legacy Execute supports primary DIRECT/COMPUTE only");
    if (!_resourcesReady) throw std::logic_error("Render graph: CompilePlan does not prepare native resources; call Compile for legacy Execute");
    for (const auto& resource : _resources)
        if (resource.Slot != UINT_MAX && resource.Texture->GetResource() != _slots[resource.Slot].ResourceObject.get())
            throw std::logic_error("Render graph texture changed; recompile after resize: " + resource.Name);
    if (computeQueue && (!_device || computeQueue->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE))
        throw std::invalid_argument("Render graph requires a COMPUTE queue on its device");
}

void GDX12RenderGraph::ResetLegacyStatistics()
{
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
}

GDX12CommandList* GDX12RenderGraph::Execute(GDX12CommandList* commands, GDX12CommandQueue* computeQueue)
{
    GDX12_SCOPED_CPU_EVENT("Graph recording");
    CheckLegacyExecution(computeQueue);
    ResetLegacyStatistics();
    return RecordLegacyRange(commands, computeQueue, 0, static_cast<UINT>(_order.size()));
}

GDX12CommandList* GDX12RenderGraph::ExecuteLegacySplit(GDX12CommandList* commands, GDX12CommandQueue* computeQueue,
    const SubmitLegacyReplacement& submit)
{
    GDX12_SCOPED_CPU_EVENT("Legacy graph split recording");
    CheckLegacyExecution(computeQueue);
    auto* direct = _device ? _device->GetCommandQueue() : nullptr;
    if (!submit || _legacySplits.empty() || !direct || !direct->GetTimelineIdentity().IsValid() ||
        !commands || commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        throw std::invalid_argument("Legacy split requires configured points and a live primary DIRECT list");
    CheckNativeOwner(commands->GetCommandList().Get(), *_device);
    if (computeQueue && (!computeQueue->GetTimelineIdentity().IsValid() ||
        computeQueue->GetIdentity().Location.Adapter != _device->GetSubmissionAdapterId()))
        throw std::invalid_argument("Legacy split COMPUTE queue must be on the primary device");
    ThrowIfFailed(_device->GetDevice()->GetDeviceRemovedReason());
    struct ActiveSplit
    {
        bool& Active;
        ~ActiveSplit() { Active = false; }
    } guard{ _executingSplit };
    _executingSplit = true;
    ResetLegacyStatistics();

    struct LoanOwner
    {
        ResourceIdentity Identity;
        std::shared_ptr<GDX12Resource> Native;
        GDX12ResourceHandle Handle;
    };
    struct Captured
    {
        GDX12Resource* Native;
        GDX12ResourceHandle Handle;
        D3D12_RESOURCE_STATES BoundaryState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES SavedState = D3D12_RESOURCE_STATE_COMMON;
        bool External = false;
        std::optional<D3D12_RESOURCE_STATES> LegacyAfter;
    };
    std::shared_ptr<std::vector<LoanOwner>> frameOwners;
    UINT begin = 0;
    for (UINT i = 0; i < _order.size(); ++i)
    {
        const auto pass = _order[i];
        const auto point = _legacySplits.find(pass);
        if (point == _legacySplits.end()) continue;
        const bool enabled = !point->second.PrefixEnabled || point->second.PrefixEnabled();
        if (!_compiled) throw std::logic_error("Legacy graph changed while enabling a prefix");
        if (!enabled) continue;
        std::optional<std::vector<LegacyExternalView>> external(std::in_place);
        if (point->second.ResolveExternal) external = point->second.ResolveExternal();
        if (!_compiled) throw std::logic_error("Legacy graph changed while resolving its boundary");
        if (!external) continue;
        if (point->second.Outputs.empty() && external->empty() && !point->second.PrefixEnabled)
            throw std::invalid_argument("Legacy split resolved without a physical boundary");
        std::vector<Captured> handles;
        const auto capture = [&](const std::vector<Resource>& ids, bool write)
        {
            std::vector<LegacyBoundaryView> views;
            for (auto id : ids)
            {
                const auto& entry = _resources[id];
                auto* texture = entry.Link->GetTexture();
                if (!texture || !texture->_resource || !texture->_resource->D3DResource ||
                    (write && entry.Texture && entry.Texture->GetResource() != texture->GetResource()))
                    throw std::invalid_argument("Legacy split boundary has no stable texture binding: " + entry.Name);
                auto* native = texture->_resource.get();
                auto& handle = native->D3DResource;
                bool known = false;
                for (const auto& other : handles)
                    if (other.Handle.Get() == handle.Get())
                    {
                        if (other.Native != native)
                            throw std::invalid_argument("Legacy split boundary has competing native resource trackers");
                        known = true;
                    }
                if (!known)
                {
                    CheckNativeOwner(handle.Get(), *_device);
                    D3D12_HEAP_PROPERTIES heap{};
                    ThrowIfFailed(handle->GetHeapProperties(&heap, nullptr));
                    if (heap.Type != D3D12_HEAP_TYPE_DEFAULT)
                        throw std::invalid_argument("Legacy split boundary requires a DEFAULT resource");
                    handles.push_back({ native, handle });
                }
                views.push_back({ entry.Link, texture, texture->_resource });
            }
            return views;
        };
        const LegacyBoundary boundary{ _nodes[pass].Instance, capture(point->second.Inputs, false),
            capture(point->second.Outputs, true), std::move(*external) };
        for (const auto& loan : boundary.ExternalViews)
        {
            const auto& identity = loan.Identity;
            if (!loan.Native || !loan.Native->D3DResource || identity.Adapter != _device->GetSubmissionAdapterId() ||
                identity.Id == ResourceId::Invalid || identity.ResourceEpoch == Epoch::Invalid ||
                identity.Allocation == AllocationId::Invalid ||
                (loan.Usage != Mode::Read && loan.Usage != Mode::Write && loan.Usage != Mode::ReadWrite) ||
                (loan.Usage != Mode::Read && !loan.LegacyAfter))
                throw std::invalid_argument("Legacy external view requires a complete primary allocation and state contract");
            const auto& handle = loan.Native->D3DResource;
            if (std::any_of(handles.begin(), handles.end(), [&](const Captured& entry)
                { return entry.Handle.Get() == handle.Get(); }) ||
                std::any_of(_slots.begin(), _slots.end(), [&](const Slot& slot)
                { return slot.ResourceObject->D3DResource.Get() == handle.Get(); }))
                throw std::invalid_argument("Legacy external view duplicates a boundary or bypasses texture pool pinning");
            CheckNativeOwner(handle.Get(), *_device);
            D3D12_HEAP_PROPERTIES heap{};
            ThrowIfFailed(handle->GetHeapProperties(&heap, nullptr));
            PlacedView view;
            view.Binding = { 0, identity, loan.Native };
            view.Handle = handle;
            view.Desc = handle->GetDesc();
            view.Heap = heap.Type;
            if ((view.Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER) ||
                (heap.Type != D3D12_HEAP_TYPE_DEFAULT && heap.Type != D3D12_HEAP_TYPE_UPLOAD))
                throw std::invalid_argument("Legacy external view must be primary-local DEFAULT or UPLOAD");
            if (heap.Type == D3D12_HEAP_TYPE_UPLOAD)
            {
                view.Entry = D3D12_RESOURCE_STATE_GENERIC_READ;
                if (loan.Native->GetCurrentState() != view.Entry || (loan.LegacyAfter && *loan.LegacyAfter != view.Entry))
                    throw std::invalid_argument("Legacy UPLOAD view must preserve its fixed GENERIC_READ state");
            }
            if (!AccessStateMatches(loan.Usage, loan.PlacedState))
                throw std::invalid_argument("Legacy external view state conflicts with its access mode");
            CheckNativeAccess(view, { 0, loan.Usage, loan.PlacedState });
            if (loan.LegacyAfter && *loan.LegacyAfter != D3D12_RESOURCE_STATE_COMMON)
            {
                if (!AccessStateMatches(Mode::Read, *loan.LegacyAfter) && !AccessStateMatches(Mode::Write, *loan.LegacyAfter))
                    throw std::invalid_argument("Legacy external view has an incompatible resume state");
                CheckNativeAccess(view, { 0, Mode::Read, *loan.LegacyAfter });
            }

            if (!frameOwners) frameOwners = std::make_shared<std::vector<LoanOwner>>();
            bool known = false;
            for (const auto& prior : *frameOwners)
                if (prior.Native == loan.Native || prior.Handle.Get() == handle.Get() || SameBacking(prior.Identity, identity))
                {
                    if (prior.Native != loan.Native || prior.Handle.Get() != handle.Get() ||
                        !SameIdentity(prior.Identity, identity))
                    {
                        const auto describe = [](const ResourceIdentity& value)
                        {
                            return std::to_string(static_cast<uint64_t>(value.Id)) + "/" +
                                std::to_string(static_cast<uint64_t>(value.Adapter)) + "/" +
                                std::to_string(static_cast<uint64_t>(value.ResourceEpoch)) + "/" +
                                std::to_string(static_cast<uint64_t>(value.Transfer)) + "/" +
                                std::to_string(static_cast<uint64_t>(value.Allocation));
                        };
                        throw std::invalid_argument("Legacy external view changed canonical owner or identity across replacements: pass=" +
                            _nodes[pass].Name + ", prior/current id/adapter/epoch/transfer/allocation=" + describe(prior.Identity) +
                            " -> " + describe(identity) + ", same_native=" + std::to_string(prior.Native == loan.Native) +
                            ", same_handle=" + std::to_string(prior.Handle.Get() == handle.Get()) +
                            ", width=" + std::to_string(view.Desc.Width));
                    }
                    known = true;
                }
            if (!known) frameOwners->push_back({ identity, loan.Native, handle });
            handles.push_back({ loan.Native.get(), handle, view.Entry, D3D12_RESOURCE_STATE_COMMON, true, loan.LegacyAfter });
        }
        const auto validateBoundary = [&]
        {
            if (!_compiled) throw std::logic_error("Legacy graph changed during split execution");
            for (const auto* views : { &boundary.Inputs, &boundary.Outputs })
                for (const auto& view : *views)
                    if (view.Link->GetTexture() != view.Texture || view.Texture->GetResource() != view.Native.get())
                        throw std::logic_error("Legacy split boundary link changed during the segment");
            for (const auto& entry : handles)
                if (entry.Native->D3DResource.Get() != entry.Handle.Get())
                    throw std::logic_error("Legacy split boundary native allocation changed during the segment");
        };

        commands = RecordLegacyRange(commands, computeQueue, begin, i, frameOwners);
        validateBoundary();
        for (auto& entry : handles)
        {
            entry.SavedState = entry.Native->GetCurrentState();
            if (entry.External && entry.SavedState == UnmanagedState)
                throw std::logic_error("Legacy prefix left an external tracker without an exact state");
            if (entry.BoundaryState != D3D12_RESOURCE_STATE_COMMON && entry.SavedState != entry.BoundaryState)
                throw std::logic_error("Legacy prefix mutated a fixed heap state");
            if (entry.SavedState != entry.BoundaryState)
                commands->ResourceBarrier({ entry.Native->GetTransitionBarrier(entry.BoundaryState) });
        }
        direct->ExecuteCommandList(commands);
        const auto prefix = direct->GetLastCompletionPoint();
        const auto replacement = submit(boundary, prefix);
        ThrowIfFailed(_device->GetDevice()->GetDeviceRemovedReason());
        validateBoundary();
        for (const auto& entry : handles)
            if (entry.Native->GetCurrentState() != entry.BoundaryState)
                throw std::logic_error("Legacy replacement did not restore its boundary state");
        const auto latest = direct->GetLastCompletionPoint();
        if (!prefix.IsValid() || !(prefix.Timeline() == direct->GetTimelineIdentity()) ||
            !(latest.Timeline() == prefix.Timeline()))
            throw std::logic_error("Legacy replacement changed the primary DIRECT timeline");
        if (replacement)
        {
            if (!replacement->IsValid() || !(replacement->Timeline() == prefix.Timeline()) ||
                replacement->Value() <= prefix.Value() || replacement->Value() > latest.Value())
                throw std::invalid_argument("Legacy replacement requires an issued primary DIRECT point after the prefix");
            begin = point->second.PrefixEnabled ? i : i + 1;
        }
        else
        {
            if (latest.Value() != prefix.Value())
                throw std::logic_error("Legacy fallback requested after primary DIRECT submission advanced");
            begin = i;
        }
        commands = direct->GetCommandList();
        if (frameOwners) commands->_legacyOwners.push_back(frameOwners);
        for (const auto& entry : handles)
            if (entry.External)
            {
                const auto state = replacement ? entry.LegacyAfter.value_or(entry.SavedState) : entry.SavedState;
                if (state != entry.BoundaryState)
                    commands->ResourceBarrier({ entry.Native->GetTransitionBarrier(state) });
            }
    }
    return RecordLegacyRange(commands, computeQueue, begin, static_cast<UINT>(_order.size()), frameOwners);
}

GDX12CommandList* GDX12RenderGraph::RecordLegacyRange(GDX12CommandList* commands, GDX12CommandQueue* computeQueue,
    UINT begin, UINT end, const std::shared_ptr<const void>& owners)
{
    const auto retain = [&](GDX12CommandList* list)
    {
        if (owners && (list->_legacyOwners.empty() || list->_legacyOwners.back() != owners))
            list->_legacyOwners.push_back(owners);
        return list;
    };
    retain(commands);
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
        commands = retain(directQueue->GetCommandList());
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
        for (UINT i = begin; i < end; ++i)
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
            commands = retain(directQueue->GetCommandList());
            auto* compute = retain(computeQueue->GetCommandList());
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
        join(pending.size(), end == _order.size() ? "frame retirement" : "legacy split boundary");
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
