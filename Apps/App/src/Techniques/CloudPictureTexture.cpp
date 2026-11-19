#include "CloudPictureTexture.h"
#include "NativeWork.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.Core/Logging/Logger.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace BistroRender
{
namespace
{
AutoConsoleVariable<bool> AsyncCloudPicture(L"App.Async.CloudPicture", false,
                                            L"Run local Cloud Picture generation on the COMPUTE queue.",
                                            EConsoleVariableFlags::Default);
AutoConsoleVariable<float> SpeedScale(L"App.CloudPicture.SpeedScale", 4.f,
                                      L"Cloud picture animation speed multiplier; 1 is the original MGPU-Noise speed.",
                                      EConsoleVariableFlags::Default,
                                      [](float value) { return std::isfinite(value) && value >= 0 && value <= 100; });
constexpr DXGI_FORMAT Format = DXGI_FORMAT_R8G8B8A8_UNORM;
std::atomic<uint64_t> Generations{0};
struct CloudGeneratorData
{
	float CloudCover = 0.2f, CloudAlpha = 8.f, SkyTint = 0.5f, TotalTime = 0;
	float CloudScale = 1.1f, CloudSpeed = 0.006f, CloudDark = 0.5f, CloudLight = 0.3f;
	XMFLOAT4 SkyColor1{0.2f, 0.4f, 0.6f, 1.f}, SkyColor2{0.4f, 0.7f, 1.f, 1.f};
	XMFLOAT4 CloudColor{1.1f, 1.1f, 0.9f, 1.f};
	XMFLOAT2 RenderTargetSize{}, Padding{};
};
static_assert(sizeof(CloudGeneratorData) == 96, "MGPU-Noise root constants must match the shader");
Id PacketFor(const Schedule& schedule, Id operation)
{
	for (Id i = 0; i < schedule.Packets.size(); ++i)
	{
		if (schedule.Packets[i].Operation == operation)
		{
			return i;
		}
	}
	throw std::logic_error("Cloud picture import missing from the primary plan");
}
void CheckFormat(GDX12Device& device, bool producer)
{
	D3D12_FEATURE_DATA_FORMAT_SUPPORT support{Format};
	ThrowIfFailed(device.GetDevice()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
	if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) ||
	    !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) ||
	    (producer && !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)))
	{
		throw std::runtime_error("Cloud picture requires RGBA8 UNORM texture sampling and producer typed UAV stores");
	}
}
} // namespace

struct CloudPictureTexture::Impl
{
	struct Publication
	{
		uint64_t Generation = 0, Sequence = 0;
		UINT Width = 0, Height = 0;
		DXGI_FORMAT PixelFormat = Format;
	};
	struct Slot
	{
		std::unique_ptr<GDX12Texture> Display;
		GDX12ResourceHandle Produced;
		QueueSubmission::TransferBuffer Transfer;
		D3D12_GPU_DESCRIPTOR_HANDLE Uav{};
		Publication Metadata;
		CompletionToken Ready;
		std::array<CompletionToken, 3> Readers{};
	};
	GDX12DeviceResources& Primary;
	std::shared_ptr<QueueSubmission> Submit;
	std::unique_ptr<Executor> Producer;
	std::unique_ptr<GDX12DescriptorHeap> ProducerHeap;
	std::unique_ptr<GDX12RootSignature> Root;
	ComPtr<ID3D12PipelineState> Pipeline;
	std::vector<Slot> Slots;
	NativeWork Work;
	CloudGeneratorData Parameters;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint{};
	uint64_t TransferBytes = 0, Generation = 0, Sequence = 0;
	UINT Width, Height, CurrentSlot = 0;
	Id ImportOperation = Invalid;
	bool Remote = false;

	Impl(GDX12DeviceResources& primary, UINT width, UINT height, std::shared_ptr<QueueSubmission> submit, bool remote)
	    : Primary(primary), Submit(std::move(submit)), Slots(primary.FrameConstants.size()), Width(width), Height(height),
	      Remote(remote)
	{
		if (!primary.Device || Slots.empty() || !width || !height || width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
		    height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
		{
			throw std::invalid_argument("Cloud picture grid must be nonzero and within D3D12 Texture2D limits");
		}
		Generation = ++Generations;
		if (!Generation)
		{
			throw std::overflow_error("Cloud picture generation exhausted");
		}
		auto& device = Submit->Device(Remote ? 1 : 0);
		const auto luid = device.GetAdapterLuid();
		CheckFormat(*primary.Device, !Remote);
		CheckFormat(device, true);
		if (Remote)
		{
			Producer = std::make_unique<Executor>(Submit, 1, UINT(Slots.size()), 1);
			ProducerHeap =
			    std::make_unique<GDX12DescriptorHeap>(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
			                                          UINT(Slots.size()), D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
		}
		GDX12RootSignatureDesc root;
		root.Constants = {sizeof(CloudGeneratorData) / sizeof(UINT)};
		root.NumSingleUAVSlots = 1;
		Root = std::make_unique<GDX12RootSignature>(&device, root);
		Pipeline = GDX12RenderHelpers::CreateComputePipeline(&device, Root.get(),
		                                                     SHADERS_FOLDER "CloudPictureNoise.hlsl", "CS");
		Parameters.RenderTargetSize = {float(width), float(height)};
		const auto producedDesc =
		    CD3DX12_RESOURCE_DESC::Tex2D(Format, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		UINT rows = 0;
		uint64_t rowBytes = 0;
		device.GetDevice()->GetCopyableFootprints(&producedDesc, 0, 1, 0, &Footprint, &rows, &rowBytes, &TransferBytes);
		if (rows != height || rowBytes != uint64_t(width) * 4 || Footprint.Footprint.RowPitch < rowBytes ||
		    TransferBytes == UINT64_MAX || !TransferBytes ||
		    uint64_t(Footprint.Footprint.RowPitch) > (UINT64_MAX - Footprint.Offset) / height ||
		    TransferBytes > uint64_t(Footprint.Footprint.RowPitch) * height + Footprint.Offset)
		{
			throw std::overflow_error("Cloud picture transfer footprint is invalid or overflows");
		}
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT primaryFootprint{};
		uint64_t primaryBytes = 0;
		primary.Device->GetDevice()->GetCopyableFootprints(&producedDesc, 0, 1, 0, &primaryFootprint, nullptr, nullptr,
		                                                   &primaryBytes);
		if (primaryBytes != TransferBytes || primaryFootprint.Footprint.RowPitch != Footprint.Footprint.RowPitch)
		{
			throw std::runtime_error("Cloud picture adapters disagree on the transfer footprint");
		}
		for (UINT index = 0; index < Slots.size(); ++index)
		{
			auto& slot = Slots[index];
			slot.Display = std::make_unique<GDX12Texture>();
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
			if (!Remote)
			{
				target.CreateUAV = true;
				target.UAVHeapIndex =
				    target.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
				target.UAVDesc.Format = Format;
				target.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			}
			slot.Display->Initialize(target);
			if (!Remote)
			{
				slot.Uav = slot.Display->GetUAV()->GPUHandle;
			}
			else
			{
				slot.Produced =
				    device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, producedDesc, D3D12_RESOURCE_STATE_COMMON);
				D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
				uav.Format = Format;
				uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				device.GetDevice()->CreateUnorderedAccessView(slot.Produced.Get(), nullptr, &uav,
				                                              ProducerHeap->GetCPUHandle(index));
				slot.Uav = ProducerHeap->GetGPUHandle(index);
				slot.Transfer = Submit->CreateTransferBuffer(TransferBytes);
			}
		}
		Log::Info("RenderGraph", "CloudPicture effective=", width, "x", height, " name=", device.GetDeviceFeatures().Name,
		          " LUID=", luid.HighPart, ":", luid.LowPart, " generation=", Generation,
		          " format=RGBA8_UNORM linear; strict=1 remote=", Remote, " rowPitch=", Footprint.Footprint.RowPitch,
		          " transferBytes=", Remote ? TransferBytes : 0, " slots=", Slots.size());
	}
	void Dispatch(ID3D12GraphicsCommandList* cmd, const CloudGeneratorData& parameters, D3D12_GPU_DESCRIPTOR_HANDLE uav)
	{
		auto* heap = Remote ? ProducerHeap->GetHeap().Get() : Primary.SRV_UAV_Heap->GetHeap().Get();
		cmd->SetDescriptorHeaps(1, &heap);
		cmd->SetComputeRootSignature(Root->GetRootSignature().Get());
		cmd->SetPipelineState(Pipeline.Get());
		cmd->SetComputeRoot32BitConstants(Root->GetBRootParamIndex(0), sizeof(parameters) / sizeof(UINT), &parameters,
		                                  0);
		cmd->SetComputeRootDescriptorTable(Root->GetURootParamIndex(0), uav);
		cmd->Dispatch((Width + 31) / 32, (Height + 31) / 32, 1);
	}
	void CheckPublication(const Slot& slot) const
	{
		if (slot.Metadata.Generation != Generation || !slot.Metadata.Sequence || slot.Metadata.Width != Width ||
		    slot.Metadata.Height != Height || slot.Metadata.PixelFormat != Format || (Remote && !slot.Ready))
		{
			throw std::logic_error("Cloud picture publication generation/grid/token mismatch");
		}
	}
	void Drain()
	{
		for (auto& slot : Slots)
		{
			for (auto reader : slot.Readers)
			{
				if (reader)
				{
					Submit->Wait(reader);
				}
			}
		}
		if (Producer)
		{
			Producer->Drain();
		}
	}
};

CloudPictureTexture::CloudPictureTexture(GDX12DeviceResources& primary, UINT width, UINT height,
                                         std::shared_ptr<QueueSubmission> submit, bool remote)
    : _impl(std::make_unique<Impl>(primary, width, height, std::move(submit), remote))
{
}
CloudPictureTexture::~CloudPictureTexture()
{
	try
	{
		_impl->Drain();
	}
	catch (...)
	{
		OutputDebugStringA("Cloud picture drain failed; retaining the in-flight generation\n");
		_impl.release();
	}
}
GDX12Texture* CloudPictureTexture::Texture(UINT frameSlot) const
{
	return _impl->Slots.at(frameSlot).Display.get();
}
void CloudPictureTexture::Prepare(UINT frameSlot, double totalTime)
{
	auto& p = *_impl;
	if (!std::isfinite(totalTime) || totalTime < 0 || totalTime > (std::numeric_limits<float>::max)())
	{
		throw std::invalid_argument("Cloud picture requires finite nonnegative animation time");
	}
	auto& slot = p.Slots.at(frameSlot);
	for (auto reader : slot.Readers)
	{
		if (reader)
		{
			p.Submit->Wait(reader);
		}
	}
	slot.Readers = {};
	if (p.Sequence == UINT64_MAX)
	{
		throw std::overflow_error("Cloud picture sequence exhausted");
	}
	p.CurrentSlot = frameSlot;
	p.Work.Commands.clear();
	p.ImportOperation = Invalid;
	p.Parameters.TotalTime = float(totalTime);
	p.Parameters.CloudSpeed = 0.006f * SpeedScale.GetValue();
	slot.Metadata = {p.Generation, ++p.Sequence, p.Width, p.Height, Format};
	if (p.Sequence == 1 || p.Sequence == 61)
	{
		Log::Info("RenderGraph", "CloudPicture frame publication sequence=", p.Sequence, " renderTime=", totalTime,
		          " remote=", p.Remote);
	}
	if (!p.Remote)
	{
		return;
	}
	p.Producer->Begin(frameSlot);
	Graph graph;
	graph.Device = 1;
	graph.FrameBoundary = false;
	graph.TerminalQueue = Queue::Copy;
	graph.Resources = {{"cloud producer local UAV", 0, 0, true}, {"cloud shared export buffer", 0, 0, false, true}};
	graph.Operations = {
	    {"MGPU-Noise cloud picture producer", {{0, Mode::Write, UnorderedAccess}}},
	    {"cloud picture publication GPU export",
	     {{0, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE}, {1, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}}}};
	graph.Operations[1].Target = Queue::Copy;
	const auto schedule = Compile(graph);
	const auto parameters = p.Parameters;
	const auto uav = slot.Uav;
	auto* produced = slot.Produced.Get();
	auto* shared = slot.Transfer.Resources[1].Get();
	const auto footprint = p.Footprint;
	p.Producer->Reserve(schedule);
	p.Producer->Execute(schedule, {produced, shared},
	                    [&p, parameters, uav, produced, shared, footprint](Id op, uint32_t, auto* cmd)
	                    {
		                    if (op == 0)
		                    {
			                    p.Dispatch(cmd, parameters, uav);
		                    }
		                    else
		                    {
			                    const CD3DX12_TEXTURE_COPY_LOCATION source(produced, 0u), target(shared, footprint);
			                    cmd->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
		                    }
	                    });
	slot.Ready = p.Producer->Completions().back();
	p.CheckPublication(slot);
	if (p.Sequence == 1)
	{
		Log::Info("RenderGraph", "CloudPicture publication generation=", p.Generation, " sequence=", p.Sequence,
		          " issued secondary token=", slot.Ready.Value, " snapshotTime=", totalTime);
	}
}
void CloudPictureTexture::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings)
{
	auto& p = *_impl;
	auto& slot = p.Slots.at(p.CurrentSlot);
	p.CheckPublication(slot);
	const Id output = NativeWork::ResourceFor(graph, bindings, slot.Display->GetResource()->D3DResource.Get(),
	                                          "cloud picture material texture");
	if (!p.Remote)
	{
		const auto parameters = p.Parameters;
		const auto uav = slot.Uav;
		p.Work.Add(graph, "MGPU-Noise cloud picture local producer", {{output, Mode::Write, UnorderedAccess}},
		           [&p, parameters, uav](auto* cmd) { p.Dispatch(cmd, parameters, uav); }, {},
		           AsyncCloudPicture.GetValue() ? Queue::Compute : Queue::Direct);
	}
	else
	{
		const Id input =
		    NativeWork::ResourceFor(graph, bindings, slot.Transfer.Resources[0].Get(), "cloud picture shared import buffer");
		auto* shared = slot.Transfer.Resources[0].Get();
		auto* display = slot.Display->GetResource()->D3DResource.Get();
		const auto footprint = p.Footprint;
		p.ImportOperation = p.Work.Add(graph, "cloud picture strict publication primary import",
		                               {{input, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
		                                {output, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
		                               [shared, display, footprint](auto* cmd)
		                               {
			                               const CD3DX12_TEXTURE_COPY_LOCATION source(shared, footprint),
			                                   target(display, 0u);
			                               cmd->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
		                               });
		graph.Operations[p.ImportOperation].Target = Queue::Copy;
	}
}
bool CloudPictureTexture::Record(Id id, ID3D12GraphicsCommandList* cmd)
{
	return _impl->Work.Record(id, cmd);
}
std::vector<PacketWait> CloudPictureTexture::Waits(const Schedule& schedule) const
{
	const auto& p = *_impl;
	if (!p.Remote || p.ImportOperation == Invalid)
	{
		return {};
	}
	const auto& slot = p.Slots.at(p.CurrentSlot);
	p.CheckPublication(slot);
	return {{PacketFor(schedule, p.ImportOperation), slot.Ready}};
}
void CloudPictureTexture::RetireFrame(const Schedule&, const std::vector<CompletionToken>& completions, bool)
{
	auto& p = *_impl;
	auto& slot = p.Slots.at(p.CurrentSlot);
	for (const auto token : completions)
	{
		if (token)
		{
			if (token.Device != 0 || token.Incarnation != p.Submit->Incarnation())
			{
				throw std::logic_error("Cloud picture reader belongs to another submission domain");
			}
			const size_t queue = token.Target == Queue::Direct ? 0 : token.Target == Queue::Compute ? 1 : 2;
			if (token.Value > slot.Readers[queue].Value)
			{
				slot.Readers[queue] = token;
			}
		}
	}
}
void CloudPictureTexture::Drain()
{
	_impl->Drain();
}
} // namespace BistroRender
