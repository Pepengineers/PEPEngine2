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
