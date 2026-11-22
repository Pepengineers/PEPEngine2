#include "SceneTextureInspector.h"
#include "Common/ConsoleVariables.h"
#include "Common/RuntimePaths.h"
#include "Engine.Core/Logging/Logger.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.UI/UI_test.h"
#include <imgui/imgui.h>
#include <cctype>
#include <cmath>
#include <fstream>
#include <shellapi.h>
#include <unordered_map>
#include <unordered_set>

namespace BistroRender
{
namespace
{
AutoConsoleVariable<bool> Enabled(L"Render.Debug.SceneTextures", false, L"Show the scene texture inspector in F1.");
constexpr UINT Tile = 512;
constexpr UINT TilesAcross = 3;
constexpr UINT Capacity = 6;
constexpr DXGI_FORMAT PreviewFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
using View = SceneTextureInspector::View;

DXGI_FORMAT PreviewSRV(const D3D12_RESOURCE_DESC& desc)
{
	if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
	    desc.SampleDesc.Count != 1 || (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
		return DXGI_FORMAT_UNKNOWN;
	switch (desc.Format)
	{
	case DXGI_FORMAT_R32_TYPELESS:
		return DXGI_FORMAT_R32_FLOAT;
	case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_R16_FLOAT:
	case DXGI_FORMAT_R8_UNORM:
	case DXGI_FORMAT_R16G16_FLOAT:
	case DXGI_FORMAT_R32G32_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
	case DXGI_FORMAT_R11G11B10_FLOAT:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		return desc.Format;
	default:
		return DXGI_FORMAT_UNKNOWN;
	}
}
View DefaultView(const D3D12_RESOURCE_DESC& desc, DXGI_FORMAT format)
{
	if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
		return View::Depth;
	if (format == DXGI_FORMAT_R32_FLOAT || format == DXGI_FORMAT_R16_FLOAT || format == DXGI_FORMAT_R8_UNORM)
		return View::Scalar;
	if (format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R11G11B10_FLOAT)
		return View::HDR;
	return View::Color;
}
}

SceneTextureInspector::SceneTextureInspector(UINT slots) : _slots(slots)
{
	int count = 0;
	auto* args = CommandLineToArgvW(GetCommandLineW(), &count);
	if (!args) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
	const std::unique_ptr<void, decltype(&LocalFree)> arguments(args, LocalFree);
	constexpr std::wstring_view prefix = L"--read-texture=";
	for (int i = 1; i < count; ++i)
	{
		const std::wstring_view arg(args[i]);
		if (arg.substr(0, prefix.size()) == prefix)
		{
			_readbackName = WStringToString(std::wstring(arg.substr(prefix.size())));
			if (_readbackName.empty()) throw std::invalid_argument("--read-texture expects a texture name");
		}
	}
}

void SceneTextureInspector::Name(const char* name, GDX12Texture* texture, View view)
{
	if (!texture || !texture->IsInitialized())
		return;
	auto* resource = texture->GetResource()->D3DResource.Get();
	const auto format = PreviewSRV(resource->GetDesc());
	if (format != DXGI_FORMAT_UNKNOWN || NeedsReadback())
		_named.push_back({name, resource, format, view});
}

std::vector<SceneTextureInspector::Source> SceneTextureInspector::Collect(Graph& graph,
    std::vector<ID3D12Resource*>& bindings, ID3D12Resource* backBuffer, bool raw)
{
	auto catalog = _named;
	if (!raw)
		catalog.erase(std::remove_if(catalog.begin(), catalog.end(),
		    [](const Source& item) { return item.Format == DXGI_FORMAT_UNKNOWN; }), catalog.end());
	const auto namedCount = catalog.size();
	std::vector<const Operation*> writers(bindings.size(), nullptr);
	for (const auto& pass : graph.Operations)
		for (const auto access : pass.Accesses)
			if (access.Usage != Mode::Read)
				writers.at(access.Resource) = &pass;
	std::unordered_map<std::string, UINT> ordinals;
	for (Id id = 0; id < bindings.size(); ++id)
	{
		auto* resource = bindings[id];
		const auto alias = std::find_if(_named.begin(), _named.end(),
		                               [resource](const auto& source) { return source.Resource == resource; });
		if (alias != _named.end())
		{
			graph.Resources[id].Name = alias->Name;
			continue;
		}
		if ((!writers[id] && !raw) || resource == backBuffer || graph.Resources[id].Memory != Heap::Default)
			continue;
		const auto desc = resource->GetDesc();
		const auto format = PreviewSRV(desc);
		if (raw ? desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER : format == DXGI_FORMAT_UNKNOWN)
			continue;
		std::string name = writers[id] ? writers[id]->Name + " / " + graph.Resources[id].Name
		                               : graph.Resources[id].Name;
		if (format == DXGI_FORMAT_UNKNOWN) name = "raw / " + name;
		name += " #" + std::to_string(++ordinals[name]);
		catalog.push_back({std::move(name), resource, format, DefaultView(desc, format)});
	}
	std::sort(catalog.begin() + namedCount, catalog.end(),
	          [](const auto& a, const auto& b) { return a.Name < b.Name; });
	return catalog;
}

void SceneTextureInspector::AppendReadback(const Input& input, UINT slot, bool finalAttempt)
{
	if (!NeedsReadback() || _readback.Buffer.Get()) return;
	const auto& [inspector, graph, bindings, work, device, backBuffer] = input;
	const auto catalog = inspector.Collect(graph, bindings, backBuffer, true);
	const auto found = std::find_if(catalog.begin(), catalog.end(),
	    [&](const Source& source) { return source.Name == _readbackName; });
	if (found == catalog.end() || graph.FindBinding(bindings, found->Resource) == Invalid)
	{
		if (finalAttempt && !_loggedMissing)
		{
			Log::Warning("RenderGraph", "Scene texture readback waiting for ", _readbackName,
			    " to appear in the active render graphs");
			_loggedMissing = true;
		}
		return;
	}

	auto* image = found->Resource;
	if (std::any_of(std::next(found), catalog.end(), [&](const Source& source) {
		    return source.Name == _readbackName && source.Resource != image;
	    }))
	{
		Log::Warning("RenderGraph", "Ambiguous scene texture readback name: ", _readbackName);
		_readbackName.clear();
		return;
	}
	const auto desc = image->GetDesc();
	if (desc.SampleDesc.Count != 1)
	{
		Log::Warning("RenderGraph", "Scene texture readback requires a single-sample resource: ", _readbackName);
		_readbackName.clear();
		return;
	}
	auto& capture = _readback;
	capture.Slot = slot;
	capture.Owner = graph.Device;
	device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &capture.Layout, &capture.Rows,
	                                          &capture.RowBytes, &capture.Bytes);
	if (!capture.Bytes || capture.Bytes == UINT64_MAX)
		throw std::runtime_error("Scene texture readback has an invalid copy footprint");
	capture.Buffer = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(capture.Bytes),
	                                      D3D12_RESOURCE_STATE_COPY_DEST);
	auto* buffer = capture.Buffer.Get();
	const Id source = NativeWork::ResourceFor(graph, bindings, image, _readbackName);
	const Id target = NativeWork::ResourceFor(graph, bindings, buffer, "scene texture readback buffer");
	work.Add(graph, "read back scene texture", {{source, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
	    {target, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
	    [=, layout = capture.Layout](auto* cmd)
	    {
		    const CD3DX12_TEXTURE_COPY_LOCATION src(image, 0u), dst(buffer, layout);
		    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	    }, {}, Queue::Copy);
}

void SceneTextureInspector::ReadbackSubmitted(Id owner, UINT slot)
{
	if (_readback.Owner == owner && _readback.Slot == slot) _readback.Submitted = true;
}

void SceneTextureInspector::RetireReadback(Id owner, UINT slot)
{
	if (!_readback.Buffer.Get() || _readback.Owner != owner || _readback.Slot != slot) return;
	auto capture = std::exchange(_readback, {});
	if (!capture.Submitted) return;
	const auto name = std::move(_readbackName);
	_readbackName.clear();
	const auto path = Engine::Core::GetExecutableDirectory() / "Logs" / "SceneTexture.bin";
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output)
	{
		Log::Warning("RenderGraph", "Cannot open scene texture readback file ", path.string());
		return;
	}
	const auto& shape = capture.Layout.Footprint;
	const D3D12_RANGE read{0, SIZE_T(capture.Bytes)};
	void* memory = nullptr;
	ThrowIfFailed(capture.Buffer.Get()->Map(0, &read, &memory));
	const auto* data = static_cast<const char*>(memory) + capture.Layout.Offset;

	float minimum = (std::numeric_limits<float>::max)(), maximum = -minimum;
	uint64_t belowOne = 0, nonFinite = 0, outsideUnitRange = 0;
	for (UINT z = 0; z < shape.Depth; ++z)
	{
		for (UINT y = 0; y < capture.Rows; ++y)
		{
			const auto* row = data + (uint64_t(z) * capture.Rows + y) * shape.RowPitch;
			output.write(row, std::streamsize(capture.RowBytes));
			if (shape.Format == DXGI_FORMAT_R32_FLOAT)
			{
				const auto* samples = reinterpret_cast<const float*>(row);
				for (UINT x = 0; x < shape.Width; ++x)
				{
					const float value = samples[x];
					if (!std::isfinite(value)) { ++nonFinite; continue; }
					minimum = (std::min)(minimum, value);
					maximum = (std::max)(maximum, value);
					if (value < .999f) ++belowOne;
					if (value < 0.f || value > 1.f) ++outsideUnitRange;
				}
			}
		}
	}
	const D3D12_RANGE written{0, 0};
	capture.Buffer.Get()->Unmap(0, &written);
	output.close();
	if (!output)
	{
		Log::Warning("RenderGraph", "Cannot write scene texture readback file ", path.string());
		return;
	}
	Log::Info("RenderGraph", "Scene texture readback name=", name, " adapter=", owner, " slot=", slot,
	    " format=", int(shape.Format), " size=", shape.Width, "x", shape.Height, "x", shape.Depth,
	    " rowBytes=", capture.RowBytes, " rows=", capture.Rows, " path=", path.string());
	if (shape.Format == DXGI_FORMAT_R32_FLOAT)
		Log::Info("RenderGraph", "Scene texture samples name=", name, " min=", minimum, " max=", maximum,
		    " below0.999=", belowOne, " nonFinite=", nonFinite, " outsideUnitRange=", outsideUnitRange);
}

SceneTextureInspector::Atlases SceneTextureInspector::Append(const Input& scene, const Input* post,
    UINT index, float nearPlane, float farPlane)
{
	if (!Enabled.GetValue() || !Engine::UI::DebugUIVisible())
		return {};
	auto catalog = Collect(scene.Plan, scene.Bindings, scene.BackBuffer);
	if (post)
	{
		auto postCatalog = post->Inspector.Collect(post->Plan, post->Bindings, post->BackBuffer);
		for (auto& source : postCatalog)
		{
			if (std::none_of(catalog.begin(), catalog.end(), [&](const auto& item) { return item.Name == source.Name; }))
			{
				source.Atlas = 1;
				catalog.push_back(std::move(source));
			}
		}
	}
	bool open = true;
	std::array<std::vector<Source>, 2> previews;
	const auto* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowSize({viewport->WorkSize.x * .88f, viewport->WorkSize.y * .85f}, ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_FirstUseEver, {.5f, .5f});
	if (ImGui::Begin("Scene Textures", &open))
	{
		std::unordered_set<ID3D12Resource*> listed;
		catalog.erase(std::remove_if(catalog.begin(), catalog.end(),
		    [&](const Source& source) { return !listed.insert(source.Resource).second; }), catalog.end());

		if (ImGui::BeginTable("inspector", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
		{
			ImGui::TableSetupColumn("Textures", ImGuiTableColumnFlags_WidthFixed, viewport->WorkSize.x * .22f);
			ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableNextColumn();
			ImGui::BeginChild("browser");
			ImGui::SetNextItemWidth(-1);
			ImGui::InputTextWithHint("##filter", "Find a texture by name", _filter.data(), _filter.size());
			if (ImGui::Button("Depth")) _selected = {"Scene.Depth"};
			ImGui::SameLine();
			if (ImGui::Button("AO")) _selected = {"Scene.AO"};
			ImGui::SameLine();
			if (ImGui::Button("G-buffer"))
				_selected = {"GBuffer.AlbedoMetallic", "GBuffer.NormalRoughness", "GBuffer.Motion",
				             "GBuffer.F0Baked", "GBuffer.Emissive", "Scene.Depth"};
			ImGui::SameLine();
			if (ImGui::Button("Clear")) _selected.clear();

			ImGui::TextDisabled("%zu / %u selected", _selected.size(), Capacity);
			ImGui::BeginChild("catalog", {}, 0, ImGuiWindowFlags_HorizontalScrollbar);
			const std::string_view filter(_filter.data());
			for (const auto& source : catalog)
			{
				const auto match = std::search(source.Name.begin(), source.Name.end(), filter.begin(), filter.end(),
				    [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
				if (!filter.empty() && match == source.Name.end())
					continue;
				const auto selected = std::find(_selected.begin(), _selected.end(), source.Name);
				bool checked = selected != _selected.end();
				ImGui::BeginDisabled(!checked && _selected.size() == Capacity);
				if (ImGui::Checkbox(source.Name.c_str(), &checked))
				{
					if (checked) _selected.push_back(source.Name);
					else _selected.erase(selected);
				}
				ImGui::EndDisabled();
			}
			ImGui::EndChild();

			ImGui::EndChild();
			ImGui::TableNextColumn();
			ImGui::BeginChild("viewer");
			ImGui::SetNextItemWidth(100);
			ImGui::Combo("Channel", &_channel, "Default\0RGB\0Red\0Green\0Blue\0Alpha\0");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(110);
			ImGui::SliderFloat("Depth range", &_depthRange, 100, 20000, "%.0f cm", ImGuiSliderFlags_Logarithmic);
			const int count = static_cast<int>(_selected.size());
			if (count > 1)
			{
				ImGui::SetNextItemWidth(100);
				ImGui::SliderInt("Columns", &_columns, 1, 3);
			}
			if (!count) ImGui::TextDisabled("Select a texture to preview.");
			const float availableHeight = ImGui::GetContentRegionAvail().y;
			if (count && ImGui::BeginTable("previews", (std::min)(_columns, count), ImGuiTableFlags_SizingStretchSame))
			{
				for (const auto& name : _selected)
				{
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(name.c_str());
					const auto source = std::find_if(catalog.begin(), catalog.end(),
					                                 [&name](const auto& item) { return item.Name == name; });
					if (source == catalog.end())
					{
						ImGui::TextDisabled("Unavailable this frame");
						continue;
					}
					const auto desc = source->Resource->GetDesc();
					ImGui::TextDisabled("%llu x %u", static_cast<unsigned long long>(desc.Width), desc.Height);
					float width = (std::max)(1.f, ImGui::GetContentRegionAvail().x);
					if (count == 1)
						width = (std::min)(width, (std::max)(1.f, availableHeight - 2 * ImGui::GetTextLineHeightWithSpacing() - 8) * float(desc.Width) / float(desc.Height));
					const float height = width * float(desc.Height) / float(desc.Width);
					auto& atlas = previews[source->Atlas];
					const UINT tile = static_cast<UINT>(atlas.size());
					const float x = float(tile % TilesAcross);
					const float y = float(tile / TilesAcross);
					ImGui::Image(static_cast<ImTextureID>(AtlasId(source->Atlas)), {width, height},
					             {(x + 0.5f / Tile) / TilesAcross, (y + 0.5f / Tile) / 2},
					             {(x + 1 - 0.5f / Tile) / TilesAcross, (y + 1 - 0.5f / Tile) / 2});
					atlas.push_back(*source);
				}
				ImGui::EndTable();
			}
			ImGui::EndChild();
			ImGui::EndTable();
		}
	}
	ImGui::End();
	if (!open) Enabled.SetValue(false);
	return {AppendAtlas(scene, index, previews[0], nearPlane, farPlane, _channel, _depthRange),
	        post ? post->Inspector.AppendAtlas(*post, index, previews[1], nearPlane, farPlane, _channel, _depthRange) : nullptr};
}

GDX12Texture* SceneTextureInspector::AppendAtlas(const Input& input, UINT index, const std::vector<Source>& previews,
    float nearPlane, float farPlane, int channel, float depthRange)
{
	if (previews.empty())
		return nullptr;
	const auto& [_, graph, bindings, work, device, backBuffer] = input;

	if (!_pipeline)
	{
		GDX12RootSignatureDesc desc;
		desc.Constants = {7};
		desc.NumSingleSRVSlots = 1;
		desc.NumSingleUAVSlots = 1;
		_root = std::make_unique<GDX12RootSignature>(&device, desc);
		_pipeline = GDX12RenderHelpers::CreateComputePipeline(&device, _root.get(),
		                                                      APP_SHADERS_FOLDER "SceneTextureInspector.hlsl", "CS");
	}
	auto& slot = _slots.at(index);
	if (!slot.Heap)
	{
		slot.Heap = std::make_unique<GDX12DescriptorHeap>(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
		    Capacity + 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
		GDX12TextureDesc desc;
		desc.Width = Tile * TilesAcross;
		desc.Height = Tile * 2;
		desc.Format = PreviewFormat;
		desc.CreateSRV = false;
		desc.CreateUAV = true;
		desc.UAVDesc.Format = PreviewFormat;
		desc.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		desc.SRV_UAV_Heap = slot.Heap.get();
		desc.UAVHeapIndex = slot.Heap->GetAvailableIndex();
		slot.Atlas.Initialize(desc);
	}
	const Id atlas = NativeWork::ResourceFor(graph, bindings, slot.Atlas.GetResource()->D3DResource.Get(), "Debug.SceneTextures");
	std::vector<Access> accesses{{atlas, Mode::Write, UnorderedAccess}};
	std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> views;
	std::vector<UINT> modes;
	for (const auto& source : previews)
	{
		const Id id = NativeWork::ResourceFor(graph, bindings, source.Resource, source.Name);
		if (std::none_of(accesses.begin(), accesses.end(), [id](auto access) { return access.Resource == id; }))
			accesses.push_back({id, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
		D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
		desc.Format = source.Format;
		desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		desc.Texture2D.MipLevels = 1;
		const UINT descriptor = static_cast<UINT>(views.size()) + 1;
		device.GetDevice()->CreateShaderResourceView(source.Resource, &desc, slot.Heap->GetCPUHandle(descriptor));
		views.push_back(slot.Heap->GetGPUHandle(descriptor));
		modes.push_back(channel == 0 ? UINT(source.Visualization) : channel == 1 ? 0 : UINT(channel + 4));
	}
	struct Constants { UINT X, Y, Size, Mode; float Near, Far, Range; };
	static_assert(sizeof(Constants) == 7 * sizeof(UINT));
	auto* heap = slot.Heap->GetHeap().Get();
	const auto uav = slot.Atlas.GetUAV()->GPUHandle;
	work.Add(graph, "debug scene texture previews", std::move(accesses),
	         [this, heap, uav, views = std::move(views), modes = std::move(modes), nearPlane, farPlane, depthRange](auto* cmd)
	         {
		         cmd->SetDescriptorHeaps(1, &heap);
		         cmd->SetComputeRootSignature(_root->GetRootSignature().Get());
		         cmd->SetPipelineState(_pipeline.Get());
		         cmd->SetComputeRootDescriptorTable(_root->GetURootParamIndex(0), uav);
		         for (UINT i = 0; i < views.size(); ++i)
		         {
			         const Constants constants{i % TilesAcross * Tile, i / TilesAcross * Tile, Tile,
			                                   modes[i], nearPlane, farPlane, depthRange};
			         cmd->SetComputeRoot32BitConstants(0, 7, &constants, 0);
			         cmd->SetComputeRootDescriptorTable(_root->GetTRootParamIndex(0), views[i]);
			         cmd->Dispatch(Tile / 8, Tile / 8, 1);
		         }
	         });
	return &slot.Atlas;
}
}
