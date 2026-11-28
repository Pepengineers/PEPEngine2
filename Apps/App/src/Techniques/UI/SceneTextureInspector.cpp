#include "SceneTextureInspector.h"
#include "BistroPlot.h"
#include "Common/DebugConsoleVariable.h"
#include "Common/RuntimePaths.h"
#include "Engine.Core/Logging/Logger.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.UI/UI_test.h"
#include "Engine.UI/PepengineTheme.h"
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
bool InspectorFocused = false;
DebugConsoleVariable<bool> Enabled(L"Render.Debug.SceneTextures", false, L"Show the scene texture inspector (F8).");
constexpr UINT Tile = 512;
constexpr UINT TilesAcross = 3;
constexpr UINT Capacity = 6;
constexpr DXGI_FORMAT PreviewFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
using View = SceneTextureInspector::View;
namespace Theme = Engine::UI::PepengineTheme;

const char* PreviewFormatName(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
	case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT";
	case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
	case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
	case DXGI_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT";
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
	case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
	case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
	case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
	case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
	default: return nullptr;
	}
}
DXGI_FORMAT PreviewSRV(const D3D12_RESOURCE_DESC& desc)
{
	if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
	    desc.SampleDesc.Count != 1 || (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
		return DXGI_FORMAT_UNKNOWN;
	const auto format = desc.Format == DXGI_FORMAT_R32_TYPELESS ? DXGI_FORMAT_R32_FLOAT : desc.Format;
	return PreviewFormatName(format) ? format : DXGI_FORMAT_UNKNOWN;
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

bool SceneTextureInspector::Visible() { return Enabled.GetValue(); }
bool SceneTextureInspector::Focused() { return Visible() && InspectorFocused; }

void SceneTextureInspector::SetVisible(bool visible)
{
#if defined(_DEBUG) || defined(DEBUG)
	if (Visible() == visible) return;
	const auto error = Engine::UI::SetSceneTexturesVisible(visible);
	if (!error.empty()) Log::Warning("RenderGraph", "Cannot change scene texture inspector visibility: ", error);
#endif
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

bool SceneTextureInspector::RequestsTexture(std::string_view name) const
{
	return (NeedsReadback(name) && !_readback.Buffer.Get()) ||
	    (PreviewsVisible() && ImGui::GetCurrentContext() && std::find(_selected.begin(), _selected.end(), name) != _selected.end());
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
	InspectorFocused = false;
	if (!Visible() || !ImGui::GetCurrentContext())
		return {};
	bool open = true;
	std::array<std::vector<Source>, 2> previews;
	const auto* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowSize({viewport->WorkSize.x * .94f, viewport->WorkSize.y * .87f}, ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_FirstUseEver, {.5f, .5f});
	const float size = std::clamp(std::round(viewport->WorkSize.y / 36.f), 17.f, 32.f);
	ImGui::PushFont(Theme::BodyFont(), size);
	Theme::Push();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {.6f * size, .48f * size});
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {.42f * size, .25f * size});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {.48f * size, .28f * size});
	_collapsed = !ImGui::Begin("Scene textures   [F8]###Scene Textures", &open);
	InspectorFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	if (!_collapsed && open)
	{
		const float em = ImGui::GetFontSize();
		auto catalog = Collect(scene.Plan, scene.Bindings, scene.BackBuffer);
		if (post)
		{
			std::unordered_set<std::string> names;
			for (const auto& source : catalog) names.insert(source.Name);
			for (auto& source : post->Inspector.Collect(post->Plan, post->Bindings, post->BackBuffer))
			{
				if (!names.insert(source.Name).second) continue;
				source.Atlas = 1;
				catalog.push_back(std::move(source));
			}
		}
		std::unordered_set<ID3D12Resource*> listed;
		catalog.erase(std::remove_if(catalog.begin(), catalog.end(),
		    [&](const Source& source) { return !listed.insert(source.Resource).second; }), catalog.end());

		const auto area = ImGui::GetContentRegionAvail();
		const bool split = area.x >= 34.f * em;
		if (ImGui::BeginTable("inspector", split ? 2 : 1, split ? ImGuiTableFlags_Resizable : ImGuiTableFlags_None))
		{
			if (split) ImGui::TableSetupColumn("Textures", ImGuiTableColumnFlags_WidthFixed, area.x * .27f);
			ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableNextColumn();
			ImGui::BeginChild("browser", {0, split ? 0.f : (std::max)(7.f * em, area.y * .33f)}, ImGuiChildFlags_Borders);
			ImGui::SetNextItemWidth(-1);
			ImGui::InputTextWithHint("##filter", "Find a texture by name", _filter.data(), _filter.size());
			const auto presetButton = [](const char* label, bool first = false) {
				if (!first) BistroPlot::ContinueRowIfFits(ImGui::CalcTextSize(label).x + 2.f * ImGui::GetStyle().FramePadding.x);
				return ImGui::Button(label);
			};
			if (presetButton("Depth", true)) _selected = {"Scene.Depth"};
			if (presetButton("AO")) _selected = {"Scene.AO"};
			if (presetButton("G-buffer"))
				_selected = {"GBuffer.AlbedoMetallic", "GBuffer.NormalRoughness", "GBuffer.Motion",
				             "GBuffer.F0Baked", "GBuffer.Emissive", "Scene.Depth"};
			if (presetButton("Clear")) _selected.clear();

			ImGui::TextDisabled("%zu / %u selected", _selected.size(), Capacity);
			ImGui::BeginChild("catalog", {}, 0, ImGuiWindowFlags_HorizontalScrollbar);
			const std::string_view filter(_filter.data());
			bool matches = false;
			for (const auto& source : catalog)
			{
				const auto match = std::search(source.Name.begin(), source.Name.end(), filter.begin(), filter.end(),
				    [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
				if (!filter.empty() && match == source.Name.end())
					continue;
				matches = true;
				const auto selected = std::find(_selected.begin(), _selected.end(), source.Name);
				bool checked = selected != _selected.end();
				if (checked)
				{
					const auto row = ImGui::GetCursorScreenPos();
					ImGui::GetWindowDrawList()->AddRectFilled(row,
					    {row.x + ImGui::GetContentRegionAvail().x, row.y + ImGui::GetFrameHeight()},
					    ImGui::GetColorU32(ImGuiCol_Header), ImGui::GetStyle().FrameRounding);
				}
				ImGui::BeginDisabled(!checked && _selected.size() == Capacity);
				if (ImGui::Checkbox(source.Name.c_str(), &checked))
				{
					if (checked) _selected.push_back(source.Name);
					else _selected.erase(selected);
				}
				ImGui::EndDisabled();
			}
			if (!matches) ImGui::TextDisabled("No matching textures");
			ImGui::EndChild();

			ImGui::EndChild();
			ImGui::TableNextColumn();
			ImGui::BeginChild("viewer", {}, ImGuiChildFlags_Borders);
			const int count = static_cast<int>(_selected.size());
			const bool hasPreview = std::any_of(catalog.begin(), catalog.end(), [&](const Source& source) {
				return std::find(_selected.begin(), _selected.end(), source.Name) != _selected.end();
			});
			const bool hasDepth = std::any_of(catalog.begin(), catalog.end(), [&](const Source& source) {
				return source.Visualization == View::Depth && std::find(_selected.begin(), _selected.end(), source.Name) != _selected.end();
			});
			const bool showDepth = hasDepth && _channel == 0;
			const int controls = 1 + int(showDepth) + int(count > 1);
			const int toolbarColumns = (std::clamp)(int(ImGui::GetContentRegionAvail().x / (8.f * em)), 1, controls);
			if (hasPreview && ImGui::BeginTable("Preview controls", toolbarColumns, ImGuiTableFlags_SizingStretchSame))
			{
				const float fieldWidth = 6.5f * em;
				const float longestLabel = ImGui::CalcTextSize(showDepth ? "Depth range" : "Channel").x;
				const auto drawLabel = [&](const char* label) {
					ImGui::TableNextColumn();
					const bool inlineLabel = longestLabel + ImGui::GetStyle().ItemSpacing.x + fieldWidth <= ImGui::GetContentRegionAvail().x;
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted(label);
					if (inlineLabel) ImGui::SameLine();
					ImGui::SetNextItemWidth((std::clamp)(ImGui::GetContentRegionAvail().x, 1.f, fieldWidth));
				};
				drawLabel("Channel");
				ImGui::Combo("##channel", &_channel, "Default\0RGB\0Red\0Green\0Blue\0Alpha\0");
				if (showDepth)
				{
					drawLabel("Depth range");
					ImGui::SliderFloat("##depthRange", &_depthRange, 100, 20000, "%.0f cm", ImGuiSliderFlags_Logarithmic);
				}
				if (count > 1)
				{
					drawLabel("Columns");
					ImGui::SliderInt("##columns", &_columns, 1, 3);
				}
				ImGui::EndTable();
				ImGui::Separator();
			}
			if (!count) ImGui::TextDisabled("Select a texture to preview.");
			const float availableHeight = ImGui::GetContentRegionAvail().y;
			if (count && ImGui::BeginTable("previews", (std::min)(_columns, count),
			    ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV))
			{
				auto remove = _selected.end();
				for (auto current = _selected.begin(); current != _selected.end(); ++current)
				{
					const auto& name = *current;
					ImGui::TableNextColumn();
					ImGui::PushFont(Theme::HeadingFont(), 0.f);
					ImGui::TextWrapped("%s", name.c_str());
					ImGui::PopFont();
					const auto source = std::find_if(catalog.begin(), catalog.end(),
					                                 [&name](const auto& item) { return item.Name == name; });
					if (source == catalog.end())
					{
						ImGui::PushID(name.c_str());
						const float button = ImGui::GetFrameHeight();
						BistroPlot::ContinueRowIfFits(button);
						if (ImGui::Button("##remove unavailable", {button, button})) remove = current;
						const auto pos = ImGui::GetItemRectMin();
						const ImVec2 center{pos.x + .5f * button, pos.y + .5f * button};
						const float d = .15f * button;
						auto* draw = ImGui::GetWindowDrawList();
						const auto ink = ImGui::GetColorU32(ImGuiCol_CheckMark);
						draw->AddLine({center.x - d, center.y - d}, {center.x + d, center.y + d}, ink, .055f * button);
						draw->AddLine({center.x - d, center.y + d}, {center.x + d, center.y - d}, ink, .055f * button);
						if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) ImGui::SetTooltip("Remove texture");
						else ImGui::SetItemTooltip("Remove texture");
						ImGui::PopID();
						ImGui::TextDisabled("Unavailable this frame");
						continue;
					}
					const auto desc = source->Resource->GetDesc();
					float width = (std::max)(1.f, ImGui::GetContentRegionAvail().x);
					if (count == 1)
						width = (std::min)(width, (std::max)(1.f, availableHeight - 3 * ImGui::GetTextLineHeightWithSpacing()) * float(desc.Width) / float(desc.Height));
					const float height = width * float(desc.Height) / float(desc.Width);
					auto& atlas = previews[source->Atlas];
					const UINT tile = static_cast<UINT>(atlas.size());
					const float x = float(tile % TilesAcross);
					const float y = float(tile / TilesAcross);
					ImGui::Image(static_cast<ImTextureID>(AtlasId(source->Atlas)), {width, height},
					             {(x + 0.5f / Tile) / TilesAcross, (y + 0.5f / Tile) / 2},
					             {(x + 1 - 0.5f / Tile) / TilesAcross, (y + 1 - 0.5f / Tile) / 2});
					ImGui::TextWrapped(u8"%llu × %u    %s", static_cast<unsigned long long>(desc.Width), desc.Height,
					    PreviewFormatName(source->Format));
					atlas.push_back(*source);
				}
				if (remove != _selected.end()) _selected.erase(remove);
				ImGui::EndTable();
			}
			ImGui::EndChild();
			ImGui::EndTable();
		}
	}
	ImGui::End();
	ImGui::PopStyleVar(3);
	Theme::Pop();
	ImGui::PopFont();
	if (!open) SetVisible(false);
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
