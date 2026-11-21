#include "SceneEnvironment.h"
#include "Common/ConsoleVariables.h"
#include "NativeWork.h"
#include "Engine.Core/Logging/Logger.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <algorithm>
#include <cmath>
#include <cstring>
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
    L"Remote camera SkyView: 1 displays the current requested LUT.",
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

struct SceneEnvironment::RemoteSkyView
{
	struct Publication
	{
		QueueSubmission::TransferBuffer Shared;
		std::unique_ptr<GDX12UploadBuffer<GDX12SkyConstants>> Constants;
		GDX12SkyConstants Key;
		uint64_t Version = 0, SourceFrame = 0, Inputs = 0;
		CompletionToken Ready, Reader;
	};

	std::shared_ptr<QueueSubmission> Submit;
	Id Device;
	GDX12DeviceResources Resources;
	std::array<GDX12Texture, 3> Luts;
	std::unique_ptr<GDX12RootSignature> Root;
	std::array<ComPtr<ID3D12PipelineState>, 3> Pipelines;
	std::vector<Publication> Ring;
	Executor Recorder;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint{};
	uint64_t Bytes = 0, Ordered = 0, InputVersion = 0, Imported = 0;
	UINT Next = 0;
	GDX12SkyConstants Medium, Multiple;
	Publication *Latest = nullptr, *Selected = nullptr;
	NativeWork RemoteWork, PrimaryWork;
	Schedule Plan;
	std::vector<ID3D12Resource*> Bindings;
	Id ImportOperation = Invalid, ImportPacket = Invalid, Consumer = Invalid;
	unsigned Reports = 0;

	RemoteSkyView(Executor& primary, Id device, UINT slots, const std::array<GDX12Texture*, 3>& source)
	    : Submit(primary.Submissions()), Device(device), Ring(slots + 2), Recorder(Submit, device, slots + 2, 1)
	{
		Resources.Device = &Submit->Device(device);
		Resources.SRV_UAV_Heap = std::make_shared<GDX12DescriptorHeap>(Resources.Device,
		    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, TextureResources_StartIndex + TextureResources_RangeLength,
		    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
		for (size_t i = 0; i < Luts.size(); ++i)
			GDX12RenderHelpers::InitializeComputeTexture(Luts[i], Resources, source[i]->GetWidth(),
			    source[i]->GetHeight(), source[i]->GetFormat(), false);
		GDX12RootSignatureDesc root;
		root.NumSingleCBVSlots = 1;
		root.NumSingleSRVSlots = 2;
		root.NumSingleUAVSlots = 1;
		root.StaticSamplers.emplace_back(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
		    D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
		Root = std::make_unique<GDX12RootSignature>(Resources.Device, root);
		constexpr const char* entries[]{"TransmittanceCS", "MultipleScatteringCS", "SkyViewCS"};
		for (size_t i = 0; i < Pipelines.size(); ++i)
			Pipelines[i] = GDX12RenderHelpers::CreateComputePipeline(Resources.Device, Root.get(),
			    SHADERS_FOLDER "SkyAtmosphere.hlsl", entries[i]);
		const auto desc = source[2]->GetResource()->D3DResource->GetDesc();
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT other{};
		uint64_t otherBytes = 0;
		Submit->Device(0).GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &Footprint, nullptr, nullptr, &Bytes);
		Resources.Device->GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &other, nullptr, nullptr, &otherBytes);
		if (!Bytes || Bytes == UINT64_MAX || Bytes != otherBytes || Footprint.Footprint.RowPitch != other.Footprint.RowPitch)
			throw std::runtime_error("SkyView adapters need matching texture footprints");
	}

	bool Matches(const std::array<GDX12Texture*, 3>& source)
	{
		for (size_t i = 0; i < Luts.size(); ++i)
			if (Luts[i].GetWidth() != source[i]->GetWidth() || Luts[i].GetHeight() != source[i]->GetHeight() ||
			    Luts[i].GetFormat() != source[i]->GetFormat()) return false;
		return true;
	}

	Publication* PrepareOrder(const GDX12SkyConstants& key, const GDX12SkyConstants& medium,
	                          const GDX12SkyConstants& multiple)
	{
		if (Latest && !Different(Latest->Key, key)) return nullptr;
		const UINT index = Next++ % UINT(Ring.size());
		auto& slot = Ring[index];
		if (&slot == Latest) throw std::logic_error("Overwriting the latest SkyView request");
		Recorder.Begin(index);
		if (slot.Reader && !Submit->Complete(slot.Reader))
			throw std::logic_error("SkyView transfer slot still in use by GPU0");
		if (!slot.Constants)
			slot.Constants = std::make_unique<GDX12UploadBuffer<GDX12SkyConstants>>(Resources.Device, 1);
		if (!slot.Shared.Resources[0]) slot.Shared = Submit->CreateTransferBuffer(Device, Bytes);
		slot.Constants->CopyData(0, key);
		slot.Key = key;
		slot.Reader = {};
		const bool transmittance = InputVersion == 0 || Different(Medium, medium);
		const bool scattering = transmittance || Different(Multiple, multiple);
		slot.Inputs = InputVersion + scattering;

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
		const auto constants = access(slot.Constants->GetResource().D3DResource.Get(), "SkyView constants",
		    Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ);
		const auto dispatch = [&](UINT stage, const char* name) {
			std::vector<Access> accesses{constants, access(native(Luts[stage]), name, Mode::Write, UnorderedAccess)};
			if (stage >= 1) accesses.push_back(access(native(Luts[0]), "SkyView transmittance", Mode::Read, read));
			if (stage == 2) accesses.push_back(access(native(Luts[1]), "SkyView multiple scattering", Mode::Read, read));
			const auto cb = slot.Constants->GetElementAddress(0);
			RemoteWork.Add(graph, name, std::move(accesses), [this, stage, cb](auto* cmd) {
				auto* heap = Resources.SRV_UAV_Heap->GetHeap().Get();
				cmd->SetDescriptorHeaps(1, &heap);
				cmd->SetComputeRootSignature(Root->GetRootSignature().Get());
				cmd->SetPipelineState(Pipelines[stage].Get());
				cmd->SetComputeRootConstantBufferView(Root->GetBRootParamIndex(0), cb);
				if (stage >= 1) cmd->SetComputeRootDescriptorTable(Root->GetTRootParamIndex(0), Luts[0].GetSRV()->GPUHandle);
				if (stage == 2) cmd->SetComputeRootDescriptorTable(Root->GetTRootParamIndex(1), Luts[1].GetSRV()->GPUHandle);
				auto& output = Luts[stage];
				cmd->SetComputeRootDescriptorTable(Root->GetURootParamIndex(0), output.GetUAV()->GPUHandle);
				cmd->Dispatch((output.GetWidth() + 7) / 8, (output.GetHeight() + 7) / 8, 1);
			}, {}, Queue::Compute);
		};
		if (transmittance) dispatch(0, "SkyView secondary transmittance");
		if (scattering) dispatch(1, "SkyView secondary multiple scattering");
		dispatch(2, "SkyView secondary camera LUT");
		auto* shared = slot.Shared.Resources[1].Get();
		auto* output = native(Luts[2]);
		RemoteWork.Add(graph, "SkyView export", {
		    access(output, "secondary SkyView", Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE),
		    access(shared, "shared SkyView", Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST)},
		    [source = CD3DX12_TEXTURE_COPY_LOCATION(output, 0u),
		     dest = CD3DX12_TEXTURE_COPY_LOCATION(shared, Footprint)](auto* cmd) {
			    cmd->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
		    }, {}, Queue::Compute);
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
		InputVersion = slot.Inputs;
		Medium = medium;
		Multiple = multiple;
		Latest = &slot;
	}

	void AppendImport(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Texture& output)
	{
		PrimaryWork.Commands.clear();
		ImportOperation = ImportPacket = Consumer = Invalid;
		auto* target = output.GetResource()->D3DResource.Get();
		const Id display = NativeWork::ResourceFor(graph, bindings, target, "visible SkyView LUT");
		if (!Selected || Selected->Version == Imported) return;
		auto* shared = Selected->Shared.Resources[0].Get();
		const Id buffer = NativeWork::ResourceFor(graph, bindings, shared, "shared SkyView");
		ImportOperation = PrimaryWork.Add(graph, "GPU0 import visible SkyView",
		    {{buffer, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE}, {display, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
		    [source = CD3DX12_TEXTURE_COPY_LOCATION(shared, Footprint),
		     dest = CD3DX12_TEXTURE_COPY_LOCATION(target, 0u)](auto* cmd) {
			    cmd->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
		    }, {}, Queue::Copy);
	}
};
SceneEnvironment::SceneEnvironment(GDX12DeviceResources& r, RenderPipelineCommonData& c) : _resources(r), _common(c)
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
}

bool SceneEnvironment::PrepareSkyView(Executor& primary, uint64_t frame, bool atmosphere, bool& localChanged)
{
	const int requested = atmosphere && SkyViewCrossSync.GetValue() == 1 ? SkyViewAdapter.GetValue() : -1;
	if (_failedSkyViewAdapter != requested) _failedSkyViewAdapter = -1;
	const auto& submit = primary.Submissions();
	const Id device = requested >= 0 && requested != _failedSkyViewAdapter ? submit->SelectAdapter(requested) : 0;
	const auto& luts = _environment._luts;
	const std::array sources{luts[0].get(), luts[1].get(), luts[2].get()};
	const auto retire = [&] {
		if (!_remoteSkyView) return;
		primary.Drain();
		_remoteSkyView->Recorder.Drain();
		_remoteSkyView.reset();
		localChanged = true;
	};
	if (_remoteSkyView && (_remoteSkyView->Device != device || !_remoteSkyView->Matches(sources))) retire();
	if (!device) return false;
	RemoteSkyView::Publication* order = nullptr;
	const auto fallback = [&](const auto& reason) {
		submit->CheckHealth();
		retire();
		_failedSkyViewAdapter = requested;
		Log::Warning("RenderGraph", "Using local SkyView: ", reason);
	};
	try
	{
		if (!_remoteSkyView)
			_remoteSkyView = std::make_unique<RemoteSkyView>(primary, device, UINT(_resources.FrameConstants.size()), sources);
		order = _remoteSkyView->PrepareOrder(_viewConstants, _mediumConstants, _multipleConstants);
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
	auto& remote = *_remoteSkyView;
	if (order) remote.Publish(*order, _mediumConstants, _multipleConstants, frame);
	remote.Selected = remote.Latest;
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
	const bool remoteSkyView = PrepareSkyView(primary, frameNumber, state.Source == SkySource::SkyAtmosphere, viewChanged);
	auto& frame = *r.FrameConstants[r.CurrFrameConstantsIndex];
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
		if (aerialChanged)
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
	if (_remoteSkyView) _remoteSkyView->AppendImport(graph, bindings, *_environment._luts[2]);
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
			const auto found = std::find(bindings.begin(), bindings.end(), native);
			access.Resource = Id(found - bindings.begin());
			if (found == bindings.end())
			{
				D3D12_HEAP_PROPERTIES heap{};
				D3D12_HEAP_FLAGS flags{};
				ThrowIfFailed(native->GetHeapProperties(&heap, &flags));
				const bool upload = heap.Type == D3D12_HEAP_TYPE_UPLOAD;
				graph.Resources.push_back({work.Name + " resource " + std::to_string(access.Resource),
				                           upload ? State(D3D12_RESOURCE_STATE_GENERIC_READ) : 0,
				                           upload ? State(D3D12_RESOURCE_STATE_GENERIC_READ) : 0,
				                           (native->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0,
				                           !upload && native->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER,
				                           upload ? Heap::Upload : Heap::Default});
				bindings.push_back(native);
			}
			operation.Accesses.push_back(access);
		}
		_operations.push_back(Id(graph.Operations.size()));
		graph.Operations.push_back(std::move(operation));
	}
}
bool SceneEnvironment::Record(Id operation, ID3D12GraphicsCommandList* cmd) const
{
	if (_remoteSkyView && _remoteSkyView->PrimaryWork.Record(operation, cmd)) return true;
	const auto found = std::lower_bound(_operations.begin(), _operations.end(), operation);
	if (found == _operations.end() || *found != operation)
	{
		return false;
	}
	_work[found - _operations.begin()].Record(cmd);
	return true;
}

void SceneEnvironment::AddSkyViewWaits(const Schedule& schedule, std::vector<PacketWait>& waits)
{
	if (!_remoteSkyView) return;
	auto& remote = *_remoteSkyView;
	for (Id i = 0; i < schedule.Packets.size(); ++i)
	{
		const auto& packet = schedule.Packets[i];
		if (remote.ImportOperation != Invalid && packet.Operation == remote.ImportOperation) remote.ImportPacket = i;
		if (packet.Name == "clustered deferred lighting") remote.Consumer = i;
	}
	if (remote.ImportOperation != Invalid)
	{
		if (remote.ImportPacket >= remote.Consumer || remote.Consumer >= schedule.Packets.size())
			throw std::logic_error("SkyView import must precede visible deferred lighting");
		waits.push_back({remote.ImportPacket, remote.Selected->Ready});
	}
}

void SceneEnvironment::SkyViewSubmitted(uint64_t frame, const std::vector<CompletionToken>& completions)
{
	if (!_remoteSkyView) return;
	auto& remote = *_remoteSkyView;
	const auto imported = remote.ImportPacket < completions.size() ? completions[remote.ImportPacket] : CompletionToken{};
	if (imported)
	{
		remote.Selected->Reader = imported;
		remote.Imported = remote.Selected->Version;
	}
	if (remote.Consumer >= completions.size() || !completions[remote.Consumer] || remote.Reports >= 6) return;
	++remote.Reports;
	const auto& displayed = *remote.Selected;
	Log::Info("RenderGraph", "Visible SkyView CrossSync=1 frame=", frame,
	    " GPU=", remote.Submit->AdapterLabel(remote.Device), " displayGPU=", remote.Submit->AdapterLabel(0),
	    " ordered=", remote.Latest->Version, " orderedFrame=", remote.Latest->SourceFrame,
	    " shown=", displayed.Version, " shownFrame=", displayed.SourceFrame,
	    " inputs=", displayed.Inputs, " exportFence=", displayed.Ready.Value, " importFence=", imported.Value,
	    " firstConsumer=clustered deferred lighting consumerFence=", completions[remote.Consumer].Value);
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
	for (size_t i = 0; i < _environment._luts.size(); ++i)
	{
		if (i == 2) continue; // Only deferred sky samples the camera LUT.
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

} // namespace BistroRender
