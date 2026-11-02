#include "Engine.RendererDX12/GDX12RenderGraph.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12Texture.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <algorithm>
#include <ostream>
#include <iomanip>
#include <queue>
#include <stdexcept>

void GDX12RenderGraph::CheckResource(Resource id) const
{
    if (id >= _resources.size()) throw std::out_of_range("Render graph: invalid resource");
}

void GDX12RenderGraph::CheckPass(Pass id) const
{
    if (id >= _nodes.size()) throw std::out_of_range("Render graph: invalid pass");
}

GDX12RenderGraph::Resource GDX12RenderGraph::Create(std::string name, GDX12Texture* texture, bool transient)
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

void GDX12RenderGraph::ExpandRead(Resource id, std::vector<Resource>& result) const
{
    if (std::find(result.begin(), result.end(), id) != result.end()) return;
    result.push_back(id);
    for (auto fallback : _resources[id].Fallbacks) ExpandRead(fallback, result);
}

void GDX12RenderGraph::AddFallback(Resource output, Resource input)
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
    _shared.emplace(resource, Import(std::move(name)));
}

void GDX12RenderGraphBuilder::SetName(std::string name) { _node.Name = std::move(name); }
void GDX12RenderGraphBuilder::HasSideEffects() { _node.SideEffect = true; }

void GDX12RenderGraphBuilder::Use(Graph::Resource resource, Graph::Mode mode, D3D12_RESOURCE_STATES state)
{
    _graph.CheckResource(resource);
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

void GDX12RenderGraphBuilder::Read(IRenderPassLink* link, D3D12_RESOURCE_STATES state)
{ Use(Find(link), Graph::Mode::Read, state); }

void GDX12RenderGraphBuilder::ReadInputs(const std::vector<IRenderPassLink*>& inputs, D3D12_RESOURCE_STATES state)
{ for (auto* input : inputs) Read(input, state); }

void GDX12RenderGraphBuilder::ReadWrite(IRenderPassLink* link, D3D12_RESOURCE_STATES state)
{ Use(Find(link), Graph::Mode::ReadWrite, state); }

void GDX12RenderGraphBuilder::Write(IRenderPassLink* link, std::string name, Lifetime lifetime,
    D3D12_RESOURCE_STATES state, GDX12Texture* storage)
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

void GDX12RenderGraphBuilder::Read(SharedResource resource)
{
    const auto found = _graph._shared.find(resource);
    if (found == _graph._shared.end()) throw std::invalid_argument("Render graph: shared input has no producer: " + _node.Name);
    Use(found->second, Graph::Mode::Read);
}

void GDX12RenderGraphBuilder::Write(SharedResource resource, std::string name)
{
    auto found = _graph._shared.find(resource);
    if (found == _graph._shared.end()) found = _graph._shared.emplace(resource, _graph.Create(std::move(name))).first;
    Use(found->second, Graph::Mode::Write);
}

void GDX12RenderGraphBuilder::Fallback(IRenderPassLink* output, IRenderPassLink* input)
{ _graph.AddFallback(Find(output), Find(input)); }

void GDX12RenderGraphBuilder::Export(IRenderPassLink* link) { _graph.Export(Find(link)); }

void GDX12RenderGraph::DependsOn(Pass pass, Pass prerequisite)
{
    CheckPass(pass); CheckPass(prerequisite);
    _nodes[pass].ExplicitDependencies.push_back(prerequisite);
    _compiled = false;
}

void GDX12RenderGraph::Export(Resource resource)
{
    CheckResource(resource);
    _resources[resource].Exported = true;
    _compiled = false;
}

void GDX12RenderGraph::Compile(bool reuseTextures)
{
    _compiled = false;
    _order.clear();
    _statistics = {};
    std::vector<Pass> writers(_resources.size(), UINT_MAX);
    std::vector<std::vector<Pass>> readers(_resources.size());
    for (Pass p = 0; p < _nodes.size(); ++p)
    {
        auto& node = _nodes[p];
        node.Dependencies = node.ExplicitDependencies;
        const auto depend = [&](Pass producer)
        {
            if (producer != UINT_MAX && producer != p) node.Dependencies.push_back(producer);
        };
        // Reads precede writes, including conditional forwarding through other outputs.
        for (const auto& access : node.Accesses)
            if (access.Usage != Mode::Write)
            {
                std::vector<Resource> inputs;
                ExpandRead(access.Id, inputs);
                for (auto id : inputs)
                {
                    if (writers[id] == UINT_MAX && !_resources[id].Imported)
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
    std::function<void(Pass)> retain = [&](Pass p)
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
                if (writers[id] == UINT_MAX && !_resources[id].Imported)
                    throw std::logic_error("Render graph: exported resource has no writer: " + _resources[id].Name);
                retain(writers[id]);
            }
        }
    for (auto p : sorted) if (live[p]) _order.push_back(p);
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
    std::vector<Resource> textures;
    for (Resource r = 0; r < _resources.size(); ++r)
        if (_resources[r].Transient && _resources[r].First != UINT_MAX) textures.push_back(r);
    std::stable_sort(textures.begin(), textures.end(), [&](Resource a, Resource b) { return _resources[a].First < _resources[b].First; });
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
    _compiled = true;
}

void GDX12RenderGraph::Execute(GDX12CommandList* commands)
{
    GDX12_SCOPED_CPU_EVENT("Graph recording");
    if (!_compiled) throw std::logic_error("Render graph must be compiled before execution");
    for (const auto& resource : _resources)
        if (resource.Slot != UINT_MAX && resource.Texture->GetResource() != _slots[resource.Slot].ResourceObject.get())
            throw std::logic_error("Render graph texture changed; recompile after resize: " + resource.Name);
    for (UINT i = 0; i < _order.size(); ++i)
    {
        const auto& node = _nodes[_order[i]];
        GDX12_SCOPED_EVENT(commands, node.Name.c_str());
        for (const auto& resource : _resources)
            if (resource.Slot != UINT_MAX && resource.First == i)
            {
                auto* physical = resource.Texture->GetResource();
                if (physical->D3DResource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
                    commands->ResourceBarrier({ physical->GetUAVBarrier() });
            }
        for (auto access : node.Accesses)
        {
            const auto& entry = _resources[access.Id];
            auto* texture = entry.Link && (access.Usage == Mode::Read || !entry.Texture) ? entry.Link->GetTexture() : entry.Texture;
            if (!texture || access.State == static_cast<D3D12_RESOURCE_STATES>(UINT_MAX)) continue;
            auto* resource = texture->GetResource();
            commands->ResourceBarrier({ CD3DX12_RESOURCE_BARRIER::Transition(resource->D3DResource.Get(), resource->GetCurrentState(), access.State) });
            resource->GDX12Resource::SetCurrentState(access.State);
        }
        node.Instance->Execute(commands);
    }
}

void GDX12RenderGraph::WriteDot(std::ostream& output) const
{
    if (!_compiled) throw std::logic_error("Render graph must be compiled before export");
    output << "digraph RenderGraph {\n";
    for (auto p : _order)
    {
        output << "  p" << p << " [label=" << std::quoted(_nodes[p].Name) << "];\n";
        for (auto predecessor : _nodes[p].Dependencies) output << "  p" << predecessor << " -> p" << p << ";\n";
    }
    output << "}\n";
}
