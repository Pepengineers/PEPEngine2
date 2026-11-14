#pragma once
#include "Graph/MetaGraph.h"
#include "Engine.RendererDX12/GDX12Resource.h"
#include <algorithm>
#include <functional>
#include <stdexcept>

namespace Meta
{
// Native work descriptions are completed on the owner before Executor::Execute.
// All resources have a single physical binding, including aliases via texture views.
struct NativeWork
{
    std::vector<std::pair<Id, std::function<void(ID3D12GraphicsCommandList *)>>> Commands;
    static Id ResourceFor(Graph &graph, std::vector<ID3D12Resource *> &bindings, ID3D12Resource *resource,
                          const std::string &name)
    {
        const auto found = std::find(bindings.begin(), bindings.end(), resource);
        if (found != bindings.end())
            return Id(found - bindings.begin());
        if (!resource || bindings.size() != graph.Resources.size())
            throw std::logic_error("Invalid native technique resource");
        D3D12_HEAP_PROPERTIES heap{};
        ThrowIfFailed(resource->GetHeapProperties(&heap, nullptr));
        const State initial = heap.Type == D3D12_HEAP_TYPE_UPLOAD     ? State(D3D12_RESOURCE_STATE_GENERIC_READ)
                              : heap.Type == D3D12_HEAP_TYPE_READBACK ? State(D3D12_RESOURCE_STATE_COPY_DEST)
                                                                      : 0;
        const auto shape = resource->GetDesc();
        const Id id = Id(bindings.size());
        graph.Resources.push_back(
            {name, initial, initial, (shape.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0,
             heap.Type == D3D12_HEAP_TYPE_DEFAULT && shape.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER});
        bindings.push_back(resource);
        return id;
    }
    // Renderer-owned assets are already published in SRV state by their upload
    // path. Import that boundary without changing the wrapper or claiming COMMON.
    static Id ImportReadOnlyTexture(Graph &graph, std::vector<ID3D12Resource *> &bindings,
                                    GDX12Resource &resource, State published, const std::string &name)
    {
        auto *native = resource.D3DResource.Get();
        if (!native || bindings.size() != graph.Resources.size() ||
            native->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            State(resource.GetCurrentState()) != published)
            throw std::logic_error("Invalid published texture import");
        D3D12_HEAP_PROPERTIES heap{};
        ThrowIfFailed(native->GetHeapProperties(&heap, nullptr));
        if (heap.Type != D3D12_HEAP_TYPE_DEFAULT)
            throw std::logic_error("Published texture import requires a DEFAULT texture");
        const auto found = std::find(bindings.begin(), bindings.end(), native);
        if (found != bindings.end())
        {
            const Id id = Id(found - bindings.begin());
            const auto &declaration = graph.Resources.at(id);
            if (declaration.Initial != published || declaration.Final != published || declaration.Decays)
                throw std::logic_error("Conflicting published texture boundary");
            for (const auto &operation : graph.Operations)
                for (auto access : operation.Accesses)
                    if (access.Resource == id && (access.Usage != Mode::Read || access.Required != published))
                        throw std::logic_error("Published texture import conflicts with an earlier writer/state");
            return id;
        }
        const Id id = Id(bindings.size());
        graph.Resources.push_back({name, published, published});
        bindings.push_back(native);
        return id;
    }
    Id Add(Graph &graph, std::string name, std::vector<Access> accesses,
           std::function<void(ID3D12GraphicsCommandList *)> record, std::vector<Id> dependencies = {})
    {
        const Id id = Id(graph.Operations.size());
        graph.Operations.push_back({std::move(name), std::move(accesses), std::move(dependencies)});
        Commands.emplace_back(id, std::move(record));
        return id;
    }
    bool Record(Id id, ID3D12GraphicsCommandList *commands) const
    {
        const auto found =
            std::find_if(Commands.begin(), Commands.end(), [id](const auto &entry) { return entry.first == id; });
        if (found == Commands.end())
            return false;
        found->second(commands);
        return true;
    }
};
} // namespace Meta
