#include "CloudPictureTexture.h"
#include "NativeWork.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.Core/Logging/Logger.h"
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
void CheckFormat(GDX12Device& device)
{
	D3D12_FEATURE_DATA_FORMAT_SUPPORT support{Format};
	ThrowIfFailed(device.GetDevice()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
	if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) ||
	    !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) ||
	    !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
	{
		throw std::runtime_error("Cloud picture requires RGBA8 UNORM texture sampling and typed UAV stores");
	}
}
} // namespace

struct CloudPictureTexture::Impl
{
	GDX12DeviceResources& Primary;
	std::unique_ptr<GDX12RootSignature> Root;
	ComPtr<ID3D12PipelineState> Pipeline;
	std::vector<std::unique_ptr<GDX12Texture>> Slots;
	NativeWork Work;
	CloudGeneratorData Parameters;
	uint64_t Sequence = 0;
	UINT Width, Height, CurrentSlot = 0;

	Impl(GDX12DeviceResources& primary, UINT width, UINT height)
	    : Primary(primary), Slots(primary.FrameConstants.size()), Width(width), Height(height)
	{
		if (!primary.Device || Slots.empty() || !width || !height || width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
		    height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
		{
			throw std::invalid_argument("Cloud picture grid must be nonzero and within D3D12 Texture2D limits");
		}
		auto& device = *primary.Device;
		CheckFormat(device);
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
			target.CreateUAV = true;
			target.UAVHeapIndex =
			    target.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
			target.UAVDesc.Format = Format;
			target.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			texture->Initialize(target);
		}
		Log::Info("RenderGraph", "CloudPicture effective=", width, "x", height, " format=RGBA8_UNORM slots=", Slots.size());
	}
	void Dispatch(ID3D12GraphicsCommandList* cmd, const CloudGeneratorData& parameters, D3D12_GPU_DESCRIPTOR_HANDLE uav)
	{
		auto* heap = Primary.SRV_UAV_Heap->GetHeap().Get();
		cmd->SetDescriptorHeaps(1, &heap);
		cmd->SetComputeRootSignature(Root->GetRootSignature().Get());
		cmd->SetPipelineState(Pipeline.Get());
		cmd->SetComputeRoot32BitConstants(Root->GetBRootParamIndex(0), sizeof(parameters) / sizeof(UINT), &parameters,
		                                  0);
		cmd->SetComputeRootDescriptorTable(Root->GetURootParamIndex(0), uav);
		cmd->Dispatch((Width + 31) / 32, (Height + 31) / 32, 1);
	}
};

CloudPictureTexture::CloudPictureTexture(GDX12DeviceResources& primary, UINT width, UINT height)
    : _impl(std::make_unique<Impl>(primary, width, height))
{
}
CloudPictureTexture::~CloudPictureTexture() = default;
GDX12Texture* CloudPictureTexture::Texture(UINT frameSlot) const
{
	return _impl->Slots.at(frameSlot).get();
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
} // namespace BistroRender
