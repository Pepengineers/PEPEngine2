#include "CloudPictureTexture.h"
#include "NativeWork.h"
#include "../Graph/RenderGraphExecutor.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.Core/Logging/Logger.h"
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <stdexcept>

namespace BistroRender
{
namespace
{
AutoConsoleVariable<bool> AsyncCloudPicture(L"App.UI.CloudPicture.AsyncCompute", false,
                                            L"Run local Cloud Picture generation on the COMPUTE queue.",
                                            EConsoleVariableFlags::Default);
AutoConsoleVariable<float> SpeedScale(L"App.UI.CloudPicture.SpeedScale", 4.f,
                                      L"Cloud picture animation speed multiplier; 1 is the original animation speed.",
                                      EConsoleVariableFlags::Default,
                                      [](float value) { return std::isfinite(value) && value >= 0 && value <= 100; });
constexpr DXGI_FORMAT Format = DXGI_FORMAT_R8G8B8A8_UNORM;
struct CloudGeneratorData
{
	float CloudCover = 0.2f, CloudAlpha = 8.f, SkyTint = 0.5f, TotalTime = 0;
	float CloudScale = 1.1f, CloudSpeed = 0.006f, CloudDark = 0.5f, CloudLight = 0.3f;
	XMFLOAT4 SkyColor1{0.2f, 0.4f, 0.6f, 1.f}, SkyColor2{0.4f, 0.7f, 1.f, 1.f};
	XMFLOAT4 CloudColor{1.1f, 1.1f, 0.9f, 1.f};
	XMFLOAT2 RenderTargetSize{}, Padding{};
};
static_assert(sizeof(CloudGeneratorData) == 96, "Cloud picture root constants must match the shader");
using Clock = std::chrono::steady_clock;
using Event = std::unique_ptr<void, decltype(&CloseHandle)>;

Event MakeEvent(BOOL manualReset)
{
	Event event(CreateEventW(nullptr, manualReset, FALSE, nullptr), CloseHandle);
	if (!event) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
	return event;
}

void CheckFormat(GDX12Device& device, bool producer)
{
	D3D12_FEATURE_DATA_FORMAT_SUPPORT support{Format};
	ThrowIfFailed(device.GetDevice()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
	if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) ||
	    !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) ||
	    (producer && !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)))
	{
		throw std::runtime_error("Cloud picture requires RGBA8 UNORM texture sampling and typed UAV stores");
	}
}
} // namespace

struct CloudPictureTexture::Impl
{
	struct SharedSlot
	{
		QueueSubmission::TransferBuffer Buffer;
		CompletionToken Export;
		// With Async.Queues off the import is compiled onto DIRECT instead of COPY.
		std::array<CompletionToken, 3> Readers{};
		uint64_t Version = 0;
	};
	struct RemoteProducer
	{
		std::vector<SharedSlot> Slots;
		Schedule ExportSchedule;
		std::unique_ptr<Executor> FrameExecutor;
		std::unique_ptr<GDX12DescriptorHeap> Heap;
		GDX12ResourceHandle Output;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint{};
		std::mutex Mutex;
		std::condition_variable Changed;
		Event StopEvent = MakeEvent(TRUE), ExportEvent = MakeEvent(FALSE);
		std::thread Thread;
		std::exception_ptr Failure;
		bool Stop = false, DrainFailed = false;
		// IDs and tokens are scoped to this resource generation (one RemoteProducer).
		Id Latest = Invalid, Pending = Invalid, Pinned = Invalid;
		Id LastOrder = Invalid; // Previous framed order; its slot holds the version and export token.
		uint64_t Exports = 0, Frames = 0, Imports = 0, Repeats = 0, ImportedVersion = 0;
	};

	GDX12DeviceResources& Primary;
	std::shared_ptr<QueueSubmission> Submit;
	Id DeviceId;
	const CloudPictureMode Mode;
	std::unique_ptr<RemoteProducer> Remote;
	std::unique_ptr<GDX12RootSignature> Root;
	ComPtr<ID3D12PipelineState> Pipeline;
	std::vector<std::unique_ptr<GDX12Texture>> Slots;
	NativeWork Work;
	CloudGeneratorData Parameters;
	uint64_t Sequence = 0;
	UINT Width, Height, CurrentSlot = 0;
	Id ImportOperation = Invalid, ImportPacket = Invalid;

	Impl(GDX12DeviceResources& primary, UINT width, UINT height, std::shared_ptr<QueueSubmission> submit, Id deviceId, CloudPictureMode mode)
	    : Primary(primary), Submit(std::move(submit)), DeviceId(deviceId), Mode(mode), Slots(primary.FrameConstants.size()), Width(width),
	      Height(height)
	{
		if (!primary.Device || Slots.empty() || !width || !height || width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
		    height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
		{
			throw std::invalid_argument("Cloud picture grid must be nonzero and within D3D12 Texture2D limits");
		}
		const bool remote = mode != CloudPictureMode::Local;
		auto& device = Submit->Device(DeviceId);
		CheckFormat(*primary.Device, !remote);
		if (remote)
		{
			CheckFormat(device, true);
			InitializeRemote(device);
		}
		else
		{
			Submit.reset(); // Local clouds do not own the secondary submission domain.
		}
		GDX12RootSignatureDesc root;
		root.Constants = {sizeof(CloudGeneratorData) / sizeof(UINT)};
		root.NumSingleUAVSlots = 1;
		Root = std::make_unique<GDX12RootSignature>(&device, root);
		Pipeline = GDX12RenderHelpers::CreateComputePipeline(&device, Root.get(),
		                                                     SHADERS_FOLDER "CloudPictureNoise.hlsl", "CS");
		Parameters.RenderTargetSize = {float(width), float(height)};
		for (auto& texture : Slots)
		{
			texture = std::make_unique<GDX12Texture>();
			GDX12TextureDesc target;
			target.Width = width;
			target.Height = height;
			target.Format = Format;
			target.Semantic = ETextureSemantic::Color;
			target.SRV_UAV_Heap = primary.SRV_UAV_Heap.get();
			target.SRVHeapIndex = target.SRV_UAV_Heap->GetAvailableIndex(Texture2D_StartIndex, Texture2D_RangeLength);
			target.SRVDesc.Format = Format;
			target.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			target.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			target.SRVDesc.Texture2D.MipLevels = 1;
			if (!remote)
			{
				target.CreateUAV = true;
				target.UAVHeapIndex =
				    target.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
				target.UAVDesc.Format = Format;
				target.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			}
			texture->Initialize(target);
		}
		Log::Info("RenderGraph", "CloudPicture effective=", width, "x", height,
		          " format=RGBA8_UNORM frameSlots=", Slots.size(), " remote=", remote,
		          " CrossSync=", int(Mode));
	}
	void InitializeRemote(GDX12Device& device)
	{
		Remote = std::make_unique<RemoteProducer>();
		auto& remote = *Remote;
		const bool frameSync = Mode != CloudPictureMode::Independent;
		const size_t historySlots = Mode == CloudPictureMode::PreviousFrame ? 1 : 0;
		remote.Slots.resize(frameSync ? Slots.size() + historySlots : 3);
		remote.ExportSchedule = BuildExportSchedule();
		if (frameSync)
			remote.FrameExecutor = std::make_unique<Executor>(Submit, DeviceId, UINT(Slots.size()), 1);
		const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
		    Format, Width, Height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		uint64_t bytes = 0, primaryBytes = 0;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT primaryFootprint{};
		device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &remote.Footprint, nullptr, nullptr, &bytes);
		Primary.Device->GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &primaryFootprint, nullptr, nullptr,
		                                                  &primaryBytes);
		if (!bytes || bytes == UINT64_MAX || bytes != primaryBytes ||
		    primaryFootprint.Footprint.RowPitch != remote.Footprint.Footprint.RowPitch)
		{
			throw std::runtime_error("CloudPicture adapters need matching RGBA8 transfer footprints");
		}
		remote.Output = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
		remote.Heap = std::make_unique<GDX12DescriptorHeap>(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
		                                                  D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = Format;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device.GetDevice()->CreateUnorderedAccessView(remote.Output.Get(), nullptr, &uav, remote.Heap->GetCPUHandle(0));
		for (auto& slot : remote.Slots)
		{
			slot.Buffer = Submit->CreateTransferBuffer(DeviceId, bytes);
		}
		Log::Info("RenderGraph", "CloudPicture transferBytes=", bytes, " rowPitch=", remote.Footprint.Footprint.RowPitch,
		          " sharedSlots=", remote.Slots.size());
	}
	void Dispatch(ID3D12GraphicsCommandList* cmd, const CloudGeneratorData& parameters, D3D12_GPU_DESCRIPTOR_HANDLE uav)
	{
		auto* heap = (Remote ? Remote->Heap.get() : Primary.SRV_UAV_Heap.get())->GetHeap().Get();
		cmd->SetDescriptorHeaps(1, &heap);
		cmd->SetComputeRootSignature(Root->GetRootSignature().Get());
		cmd->SetPipelineState(Pipeline.Get());
		cmd->SetComputeRoot32BitConstants(Root->GetBRootParamIndex(0), sizeof(parameters) / sizeof(UINT), &parameters,
		                                  0);
		cmd->SetComputeRootDescriptorTable(Root->GetURootParamIndex(0), uav);
		cmd->Dispatch((Width + 31) / 32, (Height + 31) / 32, 1);
	}
	// Both owners may observe completion first. The mutex makes publication and pinning atomic.
	void PublishCompleted()
	{
		auto& remote = *Remote;
		if (remote.Pending != Invalid && Submit->Complete(remote.Slots[remote.Pending].Export))
		{
			remote.Latest = remote.Pending;
			remote.Pending = Invalid;
			++remote.Exports;
		}
	}
	Id WritableSlot() const
	{
		const auto& remote = *Remote;
		for (Id id = 0; id < remote.Slots.size(); ++id)
		{
			if (id == remote.Latest || id == remote.Pinned)
			{
				continue;
			}
			const auto& readers = remote.Slots[id].Readers;
			if (std::all_of(readers.begin(), readers.end(),
			                [this](auto token) { return !token || Submit->Complete(token); }))
			{
				return id;
			}
		}
		return Invalid;
	}
	void WaitForExport(CompletionToken token)
	{
		auto& remote = *Remote;
		Submit->NotifyOnCompletion(token, remote.ExportEvent.get());
		const HANDLE events[] = {remote.StopEvent.get(), remote.ExportEvent.get()};
		for (;;)
		{
			const auto result = WaitForMultipleObjects(2, events, FALSE, 100);
			if (result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1) return;
			if (result == WAIT_FAILED) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
			// The other adapter can fail without signalling this fence.
			Submit->CheckHealth();
		}
	}
	Schedule BuildExportSchedule() const
	{
		Graph graph;
		graph.Device = DeviceId;
		graph.FrameBoundary = false;
		graph.TerminalQueue = Queue::Compute;
		graph.Resources = {{"cloud producer UAV", 0, 0, true}, {"cloud shared export buffer", 0, 0, false, true}};
		graph.Operations = {
		    {"cloud picture GPU1 generation", {{0, Mode::Write, UnorderedAccess}}},
		    {"cloud picture GPU1 export",
		     {{0, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE}, {1, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}}}};
		for (auto& operation : graph.Operations)
		{
			operation.Target = Queue::Compute;
		}
		return Compile(graph);
	}
	CompletionToken SubmitExport(Executor& executor, SharedSlot& slot, const CloudGeneratorData& parameters)
	{
		auto& remote = *Remote;
		auto* output = remote.Output.Get();
		auto* shared = slot.Buffer.Resources[1].Get();
		executor.Reserve(remote.ExportSchedule);
		executor.Execute(remote.ExportSchedule, {output, shared},
		    [&](Id operation, uint32_t, auto* cmd)
		    {
			    if (operation == 0)
			    {
				    Dispatch(cmd, parameters, remote.Heap->GetGPUHandle(0));
			    }
			    else
			    {
				    const CD3DX12_TEXTURE_COPY_LOCATION source(output, 0u), target(shared, remote.Footprint);
				    cmd->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
			    }
		    });
		return executor.Completions().back();
	}
	void Produce(Executor& executor, double phase, Clock::time_point epoch)
	{
		auto& remote = *Remote;
		std::unique_lock lock(remote.Mutex);
		while (!remote.Stop)
		{
			PublishCompleted();
			const Id id = WritableSlot();
			if (id == Invalid)
			{
				// The remaining slots may have GPU0 readers on different queues.
				remote.Changed.wait_for(lock, std::chrono::milliseconds(1));
				continue;
			}
			auto parameters = Parameters;
			auto& slot = remote.Slots[id];
			slot.Readers = {};
			lock.unlock();

			if (!executor.TryBegin(0))
			{
				throw std::logic_error("CloudPicture export completed before its recording slot retired");
			}
			parameters.TotalTime = float(phase + std::chrono::duration<double>(Clock::now() - epoch).count());
			const auto token = SubmitExport(executor, slot, parameters);
			lock.lock();
			slot.Export = token;
			slot.Version = remote.Exports + 1;
			remote.Pending = id;
			remote.Changed.notify_all();
			lock.unlock();
			WaitForExport(token);
			lock.lock();
		}
	}
	void RunProducer(double phase, Clock::time_point epoch)
	{
		auto& remote = *Remote;
		const auto fail = [&]
		{
			std::lock_guard lock(remote.Mutex);
			if (!remote.Failure) remote.Failure = std::current_exception();
			remote.Changed.notify_all();
		};
		try
		{
			Executor executor(Submit, DeviceId, 1, 1);
			try { Produce(executor, phase, epoch); }
			catch (...) { fail(); }
			try
			{
				executor.Drain();
				std::lock_guard lock(remote.Mutex);
				PublishCompleted();
			}
			catch (...)
			{
				remote.DrainFailed = true;
				fail();
			}
		}
		catch (...) { fail(); }
	}
	void Stop()
	{
		if (!Remote || !Remote->Thread.joinable()) return;
		{
			std::lock_guard lock(Remote->Mutex);
			Remote->Stop = true;
		}
		SetEvent(Remote->StopEvent.get());
		Remote->Changed.notify_all();
		Remote->Thread.join();
	}
	void Drain()
	{
		if (!Remote) return;
		Stop();
		if (Remote->FrameExecutor) Remote->FrameExecutor->Drain();
		if (Remote->DrainFailed)
		{
			throw std::runtime_error("CloudPicture GPU1 could not retire submitted commands");
		}
		Submit->CheckHealth();
		for (const auto& slot : Remote->Slots)
		{
			for (auto token : slot.Readers)
			{
				if (token) Submit->Wait(token);
			}
		}
		const auto primary = Submit->Device(0).GetDevice()->GetAdapterLuid();
		const auto secondary = Submit->Device(DeviceId).GetDevice()->GetAdapterLuid();
		Log::Info("RenderGraph", "CloudPicture MGPU summary GPU0 LUID=", primary.HighPart, ":", primary.LowPart,
		          " GPU1 LUID=", secondary.HighPart, ":", secondary.LowPart, " exports=", Remote->Exports,
		          " activeFrames=", Remote->Frames, " imports=", Remote->Imports, " repeatedImports=", Remote->Repeats);
	}

};

CloudPictureTexture::CloudPictureTexture(GDX12DeviceResources& primary, UINT width, UINT height,
                                         std::shared_ptr<QueueSubmission> submit, Id deviceId, CloudPictureMode mode)
    : _impl(std::make_unique<Impl>(primary, width, height, std::move(submit), deviceId, mode))
{
}
CloudPictureTexture::~CloudPictureTexture()
{
	try { _impl->Drain(); }
	catch (...)
	{
		OutputDebugStringA("CloudPicture drain failed; retaining in-flight resources until process exit.\n");
		_impl.release();
	}
}

void CloudPictureTexture::Stop() { _impl->Stop(); }
GDX12Texture* CloudPictureTexture::Texture(UINT frameSlot) const
{
	return !_impl->Remote || _impl->Remote->Pinned != Invalid ? _impl->Slots.at(frameSlot).get() : nullptr;
}
void CloudPictureTexture::Prepare(UINT frameSlot, double totalTime)
{
	auto& p = *_impl;
	if (!std::isfinite(totalTime) || totalTime < 0 || totalTime > (std::numeric_limits<float>::max)())
	{
		throw std::invalid_argument("Cloud picture requires finite nonnegative animation time");
	}
	p.CurrentSlot = frameSlot;
	p.Work.Commands.clear();
	p.ImportOperation = p.ImportPacket = Invalid;
	if (p.Remote)
	{
		auto& remote = *p.Remote;
		if (remote.FrameExecutor)
		{
			const bool previousFrame = p.Mode == CloudPictureMode::PreviousFrame;
			const Id previous = remote.LastOrder;
			// The export ring advances with orders. GPU0's display and allocator slots can cycle separately.
			const Id output = previousFrame
			                      ? (previous == Invalid ? 0 : Id((previous + 1) % remote.Slots.size()))
			                      : frameSlot;
			auto& slot = remote.Slots.at(output);
			remote.FrameExecutor->Begin(frameSlot);
			for (auto reader : slot.Readers)
			{
				if (reader) p.Submit->Wait(reader);
			}
			slot.Readers = {};
			p.Parameters.TotalTime = float(totalTime);
			p.Parameters.CloudSpeed = 0.006f * SpeedScale.GetValue();
			slot.Export = p.SubmitExport(*remote.FrameExecutor, slot, p.Parameters);
			slot.Version = ++remote.Exports;
			remote.LastOrder = output;
			remote.Pinned = previousFrame && previous != Invalid ? previous : output;
			return;
		}
		std::unique_lock lock(remote.Mutex);
		p.Parameters.CloudSpeed = 0.006f * SpeedScale.GetValue();
		if (!remote.Thread.joinable())
		{
			remote.Thread = std::thread([&p, totalTime, epoch = Clock::now()] { p.RunProducer(totalTime, epoch); });
		}
		// Bootstrap waits only for an issued token. The GPU import waits for its bytes.
		remote.Changed.wait(lock, [&]
		{
			return remote.Pending != Invalid || remote.Latest != Invalid || remote.Failure;
		});
		if (remote.Failure) std::rethrow_exception(remote.Failure);
		p.PublishCompleted();
		remote.Pinned = remote.Latest != Invalid ? remote.Latest : remote.Pending;
		return;
	}
	p.Parameters.TotalTime = float(totalTime);
	p.Parameters.CloudSpeed = 0.006f * SpeedScale.GetValue();
	++p.Sequence;
	if (p.Sequence == 1 || p.Sequence == 61)
	{
		Log::Info("RenderGraph", "CloudPicture frame publication sequence=", p.Sequence, " renderTime=", totalTime);
	}
}
void CloudPictureTexture::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings)
{
	auto& p = *_impl;
	const auto& texture = p.Slots.at(p.CurrentSlot);
	const Id output = NativeWork::ResourceFor(graph, bindings, texture->GetResource()->D3DResource.Get(),
	                                          "cloud picture material texture");
	if (p.Remote)
	{
		auto& remote = *p.Remote;
		auto* shared = remote.Slots.at(remote.Pinned).Buffer.Resources[0].Get();
		auto* display = texture->GetResource()->D3DResource.Get();
		const Id input = NativeWork::ResourceFor(graph, bindings, shared, "cloud shared import buffer");
		p.ImportOperation = p.Work.Add(graph, "cloud picture GPU0 import",
		    {{input, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
		     {output, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
		    [shared, display, footprint = remote.Footprint](auto* cmd)
		    {
			    const CD3DX12_TEXTURE_COPY_LOCATION source(shared, footprint), target(display, 0u);
			    cmd->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
		    }, {}, Queue::Copy);
		return;
	}
	const auto parameters = p.Parameters;
	const auto uav = texture->GetUAV()->GPUHandle;
	p.Work.Add(graph, "cloud picture generation", {{output, Mode::Write, UnorderedAccess}},
	           [&p, parameters, uav](auto* cmd) { p.Dispatch(cmd, parameters, uav); }, {},
	           AsyncCloudPicture.GetValue() ? Queue::Compute : Queue::Direct);
}
bool CloudPictureTexture::Record(Id id, ID3D12GraphicsCommandList* cmd)
{
	return _impl->Work.Record(id, cmd);
}
std::vector<PacketWait> CloudPictureTexture::Waits(const Schedule& schedule)
{
	auto& p = *_impl;
	if (p.ImportOperation == Invalid) return {};
	for (Id id = 0; id < schedule.Packets.size(); ++id)
	{
		if (schedule.Packets[id].Operation == p.ImportOperation)
		{
			p.ImportPacket = id;
			return {{id, p.Remote->Slots.at(p.Remote->Pinned).Export}};
		}
	}
	throw std::logic_error("CloudPicture import missing from the primary schedule");
}

bool CloudPictureTexture::IsRemote() const { return bool(_impl->Remote); }

void CloudPictureTexture::Presented()
{
	if (!_impl->Remote) return;
	auto& remote = *_impl->Remote;
	++remote.Frames;
	if (remote.FrameExecutor && remote.Frames <= 4)
		Log::Info("RenderGraph", "CloudPicture CrossSync=", int(_impl->Mode), " presented frame=", remote.Frames,
		          " ordered version=", remote.Slots.at(remote.LastOrder).Version,
		          " imported version=", remote.ImportedVersion);
}

void CloudPictureTexture::Submitted(const std::vector<CompletionToken>& completions)
{
	auto& p = *_impl;
	if (!p.Remote) return;
	auto& remote = *p.Remote;
	{
		std::lock_guard lock(remote.Mutex);
		const auto token = p.ImportPacket < completions.size() ? completions[p.ImportPacket] : CompletionToken{};
		if (token)
		{
			auto& slot = remote.Slots.at(remote.Pinned);
			const size_t queue = token.Target == Queue::Direct ? 0 : token.Target == Queue::Compute ? 1 : 2;
			slot.Readers[queue] = token;
			if (++remote.Imports == 1) p.Submit->LogFirstImport("CloudPicture", slot.Export, token);
			if (remote.ImportedVersion == slot.Version) ++remote.Repeats;
			remote.ImportedVersion = slot.Version;
		}
		remote.Pinned = Invalid;
	}
	remote.Changed.notify_all();
}
} // namespace BistroRender
