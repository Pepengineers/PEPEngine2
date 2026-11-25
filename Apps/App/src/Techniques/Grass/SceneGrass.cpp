#include "SceneGrass.h"
#include "BistroGrass.h"
#include "Techniques/Linden/LindenSimulation.h"
#include "Techniques/NativeWork.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/Logging/Logger.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12LocalLights.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include <algorithm>
#include <cassert>
#include <set>

namespace BistroRender
{
namespace
{
AutoConsoleVariable<int> WindAdapter(L"Render.Wind.Adapter", -1,
    L"Hardware GPU index for Bistro ProjectedGrid wind. -1 keeps wind on the primary GPU.",
    EConsoleVariableFlags::Default, [](int value) { return value >= -1; });
AutoConsoleVariable<int> WindCrossSync(L"Render.Wind.CrossSync", 1,
    L"Remote ProjectedGrid wind: 0 exact previous frame (first uses current), 1 exact current frame.",
    EConsoleVariableFlags::Default, [](int value) { return value == 0 || value == 1; });
AutoConsoleVariable<bool> LindenResponseOnSecondary(L"Render.Wind.LindenResponseOnSecondary", false,
    L"Evaluate Linden crown fragments on the adapter running its ProjectedGrid. Deform vertices on the primary GPU.");
// ECS wind owners count upward from one. Reserve the last ID for the scene mesh.
constexpr uint64_t LindenWindOwner = UINT64_MAX;

bool IsDeviceLoss(HRESULT code)
{
	return code == DXGI_ERROR_DEVICE_REMOVED || code == DXGI_ERROR_DEVICE_RESET ||
	    code == DXGI_ERROR_DEVICE_HUNG || code == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

struct WindCopy
{
	ID3D12Resource* Buffer;
	uint64_t Offset, Bytes;
};

XMFLOAT4 CrownArea(const GDX12WindInstance& field)
{
	return {field.CenterCm.x, field.CenterCm.y, field.ExtentsCm.x, field.ExtentsCm.y};
}
}

struct SceneGrass::RemoteWind
{
	struct Field
	{
		GDX12GrassRenderer::WindSample Sample;
		uint64_t Offset, Bytes;
		std::shared_ptr<const LindenMesh> ResponseMesh;
		bool Shown = false;
	};
	struct Frame
	{
		QueueSubmission::TransferBuffer Shared;
		std::vector<Field> Fields;
		uint64_t Capacity = 0, Version = 0;
		CompletionToken Ready, Reader;
	};
	struct DisplayField
	{
		Field* Published;
		bool Repeat;
		bool UseCurrentAsPrevious;
		std::array<Id, 2> Local{Invalid, Invalid};
		Id Consumer = Invalid;
		std::string ConsumerName;
	};
	struct Import
	{
		Frame* Source = nullptr;
		std::vector<DisplayField> Fields;
		Id Operation = Invalid, Packet = Invalid;
	};

	std::shared_ptr<QueueSubmission> Submissions;
	Id Device;
	GDX12DeviceResources Resources;
	std::unique_ptr<GDX12GrassRenderer> Solver;
	std::unique_ptr<LindenResponse> CrownEvaluator;
	std::shared_ptr<const LindenMesh> CrownMesh;
	std::unique_ptr<GDX12UploadBuffer<LindenFragment>> CrownFragments, CrownUpload;
	std::unique_ptr<GDX12UploadBuffer<FragmentPose>> CrownResponse;
	bool CrownUploaded = false;
	Executor Recorder;
	std::vector<Frame> Frames;
	std::array<Import, 2> Imports;
	Frame* Current = nullptr;
	Frame* Previous = nullptr;
	std::set<uint64_t> Reported;
	size_t Next = 0;
	uint64_t Ordered = 0;
	int Mode = -1;
	bool Imported = false;

	RemoteWind(GDX12DeviceResources& primary, Executor& executor, Id device)
	    : Submissions(executor.Submissions()), Device(device),
	      Recorder(Submissions, device, UINT(primary.FrameConstants.size() + 1), 1),
	      Frames(primary.FrameConstants.size() + 1)
	{
		Resources.Device = &Submissions->Device(device);
		Resources.FrameConstants.resize(Frames.size());
		Resources.SRV_UAV_Heap = std::make_shared<GDX12DescriptorHeap>(Resources.Device,
		    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, primary.SRV_UAV_Heap->GetNumDescriptors(),
		    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
		Solver = std::make_unique<GDX12GrassRenderer>(&Resources, 0);
		Solver->InitializeWind();
	}

	void Begin(int mode)
	{
		if (Mode != mode)
		{
			Mode = mode;
			Previous = nullptr;
			Ordered = 0;
			Reported.clear();
		}
		Imports = {};
		const UINT slot = UINT(Next++ % Frames.size());
		Current = &Frames[slot];
		if (Current == Previous) throw std::logic_error("Overwriting the previous wind publication");
		Recorder.Begin(slot);
		Resources.CurrFrameConstantsIndex = slot;
		auto& publication = *Current;
		// The extra slot covers the extra frame that can import this publication.
		if (publication.Reader && !Submissions->Complete(publication.Reader))
			throw std::logic_error("Wind display slot still in use by GPU0");
		publication.Fields.clear();
		publication.Ready = publication.Reader = {};
		auto& frame = Solver->CurrentFrame();
		frame.RetiredFields.clear();
		frame.WindCursor = 0;
		frame.WindJobs.clear();
	}

	void PrepareResponse(const std::shared_ptr<const LindenMesh>& mesh, GDX12DeviceResources& primary,
	                     std::unique_ptr<GDX12UploadBuffer<FragmentPose>>& display)
	{
		const UINT count = UINT(mesh->Fragments.size());
		if (!display || display->GetElementCount() != count)
			display = LindenResponse::CreateBuffer(primary, count, false);
		if (CrownMesh == mesh) return;
		if (CrownMesh) Recorder.Drain();
		if (!CrownEvaluator) CrownEvaluator = std::make_unique<LindenResponse>(Resources);
		CrownFragments = std::make_unique<GDX12UploadBuffer<LindenFragment>>(Resources.Device, count, EBufferType::Default, false);
		auto* heap = Resources.SRV_UAV_Heap.get();
		CrownFragments->CreateSRV(heap, heap->GetAvailableIndex());
		CrownUpload = std::make_unique<GDX12UploadBuffer<LindenFragment>>(Resources.Device, count, EBufferType::Upload, false);
		CrownUpload->CopyData(0, mesh->Fragments.data(), count);
		CrownResponse = LindenResponse::CreateBuffer(Resources, count, true);
		CrownMesh = mesh;
		CrownUploaded = false;
	}

	void AppendResponse(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work,
	                     const GDX12GrassRenderer::WindState& field)
	{
		if (!CrownUploaded)
		{
			auto* upload = CrownUpload->GetResource().D3DResource.Get();
			auto* dest = CrownFragments->GetResource().D3DResource.Get();
			const auto bytes = uint64_t(CrownMesh->Fragments.size()) * sizeof(LindenFragment);
			work.Add(graph, CrownMesh->Label + " secondary attachments upload",
			    {{NativeWork::ResourceFor(graph, bindings, upload, "linden upload"), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
			     {NativeWork::ResourceFor(graph, bindings, dest, "linden attachments"), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
			    [=](auto* cmd) { cmd->CopyBufferRegion(dest, 0, upload, 0, bytes); }, {}, Queue::Compute);
		}
		LindenWindSample wind;
		wind.Grid = field.Velocity[0].get();
		wind.Area = CrownArea(field.Source);
		wind.Time = field.Time;
		wind.Resolution = field.Resolution;
		CrownEvaluator->Append(graph, bindings, work, *CrownMesh, *CrownFragments, wind, *CrownResponse);
	}

	bool Publish(GDX12GrassRenderer& primary, bool response)
	{
		auto& frame = *Current;
		const auto& imports = primary.CurrentFrame().ImportedWind;
		if (imports.empty())
		{
			// An idle frame has no new terminal fence to retire the former solver buffers.
			if (!Solver->_wind.empty())
			{
				Recorder.Drain();
				Solver->_wind.clear();
			}
			Previous = nullptr;
			Ordered = 0;
			Reported.clear();
			return true;
		}
		for (auto it = Solver->_wind.begin(); it != Solver->_wind.end();)
			if (!imports.count(it->first))
			{
				Solver->RetireWind(it->second);
				it = Solver->_wind.erase(it);
			}
			else ++it;

		Graph graph;
		graph.Device = Device;
		graph.FrameBoundary = false;
		graph.TerminalQueue = Queue::Compute;
		std::vector<ID3D12Resource*> bindings;
		NativeWork work;
		SceneGrass::AppendWindJobs(*Solver, graph, bindings, work, true);
		std::vector<WindCopy> copies;
		std::vector<Access> accesses;
		uint64_t bytes = 0;
		for (const auto& entry : imports)
		{
			const auto& field = Solver->_wind.at(entry.first);
			const auto mesh = response && entry.first == LindenWindOwner ? CrownMesh : nullptr;
			const uint64_t size = mesh ? mesh->Fragments.size() * sizeof(FragmentPose) :
			    uint64_t(field.Resolution) * field.Resolution * sizeof(XMFLOAT4);
			frame.Fields.push_back({primary._wind.at(entry.first), bytes, size, mesh});
			const auto addCopy = [&](auto& buffer)
			{
				auto* source = buffer.GetResource().D3DResource.Get();
				accesses.push_back({NativeWork::ResourceFor(graph, bindings, source, "wind publication payload"),
				                    Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE});
				copies.push_back({source, bytes, size});
				bytes += size;
			};
			if (mesh)
			{
				AppendResponse(graph, bindings, work, field);
				addCopy(*CrownResponse);
			}
			else for (const auto& velocity : field.Velocity) addCopy(*velocity);
		}
		if (bytes > frame.Capacity)
		{
			try { frame.Shared = Submissions->CreateTransferBuffer(Device, bytes); }
			catch (const DxException& e)
			{
				if (IsDeviceLoss(e.ErrorCode)) throw;
				Submissions->CheckHealth();
				Log::Warning("RenderGraph", "Using local ProjectedGrid wind: shared wind buffer: ", e.ToString());
				return false;
			}
			catch (const std::exception& e)
			{
				Submissions->CheckHealth();
				Log::Warning("RenderGraph", "Using local ProjectedGrid wind: shared wind buffer: ", e.what());
				return false;
			}
			frame.Capacity = bytes;
		}
		auto* shared = frame.Shared.Resources[1].Get();
		accesses.push_back({NativeWork::ResourceFor(graph, bindings, shared, "shared wind velocity"),
		                    Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST});
		work.Add(graph, "secondary export ProjectedGrid velocity", std::move(accesses),
		    [shared, copies = std::move(copies)](auto* cmd)
		    {
			    for (const auto& copy : copies) cmd->CopyBufferRegion(shared, copy.Offset, copy.Buffer, 0, copy.Bytes);
		    }, {}, Queue::Compute);
		const auto schedule = Compile(graph);
		Recorder.Reserve(schedule);
		Recorder.Execute(schedule, bindings, [&](Id operation, uint32_t, auto* cmd) { work.Record(operation, cmd); });
		if (response) CrownUploaded = true;
		frame.Ready = Recorder.Completions().back();
		frame.Version = ++Ordered;
		SelectDisplay(primary);
		return true;
	}

	void SelectDisplay(GDX12GrassRenderer& primary)
	{
		Imports[0].Source = Previous;
		Imports[1].Source = Current;
		for (auto& ordered : Current->Fields)
		{
			Field* shown = &ordered;
			if (Mode == 0 && Previous && !ordered.Sample.ResetHistory)
			{
				const auto found = std::find_if(Previous->Fields.begin(), Previous->Fields.end(), [&](const auto& field) {
					return field.Sample.Source.Id == ordered.Sample.Source.Id &&
					    field.Sample.SettingsHash == ordered.Sample.SettingsHash &&
					    field.ResponseMesh == ordered.ResponseMesh;
				});
				if (found != Previous->Fields.end()) shown = &*found;
			}
			const bool repeat = shown->Shown;
			shown->Shown = true;
			const bool bootstrap = Current->Version == 1 || (Mode == 0 && shown == &ordered);
			const bool useCurrentAsPrevious = repeat || (Mode == 0 && bootstrap);
			auto& view = static_cast<GDX12GrassRenderer::WindSample&>(primary._wind.at(ordered.Sample.Source.Id));
			view = shown->Sample;
			view.ResetHistory = !repeat && (view.ResetHistory || bootstrap);
			if (useCurrentAsPrevious) view.PreviousTime = view.Time;
			auto& selected = Imports[shown == &ordered ? 1 : 0];
			selected.Fields.push_back({shown, repeat, useCurrentAsPrevious});
		}
		Previous = Current;
	}
};

namespace
{
void CreateGrassGeometry(GDX12Device* device, std::unique_ptr<GDX12RootSignature>& root,
                         ComPtr<ID3D12PipelineState>& clear, ComPtr<ID3D12PipelineState>& cull,
                         ComPtr<ID3D12PipelineState>* build = nullptr)
{
	GDX12RootSignatureDesc geometry;
	geometry.NumSingleCBVSlots = 1;
	geometry.NumSingleSRVSlots = 6;
	geometry.NumSingleUAVSlots = 3;
	geometry.VolatileSRVData = true;
	root = std::make_unique<GDX12RootSignature>(device, geometry);
	D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
	const bool waves = SUCCEEDED(device->GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1,
	                                                                                 &options, sizeof(options))) &&
	                   options.WaveOps;
	const D3D_SHADER_MACRO defines[] = {
	    {"GRASS_WAVE_OPS", waves ? "1" : "0"}, {nullptr, nullptr}};
	if (build)
		*build = GDX12RenderHelpers::CreateComputePipeline(
		    device, root.get(), APP_SHADERS_FOLDER "GrassExpand.hlsl", "BuildCS", defines);
	cull = GDX12RenderHelpers::CreateComputePipeline(device, root.get(),
	                                                          APP_SHADERS_FOLDER "GrassExpand.hlsl", "CullCS", defines);
	clear = GDX12RenderHelpers::CreateComputePipeline(
	    device, root.get(), APP_SHADERS_FOLDER "GrassExpand.hlsl", "ClearCS", defines);
}

auto GrassCullCommands(ID3D12DescriptorHeap* heap, GDX12RootSignature& layout,
                       const ComPtr<ID3D12PipelineState>& clear, const ComPtr<ID3D12PipelineState>& cull,
                       D3D12_GPU_VIRTUAL_ADDRESS parameters, D3D12_GPU_DESCRIPTOR_HANDLE roots,
                       D3D12_GPU_DESCRIPTOR_HANDLE hiz, GDX12UploadBuffer<UINT>& indices,
                       GDX12UploadBuffer<UINT>& arguments, UINT bladeCount)
{
	const auto root = layout.GetRootSignature();
	const UINT b = layout.GetBRootParamIndex(0), t = layout.GetTRootParamIndex(0), u = layout.GetURootParamIndex(0);
	const auto indexUav = indices.GetUAV()->GPUHandle, argsUav = arguments.GetUAV()->GPUHandle;
	const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(arguments.GetResource().D3DResource.Get());
	return [=](ID3D12GraphicsCommandList* cmd)
	{
		cmd->SetDescriptorHeaps(1, &heap);
		cmd->SetComputeRootSignature(root.Get());
		cmd->SetComputeRootConstantBufferView(b, parameters);
		cmd->SetComputeRootDescriptorTable(t, roots);
		cmd->SetComputeRootDescriptorTable(t + 5, hiz);
		cmd->SetComputeRootDescriptorTable(u + 1, indexUav);
		cmd->SetComputeRootDescriptorTable(u + 2, argsUav);
		cmd->SetPipelineState(clear.Get());
		cmd->Dispatch(1, 1, 1);
		cmd->ResourceBarrier(1, &barrier);
		cmd->SetPipelineState(cull.Get());
		cmd->Dispatch((bladeCount + 63) / 64, 1, 1);
	};
}

void RecordGrassView(ID3D12GraphicsCommandList* cmd, const GDX12GraphicsBindings& binding,
                     const GeometryTarget& target, const D3D12_INDEX_BUFFER_VIEW& indices,
                     ID3D12CommandSignature* signature, ID3D12Resource* arguments)
{
	auto* heap = binding.Heap->GetHeap().Get();
	cmd->SetDescriptorHeaps(1, &heap);
	cmd->SetGraphicsRootSignature(binding.Root->GetRootSignature().Get());
	cmd->SetPipelineState(binding.Pipeline.Get());
	for (const auto& [reg, cb] : binding.CBVs)
		cmd->SetGraphicsRootConstantBufferView(binding.Root->GetBRootParamIndex(reg), cb);
	for (const auto& [reg, srv] : binding.SRVs)
		cmd->SetGraphicsRootDescriptorTable(binding.Root->GetTRootParamIndex(reg), srv);
	const D3D12_VIEWPORT vp{float(target.X), float(target.Y), float(target.Width), float(target.Height), 0, 1};
	const D3D12_RECT rect{LONG(target.X), LONG(target.Y), LONG(target.X + target.Width), LONG(target.Y + target.Height)};
	cmd->RSSetViewports(1, &vp);
	cmd->RSSetScissorRects(1, &rect);
	cmd->OMSetRenderTargets(UINT(target.Rtvs.size()), target.Rtvs.data(), FALSE, &target.Dsv);
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->IASetIndexBuffer(&indices);
	cmd->ExecuteIndirect(signature, 1, arguments, 0, nullptr, 0);
}
}

SceneGrass::SceneGrass(RenderPipelineContext& c)
    : _resources(c.Primary), _scene(BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>(), c.Runtime)
{
	_gpu = std::make_unique<GDX12GrassRenderer>(&_resources, SceneDescriptorStart);
	_frames.resize(_resources.FrameConstants.size());
	CreateGrassGeometry(_resources.Device, _geometryRoot, _clearGeometry, _cullGeometry, &_buildGeometry);
	GDX12RootSignatureDesc hiz;
	hiz.Constants = {6};
	hiz.NumSingleSRVSlots = 1;
	hiz.NumSingleUAVSlots = 1;
	_hiZRoot = std::make_unique<GDX12RootSignature>(_resources.Device, hiz);
	_buildHiZ = GDX12RenderHelpers::CreateComputePipeline(_resources.Device, _hiZRoot.get(),
	                                                      APP_SHADERS_FOLDER "GrassHiZ.hlsl", "CS");
}
SceneGrass::~SceneGrass()
{
	if (!_remoteWind) return;
	try { _remoteWind->Recorder.Drain(); }
	catch (...) { (void)_remoteWind.release(); }
}

void SceneGrass::RetireRemoteWind(Executor& primary)
{
	primary.Drain();
	_remoteWind->Recorder.Drain();
	_remoteWind.reset();
}

void SceneGrass::Prepare(RenderPipelineContext& c, float dt, Executor& primary,
                         const std::shared_ptr<const LindenMesh>& mesh, const LindenSimulation& linden)
{
	_scene.CollectScene(dt);
	const auto* crown = mesh ? &mesh->CrownBounds : nullptr;
	const bool enable = crown && BistroGrass::WindEnabled.GetValue() && _scene._controls.WindEnabled &&
	                    _scene._controls.WindQuality > 0;
	if (enable)
	{
		GDX12WindInstance wind;
		static_cast<GDX12WindSettings&>(wind) = BistroGrass::ReadWindSettings(
		    {(std::max)(1.0f, crown->Extents.x), (std::max)(1.0f, crown->Extents.z)});
		wind.Id = LindenWindOwner;
		wind.CenterCm = {crown->Center.x, crown->Center.z};
		wind.DeltaSeconds = _scene._deltaSeconds;
		wind.GustStartSeconds = _scene._gustStart;
		_scene._wind.push_back(wind);
	}
	const bool projected = _scene._controls.WindEnabled && _scene._controls.WindQuality > 0 &&
	    std::any_of(_scene._wind.begin(), _scene._wind.end(), [](const auto& wind) {
		    return wind.Implementation == GDX12WindImplementation::ProjectedGrid;
	    });
	const int requested = projected ? WindAdapter.GetValue() : -1;
	if (_failedWindAdapter != requested) _failedWindAdapter = -1;
	const Id device = requested >= 0 && requested != _failedWindAdapter ?
	    primary.Submissions()->SelectAdapter(requested) : 0;
	if (_remoteWind && _remoteWind->Device != device)
		RetireRemoteWind(primary);
	if (device && !_remoteWind)
	{
		try { _remoteWind = std::make_unique<RemoteWind>(_resources, primary, device); }
		catch (const DxException& e)
		{
			if (IsDeviceLoss(e.ErrorCode)) throw;
			primary.Submissions()->CheckHealth();
			_failedWindAdapter = requested;
			Log::Warning("RenderGraph", "Using local ProjectedGrid wind: ", e.ToString());
		}
		catch (const std::exception& e)
		{
			primary.Submissions()->CheckHealth();
			_failedWindAdapter = requested;
			Log::Warning("RenderGraph", "Using local ProjectedGrid wind: ", e.what());
		}
	}
	if (_remoteWind) _remoteWind->Begin(WindCrossSync.GetValue());
	bool remoteResponse = _remoteWind && enable && _scene._wind.back().Implementation == GDX12WindImplementation::ProjectedGrid &&
	    LindenResponseOnSecondary.GetValue();
	_gpu->BeginFrame(c.Common, _scene._grass, _scene._wind, _scene._controls, _scene._sources, _scene._deltaSeconds,
	                 enable ? LindenWindOwner : 0, _remoteWind ? _remoteWind->Solver.get() : nullptr,
	                 remoteResponse ? LindenWindOwner : 0);
	auto& display = _frames.at(_resources.CurrFrameConstantsIndex).LindenResponse;
	bool responseReady = true;
	if (remoteResponse)
	{
		try { _remoteWind->PrepareResponse(mesh, _resources, display); }
		catch (const DxException& e)
		{
			if (IsDeviceLoss(e.ErrorCode)) throw;
			primary.Submissions()->CheckHealth();
			Log::Warning("RenderGraph", "Using local ProjectedGrid wind: Linden response: ", e.ToString());
			responseReady = false;
		}
		catch (const std::exception& e)
		{
			primary.Submissions()->CheckHealth();
			Log::Warning("RenderGraph", "Using local ProjectedGrid wind: Linden response: ", e.what());
			responseReady = false;
		}
	}
	else display.reset();
	if (_remoteWind && (!responseReady || !_remoteWind->Publish(*_gpu, remoteResponse)))
	{
		// Only transfer allocation can fall back. Earlier frames can still hold both sides of the shared ring.
		RetireRemoteWind(primary);
		_failedWindAdapter = requested;
		_gpu->FallBackToLocalWind(enable ? LindenWindOwner : 0);
		remoteResponse = false;
		display.reset();
	}
	_gpu->ApplyWindHistory();
	LindenWindSample sample;
	sample.Revision = _lindenWind.Revision;
	if (enable)
	{
		const auto& field = _gpu->_wind.at(LindenWindOwner);
		const auto& s = field.Source;
		if (remoteResponse) sample.Response = display.get();
		else sample.Grid = _gpu->Velocity(field)[0].get();
		sample.Area = CrownArea(s);
		sample.Time = field.Time;
		sample.Resolution = field.Resolution;
		if (!_lindenWind.Resolution || field.ResetHistory || sample.Time != _lindenWind.Time ||
		    (sample.Response != nullptr) != (_lindenWind.Response != nullptr)) ++sample.Revision;
	}
	else if (_lindenWind.Resolution) ++sample.Revision;
	_lindenWind = sample;
	_importLindenResponse = sample.Response && linden.NeedsResponse(mesh, sample.Revision);
	_gpu->PublishPrimitives(_resources.Primitives, &_scene);
}

Id SceneGrass::WindExecutionDevice() const
{
	return _remoteWind && _remoteWind->Current && _remoteWind->Current->Ready ? _remoteWind->Device : 0;
}

void SceneGrass::AppendWindImport(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work)
{
	if (!WindExecutionDevice()) return;
	// On a mixed reset frame queue the previous publication's copy before any wait for the current one.
	for (auto& import : _remoteWind->Imports)
	{
		if (import.Fields.empty()) continue;
		auto* shared = import.Source->Shared.Resources[0].Get();
		std::vector<Access> accesses{{NativeWork::ResourceFor(graph, bindings, shared, "shared wind velocity"),
		                              Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE}};
		std::vector<WindCopy> copies;
		for (auto& display : import.Fields)
		{
			const auto& field = *display.Published;
			if (field.ResponseMesh)
			{
				if (!_importLindenResponse) continue;
				auto* target = _lindenWind.Response->GetResource().D3DResource.Get();
				display.Local[0] = NativeWork::ResourceFor(graph, bindings, target, "imported linden fragment response");
				copies.push_back({target, field.Offset, field.Bytes});
				accesses.push_back({display.Local[0], Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST});
				continue;
			}
			const auto& velocity = _gpu->CurrentFrame().ImportedWind.at(field.Sample.Source.Id);
			for (size_t i = 0; i < velocity.size(); ++i)
			{
				auto* target = velocity[i]->GetResource().D3DResource.Get();
				const uint64_t grid = display.UseCurrentAsPrevious ? 0 : i;
				display.Local[i] = NativeWork::ResourceFor(graph, bindings, target, "imported wind velocity");
				copies.push_back({target, field.Offset + grid * field.Bytes, field.Bytes});
				accesses.push_back({display.Local[i], Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST});
			}
		}
		if (copies.empty()) continue;
		import.Operation = work.Add(graph, "GPU0 import ProjectedGrid publication", std::move(accesses),
		    [shared, copies = std::move(copies)](auto* cmd)
		    {
			    for (const auto& copy : copies) cmd->CopyBufferRegion(copy.Buffer, 0, shared, copy.Offset, copy.Bytes);
		    }, {}, Queue::Copy);
	}
}

void SceneGrass::AddWindWaits(const Schedule& schedule, std::vector<PacketWait>& waits)
{
	if (!WindExecutionDevice()) return;
	const bool traceStartup = _remoteWind->Mode == 0 && _remoteWind->Current->Version <= 4;
	for (auto& import : _remoteWind->Imports)
	{
		if (import.Operation == Invalid) continue;
		for (Id index = 0; index < schedule.Packets.size(); ++index)
		{
			const auto& packet = schedule.Packets[index];
			if (packet.Operation == import.Operation)
			{
				import.Packet = index;
				continue;
			}
			for (auto& field : import.Fields)
			{
				if (field.Consumer != Invalid ||
				    (!traceStartup && _remoteWind->Reported.count(field.Published->Sample.Source.Id))) continue;
				if (!std::any_of(packet.Accesses.begin(), packet.Accesses.end(), [&](const auto& access) {
					    return access.Usage == Mode::Read &&
					        (access.Resource == field.Local[0] || access.Resource == field.Local[1]);
				    })) continue;
				if (import.Packet == Invalid)
					throw std::logic_error("Wind field consumed before GPU0 import");
				field.Consumer = index;
				field.ConsumerName = packet.Name;
			}
		}
		if (import.Packet == Invalid) throw std::logic_error("Missing ProjectedGrid import packet");
		waits.push_back({import.Packet, import.Source->Ready});
	}
}

void SceneGrass::WindSubmitted(uint64_t frameNumber, const std::vector<CompletionToken>& completions)
{
	if (!WindExecutionDevice()) return;
	const auto& ordered = *_remoteWind->Current;
	for (auto& import : _remoteWind->Imports)
	{
		if (import.Packet >= completions.size() || !completions[import.Packet]) continue;
		auto& publication = *import.Source;
		publication.Reader = completions[import.Packet];
		if (!_remoteWind->Imported)
		{
			_remoteWind->Submissions->LogFirstImport("ProjectedGrid wind", publication.Ready, publication.Reader);
			_remoteWind->Imported = true;
		}
		for (const auto& field : import.Fields)
		{
			if (field.Local[0] == Invalid) continue;
			const auto& sample = field.Published->Sample;
			const auto owner = sample.Source.Id;
			const bool read = field.Consumer < completions.size() && completions[field.Consumer];
			const bool first = read && _remoteWind->Reported.insert(owner).second;
			if (!first && !(_remoteWind->Mode == 0 && ordered.Version <= 4)) continue;
			if (field.Published->ResponseMesh)
			{
				Log::Info("RenderGraph", "Linden MGPU response CrossSync=", _remoteWind->Mode, " frame=", frameNumber,
				    " mesh=", field.Published->ResponseMesh->Label, " owner=", owner,
				    " ordered=", ordered.Version, " fieldVersion=", publication.Version,
				    " responseVersion=", publication.Version, " bytes=", field.Published->Bytes,
				    " waitExportFence=", publication.Ready.Value, " importFence=", publication.Reader.Value,
				    " EvaluateFragmentsGPU=", _remoteWind->Submissions->AdapterLabel(publication.Ready.Device),
				    " DeformVerticesGPU=", _remoteWind->Submissions->AdapterLabel(publication.Reader.Device),
				    " firstConsumer=", read ? field.ConsumerName.c_str() : "none",
				    " consumerFence=", read ? completions[field.Consumer].Value : 0);
				continue;
			}
			Log::Info("RenderGraph", "ProjectedGrid wind CrossSync=", _remoteWind->Mode, " frame=", frameNumber,
			    " field=", owner == LindenWindOwner ? "Linden" : "Grass", " id=", owner,
			    " grid=", sample.Resolution, " time=", sample.Time,
			    " ordered=", ordered.Version, " imported=", publication.Version, " waitExportFence=", publication.Ready.Value,
			    " orderedExportFence=", ordered.Ready.Value, " importFence=", publication.Reader.Value,
			    " repeat=", field.Repeat, " previousIsCurrent=", field.UseCurrentAsPrevious,
			    " firstConsumer=", read ? field.ConsumerName.c_str() : "none",
			    " consumerFence=", read ? completions[field.Consumer].Value : 0);
		}
	}
}

std::vector<GDX12RenderPrimitive> SceneGrass::Primitives() const
{
	std::vector<GDX12RenderPrimitive> result;
	for (auto id : _gpu->_drawOrder)
	{
		if (const auto* primitive = _resources.Primitives.Get(_gpu->_grass.at(id).Primitive))
		{
			result.push_back(*primitive);
		}
	}
	return result;
}
void SceneGrass::AppendWindJobs(GDX12GrassRenderer& gpu, Graph& g, std::vector<ID3D12Resource*>& all,
                                NativeWork& work, bool remote)
{
	auto* heap = gpu._resources->SRV_UAV_Heap->GetHeap().Get();
	const auto access = [&](auto* buffer, Mode mode, State state)
	{
		return Access{NativeWork::ResourceFor(g, all, buffer->GetResource().D3DResource.Get(), "wind solver resource"),
		              mode, state};
	};
	for (const auto& job : gpu.CurrentFrame().WindJobs)
	{
		const auto root = gpu._windRoot->GetRootSignature();
		const auto pipeline = gpu._windPSO.at(job.Stage);
		const auto cb = job.Constants->GetElementAddress(0);
		const auto output = job.Output->GetUAV()->GPUHandle;
		const auto input = (job.Input ? job.Input : gpu._neutralWind.get())->GetSRV()->GPUHandle;
		const auto pressure = (job.Pressure ? job.Pressure : gpu._neutralWind.get())->GetSRV()->GPUHandle;
		const UINT b = gpu._windRoot->GetBRootParamIndex(0), t = gpu._windRoot->GetTRootParamIndex(0),
		           u = gpu._windRoot->GetURootParamIndex(0), groups = (job.Resolution + 7) / 8;
		std::vector<Access> accesses = {access(job.Constants, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ),
		                                access(job.Output, Mode::ReadWrite, UnorderedAccess)};
		if (job.Input)
		{
			accesses.push_back(access(job.Input, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		}
		if (job.Pressure)
		{
			accesses.push_back(access(job.Pressure, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		}
		work.Add(g, std::string(remote ? "GPU wind stage " : job.Async ? "linden wind stage " : "grass wind stage ") +
		    std::to_string(job.Stage), std::move(accesses),
		    [=](auto* cmd)
		    {
			    cmd->SetDescriptorHeaps(1, &heap);
			    cmd->SetComputeRootSignature(root.Get());
			    cmd->SetPipelineState(pipeline.Get());
			    cmd->SetComputeRootConstantBufferView(b, cb);
			    cmd->SetComputeRootDescriptorTable(t, input);
			    cmd->SetComputeRootDescriptorTable(t + 1, pressure);
			    cmd->SetComputeRootDescriptorTable(u, output);
			    cmd->Dispatch(groups, groups, 1);
		    }, {}, remote || job.Async ? Queue::Compute : Queue::Direct);
	}
}
void SceneGrass::Append(Graph& g, UINT slot, std::vector<ID3D12Resource*>& all, GDX12Descriptor& materials,
                        const std::vector<GeometryTarget>& targets, bool viewsOnly,
                        const std::function<void()>& afterInteraction, const std::function<void()>& afterDepth,
                        const std::function<void()>& afterWind)
{
	auto& gpu = *_gpu;
	auto& frame = gpu.CurrentFrame();
	auto& recorded = _frames.at(slot);
	if (!viewsOnly)
	{
		recorded.Work.Commands.clear();
	}

	const auto resource = [&](ID3D12Resource* native)
	{ return NativeWork::ResourceFor(g, all, native, "grass resource"); };
	const auto access = [&](auto* buffer, Mode mode, State state)
	{ return Access{resource(buffer->GetResource().D3DResource.Get()), mode, state}; };
	const auto add =
	    [&](std::string name, std::vector<Access> accesses, std::function<void(ID3D12GraphicsCommandList*)> command)
	{
		std::vector<Access> unique;
		for (auto item : accesses)
		{
			const auto found =
			    std::find_if(unique.begin(), unique.end(), [&](const auto& a) { return a.Resource == item.Resource; });
			if (found == unique.end())
			{
				unique.push_back(item);
			}
			else if (found->Required != item.Required || found->Usage != item.Usage)
			{
				throw std::logic_error("Conflicting frozen grass access");
			}
		}
		recorded.Work.Add(g, std::move(name), std::move(unique), std::move(command));
	};
	ID3D12DescriptorHeap* heap = _resources.SRV_UAV_Heap->GetHeap().Get();
	if (!viewsOnly)
	{
		for (const auto& [roots, upload] : frame.RootUploads)
		{
			const auto dst = roots->GetResource().D3DResource.Get(), src = upload->GetResource().D3DResource.Get();
			const auto bytes = UINT64(roots->GetElementCount()) * sizeof(XMFLOAT3);
			add("grass root upload",
			    {access(roots, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST),
			     access(upload, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ)},
			    [=](auto* cmd) { cmd->CopyBufferRegion(dst, 0, src, 0, bytes); });
		}
		{
			const auto root = gpu._interactionRoot->GetRootSignature();
			const auto pipeline = gpu._interactionPSO;
			const auto cb = frame.InteractionConstants->GetElementAddress(0);
			const auto sources = frame.Sources->GetSRV()->GPUHandle;
			const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 3> uavs = {gpu._trail->GetUAV()->GPUHandle,
			                                                         gpu._previousTrail->GetUAV()->GPUHandle,
			                                                         gpu._presence->GetUAV()->GPUHandle};
			const UINT b = gpu._interactionRoot->GetBRootParamIndex(0), t = gpu._interactionRoot->GetTRootParamIndex(0),
			           u = gpu._interactionRoot->GetURootParamIndex(0);
			add("grass interaction version " + std::to_string(gpu._interactionRevision),
			    {access(frame.Sources.get(), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ),
			     access(frame.InteractionConstants.get(), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ),
			     access(gpu._trail.get(), Mode::ReadWrite, UnorderedAccess),
			     access(gpu._previousTrail.get(), Mode::Write, UnorderedAccess),
			     access(gpu._presence.get(), Mode::Write, UnorderedAccess)},
			    [=](auto* cmd)
			    {
				    cmd->SetDescriptorHeaps(1, &heap);
				    cmd->SetComputeRootSignature(root.Get());
				    cmd->SetPipelineState(pipeline.Get());
				    cmd->SetComputeRootConstantBufferView(b, cb);
				    cmd->SetComputeRootDescriptorTable(t, sources);
				    for (UINT i = 0; i < 3; ++i)
				    {
					    cmd->SetComputeRootDescriptorTable(u + i, uavs[i]);
				    }
				    cmd->Dispatch(8, 8, 1);
			    });
		}
		if (afterInteraction)
		{
			afterInteraction();
		}
		AppendWindJobs(gpu, g, all, recorded.Work, false);
		AppendWindImport(g, all, recorded.Work);
		if (afterWind) afterWind();
	}
	if (!recorded.HiZ)
	{
		recorded.HiZ = std::make_unique<GDX12UploadBuffer<float>>(_resources.Device, 1, EBufferType::Default, false);
		recorded.HiZ->CreateSRV(_resources.SRV_UAV_Heap.get(),
		                        _resources.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart));
		recorded.HiZ->CreateUAV(_resources.SRV_UAV_Heap.get(),
		                        _resources.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart));
	}
	if (!viewsOnly && !targets.empty())
	{
		recorded.Built.resize(gpu.PatchCount());
		for (UINT patch = 0; patch < gpu.PatchCount(); ++patch)
		{
			const auto output = gpu.PrepareViewInputs(targets.front().View, patch);
			if (!output)
			{
				continue;
			}
			const auto& source = output->State.Source;
			auto& built = recorded.Built[patch];
			GrassLocalBounds(source, false).Transform(built.Bounds, source.World);
			const UINT ribbons = source.Implementation == GDX12GrassImplementation::CrossedRibbons ? 2u : 1u;
			const UINT stride =
			    (source.Segments + 1) * 2 * ribbons + ((std::max)(1u, source.Segments / 2) + 1) * 2 * ribbons + 4;
			const UINT64 count = UINT64(GrassBladeCount(source)) * stride;
			if (count > UINT_MAX / sizeof(GDX12GrassRenderer::Vertex))
			{
				throw std::length_error("Built grass vertices exceed buffer size limit");
			}
			if (!built.Vertices || built.Vertices->GetElementCount() < count)
			{
				built.Vertices = gpu.Buffer<GDX12GrassRenderer::Vertex>(UINT(count), true, true);
			}
			const auto root = _geometryRoot->GetRootSignature();
			const auto pipeline = _buildGeometry;
			const auto cb = output->Constants->GetElementAddress(output->ConstantIndex);
			const auto& rootBuffer = output->State.Roots[slot];
			const auto roots = rootBuffer->GetSRV()->GPUHandle;
			const auto wind = output->CurrentWind->GetSRV()->GPUHandle,
			           previousWind = output->PreviousWind->GetSRV()->GPUHandle;
			const auto trail = gpu._trail->GetSRV()->GPUHandle, previousTrail = gpu._previousTrail->GetSRV()->GPUHandle;
			const auto vertices = built.Vertices->GetUAV()->GPUHandle;
			const UINT b = _geometryRoot->GetBRootParamIndex(0), t = _geometryRoot->GetTRootParamIndex(0),
			           u = _geometryRoot->GetURootParamIndex(0);
			const UINT groups = (GrassBladeCount(source) + 63) / 64;
			add("grass patch " + std::to_string(patch) + " build all LOD vertices",
			    {access(output->Constants, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ),
			     access(rootBuffer.get(), Mode::Read,
			            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			     access(output->CurrentWind, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			     access(output->PreviousWind, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			     access(gpu._trail.get(), Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			     access(gpu._previousTrail.get(), Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			     access(built.Vertices.get(), Mode::Write, UnorderedAccess)},
			    [=](auto* cmd)
			    {
				    cmd->SetDescriptorHeaps(1, &heap);
				    cmd->SetComputeRootSignature(root.Get());
				    cmd->SetPipelineState(pipeline.Get());
				    cmd->SetComputeRootConstantBufferView(b, cb);
				    const D3D12_GPU_DESCRIPTOR_HANDLE inputs[] = {roots, wind, previousWind, trail, previousTrail};
				    for (UINT i = 0; i < 5; ++i)
				    {
					    cmd->SetComputeRootDescriptorTable(t + i, inputs[i]);
				    }
				    cmd->SetComputeRootDescriptorTable(u, vertices);
				    cmd->Dispatch(groups, 1, 1);
			    });
		}
	}
	bool hiZReady = false;
	std::vector<bool> mainExpanded(gpu.PatchCount(), false);
	for (const auto& target : targets)
	{
		if (target.View.Stage == GDX12GeometryStage::GBuffer && afterDepth)
		{
			afterDepth();
		}
		XMFLOAT4X4 clip;
		XMStoreFloat4x4(&clip, target.View.ViewProjection.Transpose());
		const GDX12LocalShadowFrustum frustum(clip);
		for (UINT patch = 0; patch < gpu.PatchCount(); ++patch)
		{
			if (!frustum.Intersects(recorded.Built.at(patch).Bounds))
			{
				continue;
			}
			const auto output = gpu.PrepareViewInputs(target.View, patch);
			if (!output)
			{
				continue;
			}
			const bool mainPair = !viewsOnly && target.View.Key == 1;
			auto* vertices = recorded.Built.at(patch).Vertices.get();
			auto* indices = gpu._indices.get();
			auto* arguments = gpu._arguments.get();
			if (mainPair && patch)
			{
				if (recorded.MainPatches.size() < gpu.PatchCount())
				{
					recorded.MainPatches.resize(gpu.PatchCount());
				}
				auto& buffers = recorded.MainPatches[patch];
				if (!buffers.Indices || buffers.Indices->GetElementCount() < indices->GetElementCount())
				{
					buffers.Indices = gpu.Buffer<UINT>(indices->GetElementCount(), false, true);
				}
				if (!buffers.Arguments)
				{
					buffers.Arguments = gpu.Buffer<UINT>(8, false, true);
				}
				indices = buffers.Indices.get();
				arguments = buffers.Arguments.get();
			}
			const bool reuse = mainPair && target.View.Stage == GDX12GeometryStage::GBuffer;
			if (reuse && !mainExpanded[patch])
			{
				throw std::logic_error("Grass GBuffer requires its matching depth culling");
			}
			if (mainPair && !hiZReady && target.View.SceneDepth)
			{
				struct HiZLevel { UINT Width, Height, SourceWidth, SourceHeight, SourceOffset, Offset; };
				const UINT width = target.View.SceneDepth->GetWidth(), height = target.View.SceneDepth->GetHeight();
				std::vector<HiZLevel> levels{{width, height, 0, 0, 0, 0}};
				UINT64 total = UINT64(width) * height;
				for (UINT span = 1; span < (std::max)(width, height); span *= 2)
				{
					const auto previous = levels.back();
					const UINT w = (previous.Width + 1) / 2, h = (previous.Height + 1) / 2;
					levels.push_back({w, h, previous.Width, previous.Height, previous.Offset, UINT(total)});
					total += UINT64(w) * h;
				}
				if (total > UINT_MAX / sizeof(float))
				{
					throw std::length_error("Grass HiZ exceeds buffer size limit");
				}
				recorded.HiZ->Resize(UINT(total));
				const auto root = _hiZRoot->GetRootSignature();
				const auto pipeline = _buildHiZ;
				const auto depth = target.View.SceneDepth->GetSRV()->GPUHandle, hiz = recorded.HiZ->GetUAV()->GPUHandle;
				const UINT t = _hiZRoot->GetTRootParamIndex(0), u = _hiZRoot->GetURootParamIndex(0);
				const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(recorded.HiZ->GetResource().D3DResource.Get());
				add("grass opaque depth HiZ",
				    {{target.Depth, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
				     access(recorded.HiZ.get(), Mode::ReadWrite, UnorderedAccess)},
				    [=, levels = std::move(levels)](auto* cmd)
				    {
					    cmd->SetDescriptorHeaps(1, &heap);
					    cmd->SetComputeRootSignature(root.Get());
					    cmd->SetPipelineState(pipeline.Get());
					    cmd->SetComputeRootDescriptorTable(t, depth);
					    cmd->SetComputeRootDescriptorTable(u, hiz);
					    for (const auto& level : levels)
					    {
						    if (level.SourceWidth) cmd->ResourceBarrier(1, &barrier);
						    cmd->SetComputeRoot32BitConstants(0, 6, &level, 0);
						    cmd->Dispatch((level.Width + 7) / 8, (level.Height + 7) / 8, 1);
					    }
				    });
				hiZReady = true;
			}
			const std::string name =
			    "grass view " + std::to_string(target.View.Key) + " patch " + std::to_string(patch);
			const auto cb = output->Constants->GetElementAddress(output->ConstantIndex);
			auto* argumentBuffer = arguments->GetResource().D3DResource.Get();
			if (!reuse)
			{
				const auto& state = output->State;
				auto* roots = state.Roots[slot].get();
				add(name + " clear arguments and cull indices",
				    {access(output->Constants, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ),
				     access(roots, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				     access(recorded.HiZ.get(), Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				     access(indices, Mode::Write, UnorderedAccess),
				     access(arguments, Mode::ReadWrite, UnorderedAccess)},
				    GrassCullCommands(heap, *_geometryRoot, _clearGeometry, _cullGeometry, cb,
				                      roots->GetSRV()->GPUHandle, recorded.HiZ->GetSRV()->GPUHandle,
				                      *indices, *arguments, GrassBladeCount(state.Source)));
				if (mainPair) mainExpanded[patch] = true;
			}
			const auto& constants = *_resources.FrameConstants[slot];
			D3D12_GPU_DESCRIPTOR_HANDLE localLights{};
			if (target.View.CaptureLighting)
			{
				localLights = target.View.CaptureLighting->LocalLightSrv;
				if (!localLights.ptr) localLights = constants.ShadowLights->GetSRV()->GPUHandle;
			}
			const auto binding = gpu.DrawBindings(target.View, cb, vertices->GetSRV()->GPUHandle,
			    materials.GPUHandle, constants.MainCB->GetElementAddress(0), localLights);
			const D3D12_INDEX_BUFFER_VIEW indexView{indices->GetElementAddress(0),
			    indices->GetElementCount() * UINT(sizeof(UINT)), DXGI_FORMAT_R32_UINT};
			const auto signature = gpu._signature;
			std::vector<Access> draw = {
			    access(vertices, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			    access(indices, Mode::Read, D3D12_RESOURCE_STATE_INDEX_BUFFER),
			    access(arguments, Mode::Read, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
			    access(output->Constants, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ),
			    {2, Mode::Read,
			     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
			    {target.Depth, target.View.Stage == GDX12GeometryStage::GBuffer ? Mode::Read : Mode::Write,
			     State(target.View.Stage == GDX12GeometryStage::GBuffer ? D3D12_RESOURCE_STATE_DEPTH_READ
			                                                            : D3D12_RESOURCE_STATE_DEPTH_WRITE)}};
			draw.insert(draw.end(), target.AdditionalReads.begin(), target.AdditionalReads.end());
			for (auto color : target.Colors)
			{
				draw.push_back({color, Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET});
			}
			draw.push_back(
			    access(_resources.FrameConstants[slot]->MainCB.get(), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ));
			if (target.Camera != Invalid)
			{
				D3D12_HEAP_PROPERTIES heapType{};
				ThrowIfFailed(all.at(target.Camera)->GetHeapProperties(&heapType, nullptr));
				draw.push_back({target.Camera, Mode::Read,
				                heapType.Type == D3D12_HEAP_TYPE_UPLOAD
				                    ? State(D3D12_RESOURCE_STATE_GENERIC_READ)
				                    : State(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)});
			}
			const char* pass = target.View.Stage == GDX12GeometryStage::DepthNormals ? " DepthNormals ExecuteIndirect" :
			                   " ExecuteIndirect";
			add(name + pass, draw,
			    [=](auto* cmd)
			    {
				    RecordGrassView(cmd, binding, target, indexView, signature.Get(), argumentBuffer);
			    });
		}
	}
}

void SceneGrass::AppendProbeCapture(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work,
                                    ProbeCapture& capture, UINT slot, const GeometryTarget& target,
                                    const GDX12Descriptor& materials, D3D12_GPU_VIRTUAL_ADDRESS main,
                                    D3D12_GPU_DESCRIPTOR_HANDLE localLights, const MirrorVegetationBuffer& mirror)
{
	auto& gpu = *_gpu;
	if (!gpu.PatchCount() || target.View.Filter == GDX12GeometryFilter::Static) return;
	auto& owner = capture.GPU;
	auto* descriptors = capture.Owner.SRV_UAV_Heap.get();
	assert(materials._heap == descriptors);
	const auto access = [&](auto& buffer, Mode mode, State state) {
		return Access{NativeWork::ResourceFor(graph, bindings, buffer.GetResource().D3DResource.Get(),
		                                      "probe grass scratch"), mode, state};
	};
	auto* heap = descriptors->GetHeap().Get();
	const auto& built = _frames.at(slot).Built;
	const auto lighting = target.View.CaptureLighting;
	XMFLOAT4X4 clip;
	XMStoreFloat4x4(&clip, target.View.ViewProjection.Transpose());
	const GDX12LocalShadowFrustum frustum(clip);
	for (UINT patch = 0; patch < gpu.PatchCount(); ++patch)
	{
		if (!frustum.Intersects(built.at(patch).Bounds)) continue;
		const auto view = gpu.PrepareViewInputs(target.View, patch);
		if (!view) continue;
		const bool firstCapture = !capture.Root;
		if (firstCapture)
		{
			owner.InitializeGraphics(true);
			CreateGrassGeometry(capture.Owner.Device, capture.Root, capture.Clear, capture.Cull);
			capture.Indices.resize(_frames.size());
			capture.Arguments = owner.Buffer<UINT>(8, false, true);
			capture.HiZ = std::make_unique<GDX12UploadBuffer<float>>(capture.Owner.Device, 1, EBufferType::Default, false);
			capture.HiZ->CreateSRV(descriptors, descriptors->GetAvailableIndex(SceneDescriptorStart));
		}
		auto& indices = capture.Indices.at(slot);
		if (!indices || indices->GetElementCount() < gpu._indices->GetElementCount())
		{
			indices = owner.Buffer<UINT>(gpu._indices->GetElementCount(), false, true);
		}
		const auto& state = view->State;
		// Capture uses the same all-LOD wind/interaction-deformed vertices as the scene.
		const auto vertices = mirror(built[patch].Vertices->GetResource(), sizeof(GDX12GrassRenderer::Vertex));
		const auto roots = mirror(state.Roots.at(slot)->GetResource(), sizeof(XMFLOAT3));
		const auto parameters = mirror(view->Constants->GetResource(), 0);
		const auto cb = parameters.Native->GetGPUVirtualAddress() +
		    UINT64(view->ConstantIndex) * view->Constants->GetElementSize();
		const auto arguments = access(*capture.Arguments, Mode::ReadWrite, UnorderedAccess);
		auto* argumentBuffer = capture.Arguments->GetResource().D3DResource.Get();
		work.Add(graph, "probe owner grass clear arguments and cull indices",
		         {{parameters.Resource, Mode::Read, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER},
		          {roots.Resource, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
		          access(*capture.HiZ, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
		          access(*indices, Mode::Write, UnorderedAccess), arguments},
		         GrassCullCommands(heap, *capture.Root, capture.Clear, capture.Cull, cb,
		                           roots.Srv, capture.HiZ->GetSRV()->GPUHandle,
		                           *indices, *capture.Arguments, GrassBladeCount(state.Source)));
		const auto signature = owner._signature;
		const D3D12_INDEX_BUFFER_VIEW indexView{indices->GetElementAddress(0),
		                                     indices->GetElementCount() * UINT(sizeof(UINT)), DXGI_FORMAT_R32_UINT};
		const auto binding = owner.DrawBindings(target.View, cb, vertices.Srv, materials.GPUHandle,
		                                        lighting ? main : 0, localLights);
		auto draws = target.AdditionalReads;
		draws.insert(draws.end(), {{vertices.Resource, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
		                          {parameters.Resource, Mode::Read, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER},
		                          access(*indices, Mode::Read, D3D12_RESOURCE_STATE_INDEX_BUFFER),
		                          {arguments.Resource, Mode::Read, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT},
		                          {target.Depth, Mode::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE}});
		for (auto color : target.Colors) draws.push_back({color, Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET});
		work.Add(graph, lighting ? "probe owner grass HDR capture" : "shadow owner grass depth", std::move(draws), [=](auto* cmd) {
			RecordGrassView(cmd, binding, target, indexView, signature.Get(), argumentBuffer);
		});
		if (firstCapture && lighting)
			Log::Info("RenderGraph", "Queued remote reflection grass capture owner=", graph.Device,
			          " view=", target.View.Key, " patch=", patch);
	}
}

bool SceneGrass::Record(UINT slot, Id id, ID3D12GraphicsCommandList* cmd) const
{
	return _frames.at(slot).Work.Record(id, cmd);
}

} // namespace BistroRender
