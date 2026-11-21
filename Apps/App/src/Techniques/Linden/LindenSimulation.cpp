#include "LindenSimulation.h"
#include "Techniques/Grass/SceneGrass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <type_traits>

namespace BistroRender
{
namespace
{
template<class T>
auto Buffer(GDX12DeviceResources& resources, UINT count, bool writable)
{
	auto buffer = std::make_unique<GDX12UploadBuffer<T>>(resources.Device, count, EBufferType::Default, false);
	auto* heap = resources.SRV_UAV_Heap.get();
	buffer->CreateSRV(heap, heap->GetAvailableIndex(SceneDescriptorStart));
	if (writable) buffer->CreateUAV(heap, heap->GetAvailableIndex(SceneDescriptorStart));
	return buffer;
}
struct Parameters
{
	XMFLOAT4 Area;
	float Time;
	UINT Resolution, Fragments, Vertices;
};
static_assert(sizeof(Parameters) == 8 * sizeof(UINT));

auto LindenDispatch(GDX12DeviceResources& resources, GDX12RootSignature& root, Parameters p,
                    ComPtr<ID3D12PipelineState> pipeline, std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 3> inputs,
                    D3D12_GPU_DESCRIPTOR_HANDLE dest, UINT groups)
{
	auto* heap = resources.SRV_UAV_Heap->GetHeap().Get();
	return [&root, heap, p, pipeline = std::move(pipeline), inputs, dest, groups](ID3D12GraphicsCommandList* cmd)
	{
		cmd->SetDescriptorHeaps(1, &heap);
		cmd->SetComputeRootSignature(root.GetRootSignature().Get());
		cmd->SetPipelineState(pipeline.Get());
		cmd->SetComputeRoot32BitConstants(0, 8, &p, 0);
		for (UINT i = 0; i < inputs.size(); ++i)
			if (inputs[i].ptr) cmd->SetComputeRootDescriptorTable(root.GetTRootParamIndex(i), inputs[i]);
		cmd->SetComputeRootDescriptorTable(root.GetURootParamIndex(0), dest);
		cmd->Dispatch(groups, 1, 1);
	};
}
}

LindenResponse::LindenResponse(GDX12DeviceResources& resources) : _resources(resources)
{
	GDX12RootSignatureDesc desc;
	desc.Constants = {8};
	desc.NumSingleSRVSlots = 3;
	desc.NumSingleUAVSlots = 1;
	desc.VolatileSRVData = true;
	_root = std::make_unique<GDX12RootSignature>(resources.Device, desc);
	_pipeline = GDX12RenderHelpers::CreateComputePipeline(resources.Device, _root.get(),
	                                                     APP_SHADERS_FOLDER "LindenWind.hlsl", "EvaluateFragments");
}

std::unique_ptr<GDX12UploadBuffer<FragmentPose>> LindenResponse::CreateBuffer(
    GDX12DeviceResources& resources, UINT count, bool writable)
{
	return Buffer<FragmentPose>(resources, count, writable);
}

void LindenResponse::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work,
                            const LindenMesh& mesh, GDX12UploadBuffer<LindenFragment>& fragments,
                            const LindenWindSample& wind, GDX12UploadBuffer<FragmentPose>& response)
{
	const Parameters p{wind.Area, wind.Time, wind.Resolution, UINT(mesh.Fragments.size()), UINT(mesh.Vertices.size())};
	const auto bind = [&](auto& buffer, const char* name) {
		return NativeWork::ResourceFor(graph, bindings, buffer.GetResource().D3DResource.Get(), name);
	};
	constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	std::vector<Access> reads{{bind(fragments, "linden attachments"), Mode::Read, read},
	                          {bind(response, "linden fragment response"), Mode::Write, UnorderedAccess}};
	auto field = fragments.GetSRV()->GPUHandle;
	if (p.Resolution)
	{
		if (!wind.Grid) throw std::logic_error("Linden evaluation requires the selected wind grid");
		reads.push_back({bind(*wind.Grid, "linden wind grid"), Mode::Read, read});
		field = wind.Grid->GetSRV()->GPUHandle;
	}
	work.Add(graph, mesh.Label + " evaluate crown and flutter", std::move(reads),
	         LindenDispatch(_resources, *_root, p, _pipeline, {fragments.GetSRV()->GPUHandle, field, {}},
	                       response.GetUAV()->GPUHandle, (p.Fragments + 63) / 64), {}, Queue::Compute);
}

LindenSimulation::LindenSimulation(GDX12DeviceResources& resources)
    : _resources(resources), _evaluation(resources), _frames(resources.FrameConstants.size())
{
	const D3D_SHADER_MACRO deform[] = {{"LINDEN_DEFORM", "1"}, {nullptr, nullptr}};
	_deform = GDX12RenderHelpers::CreateComputePipeline(resources.Device, _evaluation.Root(),
	                                                   APP_SHADERS_FOLDER "LindenWind.hlsl", "DeformVertices", deform);
}

LindenDisplayedPose LindenSimulation::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work,
                                            const std::shared_ptr<const LindenMesh>& mesh, const LindenWindSample& wind)
{
	auto& frame = _frames.at(_resources.CurrFrameConstantsIndex);
	frame = {};
	const bool first = !_resident || _resident->Mesh != mesh;
	const auto access = [&](auto* buffer, Mode mode, State state, const char* name)
	{ return Access{NativeWork::ResourceFor(graph, bindings, buffer->GetResource().D3DResource.Get(), name), mode, state}; };
	if (first)
	{
		frame.Retired = std::move(_resident);
		if (!mesh) { _pendingRevision = 0; return {}; }
		_resident = std::make_unique<Resident>();
		_resident->Mesh = mesh;
		auto upload = [&](const auto& values, auto& staging, auto& destination, const char* name)
		{
			using T = typename std::decay_t<decltype(values)>::value_type;
			const auto count = static_cast<UINT>(values.size());
			destination = Buffer<T>(_resources, count, false);
			staging = std::make_unique<GDX12UploadBuffer<T>>(_resources.Device, count, EBufferType::Upload, false);
			staging->CopyData(0, values.data(), count);
			const auto src = access(staging.get(), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ, name);
			const auto dst = access(destination.get(), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, name);
			auto* srcResource = bindings[src.Resource];
			auto* dstResource = bindings[dst.Resource];
			work.Add(graph, name, {src, dst}, [=](auto* cmd)
			{ cmd->CopyBufferRegion(dstResource, 0, srcResource, 0, UINT64(count) * sizeof(T)); }, {}, Queue::Compute);
		};
		upload(mesh->Vertices, frame.RestUpload, _resident->Rest, "linden rest vertices upload");
		upload(mesh->Fragments, frame.FragmentUpload, _resident->Fragments, "linden attachments upload");
		_resident->Response = Buffer<FragmentPose>(_resources, UINT(mesh->Fragments.size()), true);
		for (auto& pose : _resident->Poses) pose = Buffer<LindenPoseVertex>(_resources, UINT(mesh->Vertices.size()), true);
		_lastComputed = _lastDisplayed = 0;
	}
	auto& tree = *_resident;
	if (!first && wind.Revision == _pendingRevision)
	{
		auto* previous = tree.Poses[_lastDisplayed].get();
		_lastDisplayed = _lastComputed;
		return {tree.Poses[_lastComputed].get(), previous, _pendingRevision};
	}
	const UINT output = first ? 0 : (_lastComputed + 1) % UINT(tree.Poses.size());
	const UINT displayed = first ? output : _lastComputed;
	const UINT previous = first ? displayed : _lastDisplayed;
	const uint64_t displayedRevision = first ? 0 : _pendingRevision;
	auto* response = first ? nullptr : wind.Response;
	if (!response)
	{
		auto sample = wind;
		if (first) sample.Resolution = 0;
		response = tree.Response.get();
		_evaluation.Append(graph, bindings, work, *mesh, *tree.Fragments, sample, *response);
	}
	const Parameters p{wind.Area, wind.Time, first ? 0u : wind.Resolution,
	                   UINT(mesh->Fragments.size()), UINT(mesh->Vertices.size())};
	constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	const auto fragments = access(tree.Fragments.get(), Mode::Read, read, "linden attachments");
	work.Add(graph, mesh->Label + " deform next pose",
	         {fragments, access(tree.Rest.get(), Mode::Read, read, "linden rest vertices"),
	          access(response, Mode::Read, read, "linden fragment response"),
	          access(tree.Poses[output].get(), Mode::Write, UnorderedAccess, "linden deformed vertices")},
	         LindenDispatch(_resources, *_evaluation.Root(), p, _deform,
	                  {tree.Fragments->GetSRV()->GPUHandle, tree.Rest->GetSRV()->GPUHandle, response->GetSRV()->GPUHandle},
	                  tree.Poses[output]->GetUAV()->GPUHandle, (p.Vertices + 63) / 64), {}, Queue::Compute);
	// The frame terminal joins compute before the next frame. Keep both poses consumed by graphics intact this frame.
	_lastComputed = output;
	_lastDisplayed = displayed;
	_pendingRevision = first ? 0 : wind.Revision;
	return {tree.Poses[displayed].get(), tree.Poses[previous].get(), displayedRevision};
}
}
