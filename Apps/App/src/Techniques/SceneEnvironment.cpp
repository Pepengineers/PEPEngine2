#include "SceneEnvironment.h"
#include "Common/ConsoleVariables.h"
#include "NativeWork.h"
#include "Engine.Core/Logging/Logger.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace BistroRender
{
namespace
{
constexpr State Read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
AutoConsoleVariable<bool> AsyncEnvironment(L"App.Async.Environment", false,
                                           L"Run atmosphere, clouds and environment IBL kernels on the local COMPUTE queue.",
                                           EConsoleVariableFlags::Default);
AutoConsoleVariable<int> SkyViewAdapter(L"App.Sky.SkyView.Adapter", -1,
    L"Hardware GPU for the camera SkyView LUT. -1 keeps SkyView on the primary GPU.",
    EConsoleVariableFlags::Default, [](int value) { return value >= -1; });
AutoConsoleVariable<int> SkyViewCrossSync(L"App.Sky.SkyView.CrossSync", 1,
    L"Remote camera SkyView: 0 exact previous, 1 exact current, 3 latest completed with local bootstrap.",
    EConsoleVariableFlags::Default, [](int value) { return value == 0 || value == 1 || value == 3; });
AutoConsoleVariable<int> AerialAdapter(L"App.Sky.AerialPerspective.Adapter", -1,
    L"Hardware GPU for the active camera's aerial perspective volumes. -1 keeps them on the primary GPU.",
    EConsoleVariableFlags::Default, [](int value) { return value >= -1; });
AutoConsoleVariable<int> AerialCrossSync(L"App.Sky.AerialPerspective.CrossSync", 1,
    L"Remote aerial perspective: 1 imports the current camera's matching volume pair.",
    EConsoleVariableFlags::Default, [](int value) { return value == 1; });

bool Different(const GDX12SkyConstants& a, const GDX12SkyConstants& b)
{
	return std::memcmp(&a, &b, sizeof(a)) != 0;
}

bool DeviceLost(HRESULT code)
{
	return code == DXGI_ERROR_DEVICE_REMOVED || code == DXGI_ERROR_DEVICE_RESET ||
	    code == DXGI_ERROR_DEVICE_HUNG || code == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}
}

struct SceneEnvironment::RemoteAtmosphere
{
	using Textures = std::array<GDX12Texture*, 4>;
	struct Inputs
	{
		GDX12DeviceResources Resources;
		std::array<GDX12Texture, 2> Luts;
		GDX12SkyConstants Medium, Multiple;
		uint64_t Version = 0;

		Inputs(GDX12Device& device, const Textures& source)
		{
			Resources.Device = &device;
			Resources.SRV_UAV_Heap = std::make_shared<GDX12DescriptorHeap>(&device,
			    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, TextureResources_StartIndex + TextureResources_RangeLength,
			    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
			for (size_t i = 0; i < Luts.size(); ++i)
				GDX12RenderHelpers::InitializeComputeTexture(Luts[i], Resources, source[i]->GetWidth(),
				    source[i]->GetHeight(), source[i]->GetFormat(), false);
		}
	};
	struct View
	{
		GDX12SkyConstants Key;
		uint64_t Version = 0, SourceFrame = 0, Inputs = 0;
		CompletionToken Ready;
	};
	struct Publication : View
	{
		QueueSubmission::TransferBuffer Shared;
		std::unique_ptr<GDX12UploadBuffer<GDX12SkyConstants>> Constants;
		std::unique_ptr<GDX12UploadBuffer<GDX12CameraConstants>> Camera;
		uint64_t Generation = 0;
		CompletionToken Reader;
	};

	std::shared_ptr<QueueSubmission> Submit;
	Id Device;
	const UINT OutputCount;
	std::shared_ptr<Inputs> Input;
	std::array<GDX12Texture, 2> Outputs;
	std::unique_ptr<GDX12RootSignature> Root;
	std::array<ComPtr<ID3D12PipelineState>, 3> Pipelines;
	std::vector<Publication> Ring;
	Executor Recorder;
	std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> Footprints{};
	uint64_t Bytes = 0, Payload = 0, Ordered = 0, Imported = 0;
	uint64_t Generation = 0, CameraRevision = 0, SkyRevision = 0;
	int Policy = -1;
	UINT Next = 0;
	GDX12SkyConstants CompatibleMedium, CompatibleMultiple;
	Publication *Latest = nullptr, *Selected = nullptr;
	std::optional<View> Displayed;
	NativeWork RemoteWork, PrimaryWork;
	Schedule Plan;
	std::vector<ID3D12Resource*> Bindings;
	std::array<Id, 2> DisplayResources{Invalid, Invalid};
	Id ImportOperation = Invalid, ImportPacket = Invalid, Consumer = Invalid;
	UINT RecordedCopies = 0;
	std::string ConsumerName;
	unsigned Reports = 0;

	RemoteAtmosphere(Executor& primary, Id device, UINT slots, const Textures& source, const RemoteAtmosphere* peer)
	    : Submit(primary.Submissions()), Device(device), OutputCount(source[3] ? 2 : 1),
	      Input(peer && peer->Device == device && peer->MatchesInputs(source)
	          ? peer->Input : std::make_shared<Inputs>(Submit->Device(device), source)),
	      Ring(slots + 2), Recorder(Submit, device, slots + 2, 1)
	{
		auto& resources = Input->Resources;
		GDX12RootSignatureDesc root;
		root.NumSingleCBVSlots = Aerial() ? 2 : 1;
		root.NumSingleSRVSlots = 2;
		root.NumSingleUAVSlots = Aerial() ? 3 : 1;
		root.StaticSamplers.emplace_back(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
		    D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
		Root = std::make_unique<GDX12RootSignature>(resources.Device, root);
		const char* entries[]{"TransmittanceCS", "MultipleScatteringCS", Aerial() ? "AerialCS" : "SkyViewCS"};
		for (size_t i = 0; i < Pipelines.size(); ++i)
			Pipelines[i] = GDX12RenderHelpers::CreateComputePipeline(resources.Device, Root.get(),
			    SHADERS_FOLDER "SkyAtmosphere.hlsl", entries[i]);
		for (UINT i = 0; i < OutputCount; ++i)
		{
			const auto desc = source[i + 2]->GetResource()->D3DResource->GetDesc();
			if (Aerial())
			{
				GDX12TextureDesc texture;
				texture.Width = UINT(desc.Width);
				texture.Height = desc.Height;
				texture.Format = texture.SRVDesc.Format = texture.UAVDesc.Format = desc.Format;
				texture.CreateUAV = true;
				texture.SRV_UAV_Heap = resources.SRV_UAV_Heap.get();
				texture.SRVHeapIndex = texture.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
				texture.UAVHeapIndex = texture.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
				texture.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				texture.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
				texture.SRVDesc.Texture3D.MipLevels = 1;
				texture.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
				texture.UAVDesc.Texture3D.WSize = desc.DepthOrArraySize;
				texture.ExternalResource = resources.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc,
				    D3D12_RESOURCE_STATE_COMMON, nullptr);
				Outputs[i].Initialize(texture);
			}
			else
				GDX12RenderHelpers::InitializeComputeTexture(Outputs[i], resources, UINT(desc.Width), desc.Height, desc.Format);
			const uint64_t offset = (Bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
			    ~uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
			auto& footprint = Footprints[i];
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT remote{};
			uint64_t bytes = 0, remoteBytes = 0, rowBytes = 0;
			UINT rows = 0;
			Submit->Device(0).GetDevice()->GetCopyableFootprints(&desc, 0, 1, offset, &footprint, &rows, &rowBytes, &bytes);
			resources.Device->GetDevice()->GetCopyableFootprints(&desc, 0, 1, offset, &remote, nullptr, nullptr, &remoteBytes);
			if (!bytes || bytes == UINT64_MAX || bytes != remoteBytes || footprint.Offset != remote.Offset ||
			    footprint.Footprint.RowPitch != remote.Footprint.RowPitch)
				throw std::runtime_error("Atmosphere adapters need matching texture footprints");
			// Reserve full row strides for each depth slice, including final-row padding.
			Bytes = footprint.Offset + uint64_t(footprint.Footprint.RowPitch) * rows * footprint.Footprint.Depth;
			Payload += rowBytes * rows * footprint.Footprint.Depth;
		}
	}

	bool Aerial() const { return OutputCount == 2; }

	static bool SameShape(GDX12Texture& a, GDX12Texture& b)
	{
		const auto left = a.GetResource()->D3DResource->GetDesc(), right = b.GetResource()->D3DResource->GetDesc();
		return left.Dimension == right.Dimension && left.Width == right.Width && left.Height == right.Height &&
		    left.DepthOrArraySize == right.DepthOrArraySize && left.Format == right.Format;
	}

	bool MatchesInputs(const Textures& source) const
	{
		return SameShape(Input->Luts[0], *source[0]) && SameShape(Input->Luts[1], *source[1]);
	}

	bool Matches(const Textures& source)
	{
		if (!MatchesInputs(source)) return false;
		for (UINT i = 0; i < OutputCount; ++i)
			if (!SameShape(Outputs[i], *source[i + 2])) return false;
		return true;
	}

	void Configure(int mode, uint64_t camera, uint64_t sky, const GDX12SkyConstants& medium,
	               const GDX12SkyConstants& multiple, bool invalidated)
	{
		if (!invalidated && Policy == mode && CameraRevision == camera && SkyRevision == sky &&
		    !Different(CompatibleMedium, medium) && !Different(CompatibleMultiple, multiple)) return;
		Policy = mode;
		CameraRevision = camera;
		SkyRevision = sky;
		CompatibleMedium = medium;
		CompatibleMultiple = multiple;
		++Generation;
		Latest = Selected = nullptr;
		Displayed.reset();
		Ordered = Imported = Reports = 0;
	}

	void SelectCompleted()
	{
		Selected = nullptr;
		uint64_t version = Displayed ? Displayed->Version : 0;
		for (auto& slot : Ring)
		{
			if (slot.Generation != Generation || slot.Version <= version || !Submit->Complete(slot.Ready)) continue;
			Selected = &slot;
			version = slot.Version;
		}
	}

	std::optional<UINT> BeginOrder()
	{
		const UINT size = UINT(Ring.size());
		if (Policy != 3)
		{
			const UINT index = Next++ % size;
			const auto& slot = Ring[index];
			if (&slot == Latest) throw std::logic_error("Overwriting the latest SkyView request");
			Recorder.Begin(index);
			if (slot.Reader && !Submit->Complete(slot.Reader))
				throw std::logic_error("SkyView transfer slot still in use by GPU0");
			return index;
		}
		const uint64_t displayed = Selected ? Selected->Version : Displayed ? Displayed->Version : 0;
		for (UINT offset = 0; offset < size; ++offset)
		{
			const UINT index = (Next + offset) % size;
			const auto& slot = Ring[index];
			// Preserve newer publications, including any that completed after this frame's selection.
			if (&slot == Latest || &slot == Selected || (slot.Generation == Generation && slot.Version > displayed) ||
			    (slot.Ready && !Submit->Complete(slot.Ready)) || (slot.Reader && !Submit->Complete(slot.Reader)) ||
			    !Recorder.TryBegin(index)) continue;
			Next = (index + 1) % size;
			return index;
		}
		return std::nullopt;
	}
	Publication* PrepareOrder(const GDX12SkyConstants& key, const GDX12SkyConstants& medium,
	                          const GDX12SkyConstants& multiple, const GDX12CameraConstants* camera)
	{
		if (!camera && Latest && !Different(Latest->Key, key)) return nullptr;
		const auto index = BeginOrder();
		if (!index) return nullptr; // The caller retries the latest desired key on each frame, including when it stops changing.
		auto& slot = Ring[*index];
		auto& resources = Input->Resources;
		if (!slot.Constants)
			slot.Constants = std::make_unique<GDX12UploadBuffer<GDX12SkyConstants>>(resources.Device, 1);
		if (camera)
		{
			if (!slot.Camera) slot.Camera = std::make_unique<GDX12UploadBuffer<GDX12CameraConstants>>(resources.Device, 1);
			slot.Camera->CopyData(0, *camera);
		}
		if (!slot.Shared.Resources[0]) slot.Shared = Submit->CreateTransferBuffer(Device, Bytes);
		slot.Constants->CopyData(0, key);
		slot.Key = key;
		slot.Reader = {};
		const bool transmittance = Input->Version == 0 || Different(Input->Medium, medium);
		const bool scattering = transmittance || Different(Input->Multiple, multiple);
		slot.Inputs = Input->Version + scattering;

		Graph graph;
		graph.Device = Device;
		graph.FrameBoundary = false;
		graph.TerminalQueue = Queue::Compute;
		RemoteWork.Commands.clear();
		Bindings.clear();
		const auto native = [](auto& texture) { return texture.GetResource()->D3DResource.Get(); };
		const auto access = [&](ID3D12Resource* resource, const char* name, Mode mode, State state) {
			return Access{NativeWork::ResourceFor(graph, Bindings, resource, name), mode, state};
		};
		constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		const auto constants = access(slot.Constants->GetResource().D3DResource.Get(), "remote atmosphere constants",
		    Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ);
		const auto dispatch = [&](UINT stage, const char* name) {
			auto& output = stage < 2 ? Input->Luts[stage] : Outputs[0];
			std::vector<Access> accesses{constants, access(native(output), name, Mode::Write, UnorderedAccess)};
			if (stage >= 1) accesses.push_back(access(native(Input->Luts[0]), "remote transmittance", Mode::Read, read));
			if (stage == 2) accesses.push_back(access(native(Input->Luts[1]), "remote multiple scattering", Mode::Read, read));
			D3D12_GPU_VIRTUAL_ADDRESS cameraCB = 0;
			if (stage == 2 && camera)
			{
				accesses.push_back(access(slot.Camera->GetResource().D3DResource.Get(), "remote aerial camera",
				    Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ));
				accesses.push_back(access(native(Outputs[1]), "remote aerial transmittance", Mode::Write, UnorderedAccess));
				cameraCB = slot.Camera->GetElementAddress(0);
			}
			const auto cb = slot.Constants->GetElementAddress(0);
			RemoteWork.Add(graph, name, std::move(accesses), [this, stage, cb, cameraCB, &output](auto* cmd) {
				auto* heap = Input->Resources.SRV_UAV_Heap->GetHeap().Get();
				cmd->SetDescriptorHeaps(1, &heap);
				cmd->SetComputeRootSignature(Root->GetRootSignature().Get());
				cmd->SetPipelineState(Pipelines[stage].Get());
				cmd->SetComputeRootConstantBufferView(Root->GetBRootParamIndex(0), cb);
				if (stage >= 1) cmd->SetComputeRootDescriptorTable(Root->GetTRootParamIndex(0), Input->Luts[0].GetSRV()->GPUHandle);
				if (stage == 2) cmd->SetComputeRootDescriptorTable(Root->GetTRootParamIndex(1), Input->Luts[1].GetSRV()->GPUHandle);
				if (cameraCB)
				{
					cmd->SetComputeRootConstantBufferView(Root->GetBRootParamIndex(1), cameraCB);
					cmd->SetComputeRootDescriptorTable(Root->GetURootParamIndex(1), output.GetUAV()->GPUHandle);
					cmd->SetComputeRootDescriptorTable(Root->GetURootParamIndex(2), Outputs[1].GetUAV()->GPUHandle);
					cmd->Dispatch((output.GetWidth() + 3) / 4, (output.GetHeight() + 3) / 4,
					              (Footprints[0].Footprint.Depth + 3) / 4);
				}
				else
				{
					cmd->SetComputeRootDescriptorTable(Root->GetURootParamIndex(0), output.GetUAV()->GPUHandle);
					cmd->Dispatch((output.GetWidth() + 7) / 8, (output.GetHeight() + 7) / 8, 1);
				}
			}, {}, Queue::Compute);
		};
		// Both techniques submit to the same native compute queue on this device. Each graph returns the LUTs to COMMON.
		if (transmittance) dispatch(0, "secondary atmosphere transmittance");
		if (scattering) dispatch(1, "secondary atmosphere multiple scattering");
		dispatch(2, Aerial() ? "secondary camera aerial perspective" : "SkyView secondary camera LUT");
		std::array<GDX12Texture*, 2> outputs{&Outputs[0], &Outputs[1]};
		AppendTransfer(graph, Bindings, RemoteWork, outputs, slot.Shared.Resources[1].Get(), false);
		Plan = Compile(graph);
		Recorder.Reserve(Plan);
		return &slot;
	}

	void Publish(Publication& slot, const GDX12SkyConstants& medium, const GDX12SkyConstants& multiple, uint64_t frame)
	{
		Recorder.Execute(Plan, Bindings, [&](Id op, uint32_t, auto* cmd) { RemoteWork.Record(op, cmd); });
		slot.Ready = Recorder.Completions().back();
		slot.Version = ++Ordered;
		slot.SourceFrame = frame;
		slot.Generation = Generation;
		Input->Version = slot.Inputs;
		Input->Medium = medium;
		Input->Multiple = multiple;
		Latest = &slot;
	}

	Id AppendTransfer(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work,
	                  const std::array<GDX12Texture*, 2>& textures, ID3D12Resource* shared, bool import)
	{
		constexpr State src = D3D12_RESOURCE_STATE_COPY_SOURCE, dst = D3D12_RESOURCE_STATE_COPY_DEST;
		const Id buffer = NativeWork::ResourceFor(graph, bindings, shared, Aerial() ? "shared aerial pair" : "shared SkyView");
		std::vector<Access> accesses{{buffer, import ? Mode::Read : Mode::Write, import ? src : dst}};
		std::array<CD3DX12_TEXTURE_COPY_LOCATION, 2> sources, destinations;
		for (UINT i = 0; i < OutputCount; ++i)
		{
			auto* texture = textures[i]->GetResource()->D3DResource.Get();
			const Id id = NativeWork::ResourceFor(graph, bindings, texture,
			    Aerial() ? (i == 0 ? "aerial radiance" : "aerial transmittance") : "visible SkyView LUT");
			accesses.push_back({id, import ? Mode::Write : Mode::Read, import ? dst : src});
			const CD3DX12_TEXTURE_COPY_LOCATION packed(shared, Footprints[i]), subresource(texture, 0u);
			sources[i] = import ? packed : subresource;
			destinations[i] = import ? subresource : packed;
		}
		const char* name = Aerial() ? (import ? "GPU0 import aerial pair" : "aerial pair export")
		                           : (import ? "GPU0 import visible SkyView" : "SkyView export");
		return work.Add(graph, name, std::move(accesses), [this, sources, destinations, import](auto* cmd) {
			for (UINT i = 0; i < OutputCount; ++i)
				cmd->CopyTextureRegion(&destinations[i], 0, 0, 0, &sources[i], nullptr);
			if (import && Aerial()) RecordedCopies = OutputCount;
		}, {}, import ? Queue::Copy : Queue::Compute);
	}

	void BeginDisplay()
	{
		PrimaryWork.Commands.clear();
		DisplayResources.fill(Invalid);
		ImportOperation = ImportPacket = Consumer = Invalid;
		RecordedCopies = 0;
	}

	void AppendImport(Graph& graph, std::vector<ID3D12Resource*>& bindings,
	                  const std::array<GDX12Texture*, 2>& textures)
	{
		if (ImportOperation != Invalid || !Selected || Selected->Version == Imported) return;
		ImportOperation = AppendTransfer(graph, bindings, PrimaryWork, textures, Selected->Shared.Resources[0].Get(), true);
	}
};
SceneEnvironment::SceneEnvironment(GDX12DeviceResources& r, RenderPipelineCommonData& c)
    : _resources(r), _common(c), _skyViewSampling(r.FrameConstants.size())
{
	_environment.Setup(&r, &c);
	_environment.Initialize();
	_cloud.Setup(&r, &c);
	// Resource creation remains an owner-only prerequisite.
}
SceneEnvironment::~SceneEnvironment()
{
	_environment.ClearDenendencies();
	_cloud.ClearDenendencies();
}
void SceneEnvironment::SetClouds(const GDX12CloudParameters& p)
{
	_cloud.SetParameters(p);
	_cloudSelected = p.Enabled;
}
void SceneEnvironment::ResetClouds()
{
	_cloud.Reset();
}

void SceneEnvironment::Submitted(uint64_t fence)
{
	if (!fence)
	{
		throw std::invalid_argument("Missing environment completion");
	}
	if (!_work.empty())
	{
		_retained.emplace_back(fence, _work);
	}
}
void SceneEnvironment::Freeze(double delta, Executor& primary, uint64_t frame)
{
	const auto completed = _resources.Device->GetCommandQueue()->GetFence()->GetCompletedValue();
	_retained.erase(std::remove_if(_retained.begin(), _retained.end(),
	                               [completed](const auto& entry) { return entry.first <= completed; }),
	                _retained.end());
	_cloud.Advance(_cloudSelected ? delta : 0);
	_cloudInput = _cloud.CaptureInputs();
	_work.clear();
	Build(primary, frame);
	// The graph can register readers in append before registering environment work in prelude.
	if (_remoteSkyView) _remoteSkyView->BeginDisplay();
	if (_remoteAerial) _remoteAerial->BeginDisplay();
}

bool SceneEnvironment::PrepareAtmosphere(Executor& primary, uint64_t frame, bool atmosphere, uint64_t skyRevision,
                                         bool& localChanged, const GDX12CameraConstants* camera, bool invalidated)
{
	const int mode = camera ? AerialCrossSync.GetValue() : SkyViewCrossSync.GetValue();
	const int requested = atmosphere ? (camera ? AerialAdapter.GetValue() : SkyViewAdapter.GetValue()) : -1;
	auto& active = camera ? _remoteAerial : _remoteSkyView;
	auto& peer = camera ? _remoteSkyView : _remoteAerial;
	auto& failed = camera ? _failedAerialAdapter : _failedSkyViewAdapter;
	if (failed != requested) failed = -1;
	const auto& submit = primary.Submissions();
	const Id device = requested >= 0 && requested != failed ? submit->SelectAdapter(requested) : 0;
	const auto& luts = _environment._luts;
	const RemoteAtmosphere::Textures sources{luts[0].get(), luts[1].get(),
	    luts[camera ? 3 : 2].get(), camera ? luts[4].get() : nullptr};
	const auto retire = [&] {
		if (!active) return;
		primary.Drain();
		active->Recorder.Drain();
		active.reset();
		localChanged = true;
	};
	if (active && (active->Device != device || !active->Matches(sources))) retire();
	if (!device) return false;
	const auto& key = camera ? _aerialConstants : _viewConstants;
	RemoteAtmosphere::Publication* order = nullptr;
	const auto fallback = [&](const auto& reason) {
		submit->CheckHealth();
		retire();
		failed = requested;
		Log::Warning("RenderGraph", camera ? "Using local Aerial Perspective: " : "Using local SkyView: ", reason);
	};
	try
	{
		if (!active)
			active = std::make_unique<RemoteAtmosphere>(primary, device, UINT(_resources.FrameConstants.size()), sources, peer.get());
		active->Configure(mode, _common.HistoryRevision, skyRevision, _mediumConstants, _multipleConstants, invalidated);
		if (mode == 3) active->SelectCompleted();
		if (!camera || localChanged || !active->Latest)
			order = active->PrepareOrder(key, _mediumConstants, _multipleConstants, camera);
	}
	catch (const DxException& e)
	{
		if (DeviceLost(e.ErrorCode)) throw;
		fallback(e.ToString());
		return false;
	}
	catch (const std::exception& e)
	{
		fallback(e.what());
		return false;
	}
	auto& remote = *active;
	// Capture the request of the preceding frame, even when that frame did not dispatch.
	auto* previous = remote.Latest;
	if (order) remote.Publish(*order, _mediumConstants, _multipleConstants, frame);
	if (mode != 3) remote.Selected = mode == 0 && previous ? previous : remote.Latest;
	if (remote.Selected) remote.Displayed = static_cast<const RemoteAtmosphere::View&>(*remote.Selected);
	if (!remote.Displayed)
	{
		remote.Displayed = RemoteAtmosphere::View{key, 0, frame};
		localChanged = true;
		return false;
	}
	return true;
}

void SceneEnvironment::Build(Executor& primary, uint64_t frameNumber)
{
	auto& e = _environment;
	auto& r = _resources;
	auto state = r.Sky;
	const bool clouds = _cloudSelected && _cloudInput.Enabled;
	const bool cloudChanged =
	    clouds != e._cloudActive || (clouds && e._cloudSourceRevision != _cloudInput.SourceRevision);
	const bool lightingChanged = clouds && e._cloudLightingRevision != _cloudInput.LightingRevision;
	e._cloudActive = clouds;
	e._cloudSourceRevision = _cloudInput.SourceRevision;
	e._cloudLightingRevision = _cloudInput.LightingRevision;
	if (clouds && !_cloud._texture->IsInitialized())
	{
		_cloud.CreateResources();
	}
	if (clouds)
	{
		state.Source = SkySource::SkyBox;
		state.Quality = _cloudInput.Quality;
		state.Box.RadianceMultiplier = 1;
		state.Box.RotationRadians = 0;
	}
	bool changed = e._invalidated || cloudChanged || e._revision != state.Revision || e._quality != state.Quality ||
	               e._source != state.Source;
	const bool discontinuity = changed;
	GDX12SkyConstants constants;
	bool transmittanceChanged = false, multipleChanged = false, viewChanged = false, aerialChanged = false;
	constants.Mode.x = UINT(state.Source);
	SkyIBLSettings ibl;
	SkyAtmosphereSettings atmosphere;
	if (state.Source == SkySource::SkyBox)
	{
		ValidateSkySettings(state.Box);
		ibl = clouds ? ResolveSkyIBLSettings(state.Box.IBL, state.Quality)
		             : ResolveSkyBoxIBLSettings(state.Box.IBL, state.Quality);
		changed |= e._resolvedIBL.CubeResolution != ibl.CubeResolution ||
		           e._resolvedIBL.FilterSamples != ibl.FilterSamples || e._resolvedIBL.Intensity != ibl.Intensity;
		e._resolvedIBL = ibl;
		constants.Box = {state.Box.RadianceMultiplier, std::sin(state.Box.RotationRadians),
		                 std::cos(state.Box.RotationRadians), 0};
		if (!clouds && (e._boxAsset != state.Box.Cubemap || !e._boxTexture->IsInitialized()))
		{
			e.RetireResources();
			const auto path = state.Box.Cubemap.is_absolute()
			                      ? state.Box.Cubemap
			                      : std::filesystem::path(ASSETS_FOLDER) / state.Box.Cubemap;
			const auto* source = Engine::Core::AssetManager::GetInstance().LoadTexture(path);
			if (!source || !source->IsCubeMap() || source->GetArraySize() != 6 || !source->GetWidth() ||
			    source->GetWidth() != source->GetHeight() || !source->GetMipLevels() ||
			    source->GetSubresourceCount() != 6 * source->GetMipLevels())
			{
				throw std::runtime_error("Environment requires a complete DDS cubemap");
			}
			GDX12TextureDesc desc;
			desc.Format = desc.SRVDesc.Format = FormatToSRGB(source->GetFormat());
			desc.Semantic = ETextureSemantic::Color;
			desc.Width = source->GetWidth();
			desc.Height = source->GetHeight();
			desc.SRV_UAV_Heap = r.SRV_UAV_Heap.get();
			desc.SRVHeapIndex =
			    e._boxTexture->IsInitialized()
			        ? e._boxTexture->GetSRV()->HeapIndex
			        : desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
			desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
			desc.SRVDesc.TextureCube.MipLevels = source->GetMipLevels();
			const auto shape =
			    CD3DX12_RESOURCE_DESC::Tex2D(desc.Format, desc.Width, desc.Height, 6, UINT16(source->GetMipLevels()));
			desc.ExternalResource =
			    r.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, shape, D3D12_RESOURCE_STATE_COMMON);
			e._boxTexture->Initialize(desc);
			const UINT count = UINT(source->GetSubresourceCount());
			std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
			std::vector<UINT> rows(count);
			std::vector<UINT64> rowBytes(count);
			UINT64 bytes = 0;
			r.Device->GetDevice()->GetCopyableFootprints(&shape, 0, count, 0, footprints.data(), rows.data(),
			                                             rowBytes.data(), &bytes);
			auto upload = std::make_shared<GDX12Resource>();
			upload->D3DResource = r.Device->CreateResource(D3D12_HEAP_TYPE_UPLOAD, CD3DX12_RESOURCE_DESC::Buffer(bytes),
			                                               D3D12_RESOURCE_STATE_GENERIC_READ);
			void* mapped = nullptr;
			const D3D12_RANGE noRead{};
			ThrowIfFailed(upload->D3DResource->Map(0, &noRead, &mapped));
			for (UINT i = 0; i < count; ++i)
			{
				const auto& data = source->GetSubresources()[i];
				if (data.RowPitch < rowBytes[i] || data.Data.size() < data.RowPitch * (rows[i] - 1) + rowBytes[i])
				{
					throw std::runtime_error("Incomplete environment subresource");
				}
				for (UINT row = 0; row < rows[i]; ++row)
				{
					std::memcpy(static_cast<std::byte*>(mapped) + footprints[i].Offset +
					                UINT64(row) * footprints[i].Footprint.RowPitch,
					            data.Data.data() + UINT64(row) * data.RowPitch, SIZE_T(rowBytes[i]));
				}
			}
			upload->D3DResource->Unmap(0, nullptr);
			auto* target = e._boxTexture->GetResource();
			_work.push_back({"environment DDS upload",
			                 {{upload.get(), {Invalid, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ}},
			                  {target, {Invalid, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}}},
			                 [upload, target, footprints](auto* cmd)
			                 {
				                 for (UINT i = 0; i < footprints.size(); ++i)
				                 {
					                 const CD3DX12_TEXTURE_COPY_LOCATION dst(target->D3DResource.Get(), i),
					                     src(upload->D3DResource.Get(), footprints[i]);
					                 cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
				                 }
			                 }});
			e._boxAsset = state.Box.Cubemap;
			changed = true;
		}
	}
	else if (state.Source == SkySource::SkyAtmosphere)
	{
		atmosphere = ResolveSkyAtmosphereSettings(state.Atmosphere, state.Quality);
		ibl = atmosphere.IBL;
		const UINT dimensions[5][3] = {{atmosphere.TransmittanceWidth, atmosphere.TransmittanceHeight, 1},
		                               {atmosphere.MultipleScatteringSize, atmosphere.MultipleScatteringSize, 1},
		                               {atmosphere.SkyViewWidth, atmosphere.SkyViewHeight, 1},
		                               {atmosphere.AerialWidth, atmosphere.AerialHeight, atmosphere.AerialDepth},
		                               {atmosphere.AerialWidth, atmosphere.AerialHeight, atmosphere.AerialDepth}};
		std::array<bool, 5> resized{};
		for (UINT i = 0; i < resized.size(); ++i)
		{
			resized[i] = !e._luts[i]->IsInitialized() || e._luts[i]->GetWidth() != dimensions[i][0] ||
			             e._luts[i]->GetHeight() != dimensions[i][1] ||
			             e._luts[i]->GetResource()->D3DResource->GetDesc().DepthOrArraySize != dimensions[i][2];
		}
		e.EnsureAtmosphere(atmosphere);
		const bool captureResized = !_captureSkyView || resized[2];
		if (captureResized)
		{
			if (!_captureSkyView)
				_captureSkyView = std::make_unique<GDX12Texture>();
			e.CreateTexture(*_captureSkyView, dimensions[2][0], dimensions[2][1], 1, 1, false);
		}
		const auto pack = [](const Vector3& v, float w) { return XMFLOAT4(v.x, v.y, v.z, w); };
		constants.RayleighHeight = pack(atmosphere.RayleighScatteringPerKm, atmosphere.RayleighScaleHeightKm);
		constants.MieScatteringHeight = pack(atmosphere.MieScatteringPerKm, atmosphere.MieScaleHeightKm);
		constants.MieAbsorptionG = pack(atmosphere.MieAbsorptionPerKm, atmosphere.MieAnisotropy);
		constants.OzoneCenter = pack(atmosphere.OzoneAbsorptionPerKm, atmosphere.OzoneCenterKm);
		constants.GroundWidth = pack(atmosphere.GroundAlbedo, atmosphere.OzoneHalfWidthKm);
		constants.CenterRadius = pack(atmosphere.PlanetCenterCm, atmosphere.PlanetRadiusKm);
		constants.Atmosphere = {atmosphere.PlanetRadiusKm + atmosphere.AtmosphereHeightKm, atmosphere.AerialDistanceKm,
		                        atmosphere.SunAngularRadiusRadians, ibl.Intensity};
		constants.SunDirection = pack(atmosphere.DirectionToSun, 0);
		constants.SunIlluminance = pack(atmosphere.SunIlluminanceLux * atmosphere.LuminanceScale, 0);
		Vector3 relative = (_common.CameraView.Invert().Translation() - atmosphere.PlanetCenterCm) * .00001f;
		if (relative.LengthSquared() < 1e-12f)
		{
			throw std::invalid_argument("Atmosphere camera at planet center");
		}
		if (relative.Length() < atmosphere.PlanetRadiusKm + .001f)
		{
			relative.Normalize();
			relative *= atmosphere.PlanetRadiusKm + .001f;
		}
		constants.CameraPosition = pack(relative, 0);
		constants.Samples = {atmosphere.TransmittanceSamples, atmosphere.SkySamples,
		                     atmosphere.MultipleScatteringDirections, atmosphere.MultipleScatteringSamples};
		constants.Mode.y = atmosphere.AerialSamples;
		constants.Box.z = float(_common.DownscaledWidth ? _common.DownscaledWidth : _common.WindowWidth);
		constants.Box.w = float(_common.DownscaledHeight ? _common.DownscaledHeight : _common.WindowHeight);
		auto capture = constants;
		capture.CameraPosition = pack((atmosphere.IBLCapturePositionCm - atmosphere.PlanetCenterCm) * .00001f, 0);
		r.FrameConstants[r.CurrFrameConstantsIndex]->SkyCB->CopyData(1, capture);

		const auto differs = [](const auto& a, const auto& b) { return std::memcmp(&a, &b, sizeof(a)) != 0; };
		GDX12SkyConstants medium;
		medium.RayleighHeight = constants.RayleighHeight;
		medium.MieScatteringHeight = constants.MieScatteringHeight;
		medium.MieAbsorptionG = constants.MieAbsorptionG;
		medium.MieAbsorptionG.w = 0; // extinction does not depend on phase anisotropy
		medium.OzoneCenter = constants.OzoneCenter;
		medium.GroundWidth.w = constants.GroundWidth.w;
		medium.CenterRadius.w = constants.CenterRadius.w;
		medium.Atmosphere.x = constants.Atmosphere.x;
		medium.Samples.x = constants.Samples.x;
		transmittanceChanged = !_atmosphereReady || resized[0] || differs(medium, _mediumConstants);
		auto multiple = medium;
		multiple.GroundWidth = constants.GroundWidth;
		multiple.Samples.z = constants.Samples.z;
		multiple.Samples.w = constants.Samples.w;
		multipleChanged =
		    !_atmosphereReady || resized[1] || transmittanceChanged || differs(multiple, _multipleConstants);
		auto view = constants;
		view.Box = {};
		view.Mode.y = 0;
		view.Atmosphere.y = view.Atmosphere.z = view.Atmosphere.w = 0;
		auto captureKey = view;
		captureKey.CameraPosition = capture.CameraPosition;
		captureKey.Box.x = float(atmosphere.SkyViewWidth);
		captureKey.Box.y = float(atmosphere.SkyViewHeight);
		captureKey.Atmosphere.w = ibl.Intensity;
		captureKey.Mode.z = ibl.FilterSamples;
		captureKey.Mode.w = ibl.CubeResolution;
		changed = !_atmosphereReady || e._invalidated || e._source != state.Source || multipleChanged || captureResized ||
		          differs(captureKey, _captureConstants);
		viewChanged = !_atmosphereReady || multipleChanged || resized[2] || differs(view, _viewConstants);
		auto aerial = constants;
		aerial.Atmosphere.z = aerial.Atmosphere.w = 0;
		aerial.Samples.y = 0;
		const auto viewProjection = _common.CameraView * _common.CameraProjection;
		aerialChanged = !_atmosphereReady || multipleChanged || resized[3] || resized[4] ||
		                differs(aerial, _aerialConstants) || differs(viewProjection, _aerialViewProjection);
		_mediumConstants = medium;
		_multipleConstants = multiple;
		_captureConstants = captureKey;
		_viewConstants = view;
		_aerialConstants = aerial;
		_aerialViewProjection = viewProjection;
		_atmosphereReady = true;
		e._resolvedIBL = ibl;
	}
	changed |= lightingChanged;
	const bool atmosphereActive = state.Source == SkySource::SkyAtmosphere;
	const bool remoteSkyView = PrepareAtmosphere(primary, frameNumber, atmosphereActive,
	    state.Revision, viewChanged);
	const auto& visibleKey = _remoteSkyView && _remoteSkyView->Displayed ? _remoteSkyView->Displayed->Key : constants;
	auto& sampling = _skyViewSampling.at(r.CurrFrameConstantsIndex);
	if (!sampling) sampling = std::make_unique<GDX12UploadBuffer<XMFLOAT4>>(r.Device, 1);
	sampling->CopyData(0, XMFLOAT4(visibleKey.CameraPosition.x, visibleKey.CameraPosition.y,
	    visibleKey.CameraPosition.z, visibleKey.CenterRadius.w));
	auto& frame = *r.FrameConstants[r.CurrFrameConstantsIndex];
	// This is the same jittered camera snapshot bound to b1 by the local AerialCS.
	GDX12CameraConstants aerialCamera;
	if (atmosphereActive && AerialAdapter.GetValue() >= 0)
		aerialCamera = frame.CameraCB->GetCPUData()[_common.ActiveCameraCBufferIndex];
	const bool remoteAerial = PrepareAtmosphere(primary, frameNumber, atmosphereActive, state.Revision,
	    aerialChanged, &aerialCamera, discontinuity);
	frame.SkyCB->CopyData(0, constants);
	if (changed)
	{
		e.EnsureOutputs(state.Source != SkySource::Off, state.Source == SkySource::Off ? 1 : ibl.CubeResolution);
	}
	if (changed && state.Source != SkySource::SkyAtmosphere)
	{
		e.ReleaseAtmosphere();
		_captureSkyView.reset();
		e.PublishAtmosphereViews(false);
	}
	if (state.Source != SkySource::SkyBox && e._boxTexture->IsInitialized())
	{
		e.RetireResources();
		e._boxTexture = std::make_unique<GDX12Texture>();
		e._boxAsset.clear();
	}
	auto* heap = r.SRV_UAV_Heap->GetHeap().Get();
	const auto add = [&](const std::string& name, std::vector<std::pair<GDX12Resource*, Access>> accesses,
	                     std::function<void(ID3D12GraphicsCommandList*)> record, bool compute = true,
	                     WorkDomain domain = WorkDomain::Cached)
	{
		_work.push_back({name, std::move(accesses), [heap, record](auto* cmd)
		                 {
			                 cmd->SetDescriptorHeaps(1, &heap);
			                 record(cmd);
		                 }, compute, domain});
	};
	const auto access = [](GDX12Texture* t, Mode mode, State required)
	{ return std::make_pair(static_cast<GDX12Resource*>(t->GetResource()), Access{Invalid, mode, required}); };
	if (clouds && changed)
	{
		auto* texture = _cloud._texture.get();
		const auto input = _cloudInput;
		auto* root = _cloud._root.get();
		auto* pso = _cloud._pipeline.Get();
		const auto uav = texture->GetUAV()->GPUHandle;
		add("local cloud source", {access(texture, Mode::Write, UnorderedAccess)},
		    [input, root, pso, uav](auto* cmd)
		    {
			    struct Constants
			    {
				    UINT Size, Octaves, Implementation, Enabled;
				    float Scale, Coverage, Softness, Padding;
				    Vector3 Horizon;
				    float PhaseX;
				    Vector3 Zenith;
				    float PhaseY;
				    Vector3 Cloud;
				    UINT Seed;
			    };
			    const auto& p = input.Parameters;
			    const Constants c{256,
			                      input.Octaves,
			                      UINT(p.Implementation),
			                      input.Enabled ? 1u : 0u,
			                      p.Scale,
			                      p.Coverage,
			                      p.Softness,
			                      0,
			                      p.HorizonColor,
			                      input.Phase.x,
			                      p.ZenithColor,
			                      input.Phase.y,
			                      p.CloudColor,
			                      p.Seed};
			    static_assert(sizeof(c) == 80);
			    cmd->SetComputeRootSignature(root->GetRootSignature().Get());
			    cmd->SetPipelineState(pso);
			    cmd->SetComputeRoot32BitConstants(0, 20, &c, 0);
			    cmd->SetComputeRootDescriptorTable(root->GetURootParamIndex(0), uav);
			    cmd->Dispatch(32, 32, 6);
		    });
	}
	const auto sky = [&](UINT pipeline, UINT constantsIndex, std::vector<std::pair<GDX12Resource*, Access>> accesses,
	                     UINT x, UINT y, UINT z, std::array<GDX12Texture*, 4> srvs,
	                     std::array<GDX12Descriptor*, 4> uavs, WorkDomain domain = WorkDomain::Cached)
	{
		auto* root = e._skyRoot.get();
		auto* pso = e._skyPipelines[pipeline].Get();
		const auto cb = frame.SkyCB->GetElementAddress(constantsIndex);
		// Aerial perspective is the only kernel that reads the camera buffer.
		const auto camera = pipeline == 3 ? frame.CameraCB->GetElementAddress(_common.ActiveCameraCBufferIndex) : 0;
		std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4> reads{}, writes{};
		for (UINT i = 0; i < 4; ++i)
		{
			reads[i] = srvs[i] ? srvs[i]->GetSRV()->GPUHandle : (i == 3 ? e._nullCube.GPUHandle : e._null2D.GPUHandle);
			if (uavs[i])
			{
				writes[i] = uavs[i]->GPUHandle;
			}
		}
		accesses.push_back({&frame.SkyCB->GetResource(), {Invalid, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ}});
		if (camera)
			accesses.push_back({&frame.CameraCB->GetResource(), {Invalid, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ}});
		add("atmosphere/source kernel " + std::to_string(pipeline), std::move(accesses),
		    [root, pso, cb, camera, reads, writes, x, y, z](auto* cmd)
		    {
			    cmd->SetComputeRootSignature(root->GetRootSignature().Get());
			    cmd->SetPipelineState(pso);
			    cmd->SetComputeRootConstantBufferView(root->GetBRootParamIndex(0), cb);
			    if (camera)
				    cmd->SetComputeRootConstantBufferView(root->GetBRootParamIndex(1), camera);
			    for (UINT i = 0; i < 4; ++i)
			    {
				    cmd->SetComputeRootDescriptorTable(root->GetTRootParamIndex(i), reads[i]);
				    if (writes[i].ptr)
				    {
					    cmd->SetComputeRootDescriptorTable(root->GetURootParamIndex(i), writes[i]);
				    }
			    }
			    cmd->Dispatch(x, y, z);
		    }, true, domain);
	};
	if (state.Source == SkySource::SkyAtmosphere)
	{
		auto& l = e._luts;
		if (transmittanceChanged)
		{
			sky(0, 1, {access(l[0].get(), Mode::Write, UnorderedAccess)}, (l[0]->GetWidth() + 7) / 8,
			    (l[0]->GetHeight() + 7) / 8, 1, {}, {l[0]->GetUAV()});
		}
		if (multipleChanged)
		{
			sky(1, 1, {access(l[0].get(), Mode::Read, Read), access(l[1].get(), Mode::Write, UnorderedAccess)},
			    (l[1]->GetWidth() + 7) / 8, (l[1]->GetHeight() + 7) / 8, 1, {l[0].get()}, {l[1]->GetUAV()});
		}
		if (changed)
		{
			sky(2, 1,
			    {access(l[0].get(), Mode::Read, Read), access(l[1].get(), Mode::Read, Read),
			     access(_captureSkyView.get(), Mode::Write, UnorderedAccess)},
			    (_captureSkyView->GetWidth() + 7) / 8, (_captureSkyView->GetHeight() + 7) / 8, 1,
			    {l[0].get(), l[1].get()}, {_captureSkyView->GetUAV()});
		}
	}
	if (changed && state.Source != SkySource::Off)
	{
		for (UINT mip = 0; mip < e._sourceUAVs.size(); ++mip)
		{
			std::vector<std::pair<GDX12Resource*, Access>> accesses{
			    access(e._textures[3].get(), Mode::Write, UnorderedAccess)};
			std::array<GDX12Texture*, 4> inputs{};
			if (state.Source == SkySource::SkyAtmosphere)
			{
				inputs = {e._luts[0].get(), e._luts[1].get(), _captureSkyView.get(), nullptr};
				for (UINT i = 0; i < 3; ++i)
				{
					accesses.push_back(access(inputs[i], Mode::Read, Read));
				}
			}
			else
			{
				inputs[3] = clouds ? _cloud._texture.get() : e._boxTexture.get();
				accesses.push_back(access(inputs[3], Mode::Read, Read));
			}
			const UINT size = (std::max)(1u, e._textures[3]->GetWidth() >> mip);
			sky(4, state.Source == SkySource::SkyAtmosphere ? 1 : 0, std::move(accesses), (size + 7) / 8,
			    (size + 7) / 8, 6, inputs, {nullptr, nullptr, nullptr, e._sourceUAVs[mip].get()});
		}
		const auto filter = [&](UINT pipeline, UINT size, float roughness, GDX12Descriptor* uav, GDX12Texture* target)
		{
			auto* root = e._filterRoot.get();
			auto* pso = e._filters[pipeline].Get();
			const auto srv = e._textures[3]->GetSRV()->GPUHandle, output = uav->GPUHandle;
			const UINT samples = ibl.FilterSamples;
			const float intensity = ibl.Intensity;
			add("environment filter " + std::to_string(pipeline) + " roughness " + std::to_string(roughness),
			    {access(e._textures[3].get(), Mode::Read, Read), access(target, Mode::Write, UnorderedAccess)},
			    [root, pso, srv, output, size, roughness, samples, intensity](auto* cmd)
			    {
				    const struct
				    {
					    UINT Size;
					    float Roughness;
					    UINT Samples;
					    float Intensity;
				    } c{size, roughness, samples, intensity};
				    cmd->SetComputeRootSignature(root->GetRootSignature().Get());
				    cmd->SetPipelineState(pso);
				    cmd->SetComputeRoot32BitConstants(0, 4, &c, 0);
				    cmd->SetComputeRootDescriptorTable(root->GetTRootParamIndex(0), srv);
				    cmd->SetComputeRootDescriptorTable(root->GetURootParamIndex(0), output);
				    cmd->Dispatch((size + 7) / 8, (size + 7) / 8, 6);
			    });
		};
		filter(0, e._textures[0]->GetWidth(), 0, e._textures[0]->GetUAV(), e._textures[0].get());
		for (UINT mip = 0; mip < e._prefilterMips; ++mip)
		{
			filter(1, e._textures[1]->GetWidth() >> mip, e._prefilterMips > 1 ? mip / 7.f : 0,
			       e._prefilterUAVs[mip].get(), e._textures[1].get());
		}
	}
	else if (changed)
	{
		if (!e._disabledPixel)
		{
			e._disabledPixel = std::make_unique<GDX12UploadBuffer<std::array<uint16_t, 4>>>(r.Device, 1);
			e._disabledPixel->CopyData(0, {0, 0, 0, 0x3c00});
		}
		for (UINT i : {0u, 1u, 3u})
		{
			auto* target = e._textures[i]->GetResource();
			auto* source = e._disabledPixel->GetResource().D3DResource.Get();
			add("disabled environment black " + std::to_string(i),
			    {access(e._textures[i].get(), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST),
			     {&e._disabledPixel->GetResource(), {Invalid, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ}}},
			    [target, source](auto* cmd)
			    {
				    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{
				        0, {DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT}};
				    const CD3DX12_TEXTURE_COPY_LOCATION src(source, fp);
				    for (UINT face = 0; face < 6; ++face)
				    {
					    const CD3DX12_TEXTURE_COPY_LOCATION dst(target->D3DResource.Get(), face);
					    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
				    }
			    }, false);
		}
	}
	if (!e._brdfReady)
	{
		auto* root = e._filterRoot.get();
		auto* pso = e._filters[2].Get();
		const auto uav = e._textures[2]->GetUAV()->GPUHandle;
		const UINT size = e._textures[2]->GetWidth();
		add("environment BRDF integration", {access(e._textures[2].get(), Mode::Write, UnorderedAccess)},
		    [root, pso, uav, size](auto* cmd)
		    {
			    const UINT c[4] = {size, 0, 0, 0};
			    cmd->SetComputeRootSignature(root->GetRootSignature().Get());
			    cmd->SetPipelineState(pso);
			    cmd->SetComputeRoot32BitConstants(0, 4, c, 0);
			    cmd->SetComputeRootDescriptorTable(root->GetURootParamIndex(1), uav);
			    cmd->Dispatch((size + 7) / 8, (size + 7) / 8, 1);
		    });
		e._brdfReady = true;
	}
	if (state.Source == SkySource::SkyAtmosphere)
	{
		auto& l = e._luts;
		if (viewChanged && !remoteSkyView)
		{
			sky(2, 0,
			    {access(l[0].get(), Mode::Read, Read), access(l[1].get(), Mode::Read, Read),
			     access(l[2].get(), Mode::Write, UnorderedAccess)},
			    (l[2]->GetWidth() + 7) / 8, (l[2]->GetHeight() + 7) / 8, 1, {l[0].get(), l[1].get()}, {l[2]->GetUAV()},
			    WorkDomain::Camera);
		}
		if (aerialChanged && !remoteAerial)
		{
			sky(3, 0,
			    {access(l[0].get(), Mode::Read, Read), access(l[1].get(), Mode::Read, Read),
			     access(l[3].get(), Mode::Write, UnorderedAccess), access(l[4].get(), Mode::Write, UnorderedAccess)},
			    (atmosphere.AerialWidth + 3) / 4, (atmosphere.AerialHeight + 3) / 4, (atmosphere.AerialDepth + 3) / 4,
			    {l[0].get(), l[1].get()}, {nullptr, l[3]->GetUAV(), l[4]->GetUAV()}, WorkDomain::Camera);
		}
	}
	if (discontinuity)
	{
		e.ResetEnvironmentHistory();
	}
	e._revision = state.Revision;
	e._quality = state.Quality;
	e._source = state.Source;
	e._invalidated = false;
}
bool SceneEnvironment::AsyncComputeEnabled() const
{
	return AsyncEnvironment.GetValue();
}
void SceneEnvironment::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings)
{
	if (graph.Resources.size() != bindings.size())
	{
		throw std::logic_error("Environment binding mismatch");
	}
	_operations.clear();
	if (_remoteSkyView)
		_remoteSkyView->AppendImport(graph, bindings, {_environment._luts[2].get(), nullptr});
	const bool async = AsyncComputeEnabled();
	for (const auto& work : _work)
	{
		Operation operation{(work.Domain == WorkDomain::Cached ? "cached environment / " : "camera environment / ") +
		                    work.Name};
		if (async && work.Compute)
		{
			operation.Target = Queue::Compute;
		}
		for (auto [resource, access] : work.Accesses)
		{
			if (operation.Target == Queue::Compute && access.Required == Read)
			{
				access.Required = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			}
			auto* native = resource->D3DResource.Get();
			access.Resource = graph.FindBinding(bindings, native);
			if (access.Resource == Invalid)
				access.Resource = NativeWork::ResourceFor(graph, bindings, native,
					work.Name + " resource " + std::to_string(bindings.size()));
			operation.Accesses.push_back(access);
		}
		_operations.push_back(Id(graph.Operations.size()));
		graph.Operations.push_back(std::move(operation));
	}
}
bool SceneEnvironment::Record(Id operation, ID3D12GraphicsCommandList* cmd) const
{
	if (_remoteSkyView && _remoteSkyView->PrimaryWork.Record(operation, cmd)) return true;
	if (_remoteAerial && _remoteAerial->PrimaryWork.Record(operation, cmd)) return true;
	const auto found = std::lower_bound(_operations.begin(), _operations.end(), operation);
	if (found == _operations.end() || *found != operation)
	{
		return false;
	}
	_work[found - _operations.begin()].Record(cmd);
	return true;
}

void SceneEnvironment::AddAtmosphereWaits(const Schedule& schedule, std::vector<PacketWait>& waits)
{
	for (auto* remote : {_remoteSkyView.get(), _remoteAerial.get()})
	{
		if (!remote) continue;
		for (Id i = 0; i < schedule.Packets.size(); ++i)
		{
			const auto& packet = schedule.Packets[i];
			if (remote->ImportOperation != Invalid && packet.Operation == remote->ImportOperation) remote->ImportPacket = i;
			if (remote->Consumer != Invalid) continue;
			const auto reads = [&](Id resource) {
				return resource != Invalid && std::any_of(packet.Accesses.begin(), packet.Accesses.end(),
				    [resource](const Access& access) { return access.Usage == Mode::Read && access.Resource == resource; });
			};
			if (remote->Aerial() ? reads(remote->DisplayResources[0]) && reads(remote->DisplayResources[1])
			                     : packet.Name == "clustered deferred lighting") remote->Consumer = i;
		}
		if (remote->Aerial() && remote->Consumer != Invalid) remote->ConsumerName = schedule.Packets[remote->Consumer].Name;
		if (remote->ImportOperation == Invalid) continue;
		if (remote->ImportPacket >= remote->Consumer || remote->Consumer >= schedule.Packets.size())
			throw std::logic_error("Atmosphere import must precede its first reader");
		if (remote->Policy == 3 && !remote->Submit->Complete(remote->Selected->Ready))
			throw std::logic_error("Latest-ready SkyView selected an unfinished publication");
		waits.push_back({remote->ImportPacket, remote->Selected->Ready});
	}
}

void SceneEnvironment::AtmosphereSubmitted(uint64_t frame, const std::vector<CompletionToken>& completions)
{
	for (auto* active : {_remoteSkyView.get(), _remoteAerial.get()})
	{
		if (!active) continue;
		auto& remote = *active;
		const auto imported = remote.ImportPacket < completions.size() ? completions[remote.ImportPacket] : CompletionToken{};
		if (imported)
		{
			remote.Selected->Reader = imported;
			remote.Imported = remote.Selected->Version;
		}
		if (remote.Consumer >= completions.size() || !completions[remote.Consumer] || remote.Reports >= 6) continue;
		++remote.Reports;
		const auto& displayed = *remote.Displayed;
		if (remote.Aerial())
		{
			const auto& shape = remote.Footprints[0].Footprint;
			Log::Info("RenderGraph", "Aerial Perspective CrossSync=", remote.Policy, " frame=", frame,
			    " GPU=", remote.Submit->AdapterLabel(remote.Device), " displayGPU=", remote.Submit->AdapterLabel(0),
			    " ordered=", remote.Latest->Version, " used=", displayed.Version, " usedFrame=", displayed.SourceFrame,
			    " size=", shape.Width, "x", shape.Height, "x", shape.Depth, " payloadBytes=", remote.Payload,
			    " inputs=", displayed.Inputs, " sharedInputs=", _remoteSkyView && _remoteSkyView->Input == remote.Input,
			    " exportFence=", displayed.Ready.Value, " importFence=", imported.Value,
			    " waitExportFence=", imported ? displayed.Ready.Value : 0, " recordedCopies=", remote.RecordedCopies,
			    " firstConsumer=", remote.ConsumerName, " consumerFence=", completions[remote.Consumer].Value);
			continue;
		}
		Log::Info("RenderGraph", "Visible SkyView CrossSync=", remote.Policy, " frame=", frame,
		    " GPU=", remote.Submit->AdapterLabel(remote.Device), " displayGPU=", remote.Submit->AdapterLabel(0),
		    " ordered=", remote.Latest ? remote.Latest->Version : 0,
		    " orderedFrame=", remote.Latest ? remote.Latest->SourceFrame : 0,
		    " shown=", displayed.Version, " shownFrame=", displayed.SourceFrame,
		    " exportFence=", displayed.Ready.Value, " importFence=", imported.Value,
		    " inputs=", displayed.Inputs, " generation=", remote.Generation,
		    " orderedInputs=", remote.Latest ? remote.Latest->Inputs : 0,
		    " firstConsumer=clustered deferred lighting consumerFence=", completions[remote.Consumer].Value);
	}
}
std::vector<Access> SceneEnvironment::Reads(Graph& graph, std::vector<ID3D12Resource*>& bindings) const
{
	std::vector<Access> accesses;
	for (const auto& texture : _environment._textures)
	{
		accesses.push_back(
		    {NativeWork::ResourceFor(graph, bindings, texture->GetResource()->D3DResource.Get(), "environment input"),
		     Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE});
	}
	for (size_t i = 0; i < 2; ++i)
	{
		const auto& texture = _environment._luts[i];
		if (texture->IsInitialized())
		{
			accesses.push_back({NativeWork::ResourceFor(graph, bindings, texture->GetResource()->D3DResource.Get(),
			                                            "atmosphere input"),
			                    Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE});
		}
	}
	return accesses;
}

void SceneEnvironment::AddAerialReads(Graph& graph, std::vector<ID3D12Resource*>& bindings, std::vector<Access>& accesses)
{
	if (_environment._source != SkySource::SkyAtmosphere) return;
	const std::array outputs{_environment._luts[3].get(), _environment._luts[4].get()};
	if (_remoteAerial) _remoteAerial->AppendImport(graph, bindings, outputs);
	for (size_t i = 0; i < outputs.size(); ++i)
	{
		const Id id = NativeWork::ResourceFor(graph, bindings, outputs[i]->GetResource()->D3DResource.Get(),
		                                     "camera aerial perspective");
		if (_remoteAerial) _remoteAerial->DisplayResources[i] = id;
		accesses.push_back({id, Mode::Read, Read});
	}
}

} // namespace BistroRender
