#pragma once
#include "Graph/RenderGraph.h"
#include "Engine.RendererDX12/GDX12Resource.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include <algorithm>
#include <functional>
#include <stdexcept>

namespace BistroRender
{
constexpr UINT SceneDescriptorStart = TextureCube_StartIndex + TextureCube_RangeLength;
// Native work descriptions are completed on the owner before Executor::Execute.
// All resources have a single physical binding, including aliases via texture views.
struct NativeWork
{
	std::vector<std::pair<Id, std::function<void(ID3D12GraphicsCommandList*)>>> Commands;
	static Id ResourceFor(Graph& graph, std::vector<ID3D12Resource*>& bindings, ID3D12Resource* resource,
	                      const std::string& name)
	{
		const auto found = std::find(bindings.begin(), bindings.end(), resource);
		if (found != bindings.end())
		{
			return Id(found - bindings.begin());
		}
		if (!resource || bindings.size() != graph.Resources.size())
		{
			throw std::logic_error("Invalid native technique resource");
		}
		D3D12_HEAP_PROPERTIES heap{};
		ThrowIfFailed(resource->GetHeapProperties(&heap, nullptr));
		const State initial = heap.Type == D3D12_HEAP_TYPE_UPLOAD     ? State(D3D12_RESOURCE_STATE_GENERIC_READ)
		                      : heap.Type == D3D12_HEAP_TYPE_READBACK ? State(D3D12_RESOURCE_STATE_COPY_DEST)
		                                                              : 0;
		const auto shape = resource->GetDesc();
		const Id id = Id(bindings.size());
		graph.Resources.push_back(
		    {name, initial, initial, (shape.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0,
		     heap.Type == D3D12_HEAP_TYPE_DEFAULT && shape.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER,
		     heap.Type == D3D12_HEAP_TYPE_UPLOAD     ? Heap::Upload
		     : heap.Type == D3D12_HEAP_TYPE_READBACK ? Heap::Readback
		                                             : Heap::Default});
		bindings.push_back(resource);
		return id;
	}
	// Renderer-owned assets are already published in SRV state by their upload
	// path. Import that boundary without changing the wrapper or claiming COMMON.
	static Id ImportReadOnlyTexture(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Resource& resource,
	                                State published, const std::string& name)
	{
		auto* native = resource.D3DResource.Get();
		if (!native || bindings.size() != graph.Resources.size() ||
		    native->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
		    State(resource.GetCurrentState()) != published)
		{
			throw std::logic_error("Invalid published texture import");
		}
		D3D12_HEAP_PROPERTIES heap{};
		ThrowIfFailed(native->GetHeapProperties(&heap, nullptr));
		if (heap.Type != D3D12_HEAP_TYPE_DEFAULT)
		{
			throw std::logic_error("Published texture import requires a DEFAULT texture");
		}
		const auto found = std::find(bindings.begin(), bindings.end(), native);
		if (found != bindings.end())
		{
			const Id id = Id(found - bindings.begin());
			const auto& declaration = graph.Resources.at(id);
			if (declaration.Initial != published || declaration.Final != published || declaration.Decays)
			{
				throw std::logic_error("Conflicting published texture boundary");
			}
			for (const auto& operation : graph.Operations)
			{
				for (auto access : operation.Accesses)
				{
					if (access.Resource == id && (access.Usage != Mode::Read || access.Required != published))
					{
						throw std::logic_error("Published texture import conflicts with an earlier writer/state");
					}
				}
			}
			return id;
		}
		const Id id = Id(bindings.size());
		graph.Resources.push_back({name, published, published});
		bindings.push_back(native);
		return id;
	}
	Id Add(Graph& graph, std::string name, std::vector<Access> accesses,
	       std::function<void(ID3D12GraphicsCommandList*)> record, std::vector<Id> dependencies = {})
	{
		const Id id = Id(graph.Operations.size());
		graph.Operations.push_back({std::move(name), std::move(accesses), std::move(dependencies)});
		Commands.emplace_back(id, std::move(record));
		return id;
	}
	Id AddOwned(Graph& graph, std::string name, std::vector<Access> accesses,
	            const std::vector<ID3D12Resource*>& bindings, std::function<void(GDX12CommandList*)> record,
	            const char* signalError, std::vector<Id> dependencies = {})
	{
		std::vector<GDX12RecordingStates::Binding> states;
		states.reserve(accesses.size());
		for (const auto& access : accesses)
		{
			states.emplace_back(bindings.at(access.Resource), D3D12_RESOURCE_STATES(access.Required));
		}
		const Id operation = Add(
		    graph, std::move(name), std::move(accesses),
		    [states = std::move(states), record = std::move(record), signalError](auto* commands)
		    {
			    GDX12RecordingStates tracker(states);
			    GDX12CommandList borrowed(commands);
			    record(&borrowed);
			    if (borrowed.HasSubmissionSignals())
			    {
				    throw std::logic_error(signalError);
			    }
			    tracker.Finish(commands);
		    },
		    std::move(dependencies));
		graph.Operations.at(operation).OwnerRecording = true;
		graph.Operations.at(operation).Target = Queue::Direct;
		return operation;
	}
	bool Record(Id id, ID3D12GraphicsCommandList* commands) const
	{
		const auto found = std::lower_bound(Commands.begin(), Commands.end(), id,
		                                    [](const auto& entry, Id operation) { return entry.first < operation; });
		if (found == Commands.end() || found->first != id)
		{
			return false;
		}
		found->second(commands);
		return true;
	}
};
} // namespace BistroRender
