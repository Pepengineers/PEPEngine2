#pragma once
#include "Techniques/NativeWork.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include "Engine.RendererDX12/GDX12Texture.h"
#include "Engine.UI/OffscreenUI.h"
#include <array>

namespace BistroRender
{
class SceneTextureInspector
{
public:
	enum class View : UINT { Color, Scalar, Depth, Normals, Motion, HDR };

	explicit SceneTextureInspector(UINT slots);
	bool NeedsReadback() const { return !_readbackName.empty(); }
	bool NeedsReadback(std::string_view name) const { return _readbackName == name; }
	struct Input
	{
		SceneTextureInspector& Inspector;
		Graph& Plan;
		std::vector<ID3D12Resource*>& Bindings;
		NativeWork& Work;
		GDX12Device& Device;
		ID3D12Resource* BackBuffer;
	};
	void AppendReadback(const Input&, UINT slot, bool finalAttempt = true);
	void ReadbackSubmitted(Id owner, UINT slot);
	void RetireReadback(Id owner, UINT slot);
	void BeginPlan() { _named.clear(); }
	void Name(const char* name, GDX12Texture* texture, View view = View::Color);
	using Atlases = std::array<GDX12Texture*, 2>;
	Atlases Append(const Input&, const Input* post, UINT slot, float nearPlane, float farPlane);
	static constexpr auto AtlasId(UINT index) { return Engine::UI::UITextureId(0x424953544558494eull + index); }

private:
	struct Source
	{
		std::string Name;
		ID3D12Resource* Resource;
		DXGI_FORMAT Format;
		View Visualization;
		UINT Atlas = 0;
	};
	struct Slot
	{
		std::unique_ptr<GDX12DescriptorHeap> Heap;
		GDX12Texture Atlas;
	};
	struct Readback
	{
		GDX12ResourceHandle Buffer;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout{};
		UINT Rows = 0, Slot = 0;
		UINT64 RowBytes = 0, Bytes = 0;
		Id Owner = Invalid;
		bool Submitted = false;
	};
	std::vector<Source> Collect(Graph&, std::vector<ID3D12Resource*>&, ID3D12Resource* backBuffer, bool raw = false);
	GDX12Texture* AppendAtlas(const Input&, UINT slot, const std::vector<Source>&, float nearPlane,
	                         float farPlane, int channel, float depthRange);
	std::string _readbackName;
	Readback _readback;
	bool _loggedMissing = false;
	std::vector<Source> _named;
	std::vector<std::string> _selected{"Scene.Depth", "Scene.AO"};
	std::vector<Slot> _slots;
	std::unique_ptr<GDX12RootSignature> _root;
	ComPtr<ID3D12PipelineState> _pipeline;
	std::array<char, 160> _filter{};
	int _columns = 2;
	int _channel = 0;
	float _depthRange = 1000;
};
}
