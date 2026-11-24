#pragma once
#include "SceneLocalShadows.h"
#include "LocalShadowTileCopy.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"

namespace BistroRender
{
struct PublishedShadows
{
	PublishedShadows(SceneLocalShadows& pass, UINT slot, UINT lightCount)
	    : Texture(pass.Output(slot)), Lights(pass.LightSource(slot)), Source(pass), Slot(slot), LightCount(lightCount),
	      Enabled(pass.HasAtlas(slot)) {}
	GDX12Texture* Texture;
	GDX12UploadBuffer<GDX12LocalLightConstants>& Lights;
	const SceneLocalShadows& Source;
	UINT Slot, LightCount;
	bool Enabled;
};

// The caller retires the consumer's previous submission before preparing another capture or frame.
class PublishedShadowMirror
{
	struct Face
	{
		UINT Light, Index;
		GDX12LocalShadowTile Tile;
	};
	using Tiles = std::map<UINT, std::vector<Face>>;

	static Tiles ReadyTiles(const PublishedShadows& source)
	{
		Tiles tiles;
		for (UINT i = 0; i < source.LightCount; ++i)
		{
			const auto& light = source.Lights.GetCPUData()[i];
			if (light.ShadowFirstSlice == UINT_MAX) continue;
			for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
				if (light._padding[2] & (1u << face))
				{
					const auto& tile = light.ShadowTiles[face];
					tiles[tile.Side].push_back({i, face, tile});
				}
		}
		return tiles;
	}

	static LocalShadowTileTransfer::Bytes TransferCost(GDX12Device& primary, GDX12Device& owner, const Tiles& tiles)
	{
		LocalShadowTileTransfer::Bytes cost;
		for (const auto& [side, faces] : tiles)
			for (size_t first = 0; first < faces.size(); first += MaxLocalShadowAtlasCount)
				cost += LocalShadowTileTransfer::Cost(primary, owner, side,
				    UINT((std::min)(faces.size() - first, size_t(MaxLocalShadowAtlasCount))), true);
		return cost;
	}

public:
	static bool CanRecreate(const QueueSubmission& submit, Id owner,
	                        std::initializer_list<const PublishedShadows*> sources)
	{
		auto& primary = submit.Device(0);
		auto& device = submit.Device(owner);
		LocalShadowTileTransfer::Bytes cost;
		for (const auto* source : sources)
		{
			if (!source || !source->Enabled) continue;
			const auto shape = source->Texture->GetResource()->D3DResource->GetDesc();
			cost.Owner += ShadowMemory::Size(device, shape);
			cost += TransferCost(primary, device, ReadyTiles(*source));
		}
		return !cost.Owner || (ShadowMemory::Fits(primary, cost.Primary) && ShadowMemory::Fits(device, cost.Owner));
	}

	void Clear()
	{
		_transfers.clear();
		_tiles.clear();
		_versions.clear();
		_dsvs.clear();
		_depth.reset();
		_memory.Reset();
	}

	bool Prepare(const QueueSubmission& submit, Id owner, GDX12DescriptorHeap& heap, const PublishedShadows& source)
	{
		_transfers.clear();
		if (!source.Enabled)
		{
			Clear();
			return true;
		}
		auto& primary = submit.Device(0);
		auto& device = submit.Device(owner);
		const auto shape = source.Texture->GetResource()->D3DResource->GetDesc();
		if (_depth && (_depth->GetWidth() != shape.Width || _dsvs.size() != shape.DepthOrArraySize)) Clear();
		_tiles = ReadyTiles(source);
		auto cost = TransferCost(primary, device, _tiles);
		if (!_depth) cost.Owner += ShadowMemory::Size(device, shape);
		if (!ShadowMemory::Fits(primary, cost.Primary) || !ShadowMemory::Fits(device, cost.Owner))
		{
			Clear();
			return false;
		}
		_submit = &submit;
		_owner = owner;
		if (!_depth)
		{
			GDX12TextureDesc texture{};
			texture.Width = texture.Height = UINT(shape.Width);
			texture.Format = DXGI_FORMAT_D32_FLOAT;
			texture.SRVDesc = ShadowMemory::View(shape.DepthOrArraySize);
			texture.SRV_UAV_Heap = &heap;
			texture.ExternalResource = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, shape, D3D12_RESOURCE_STATE_COMMON);
			texture.SRVHeapIndex = heap.GetAvailableIndex(SceneDescriptorStart);
			_depth = std::make_unique<GDX12Texture>();
			_depth->Initialize(texture);
			_memory.Set(device, ShadowMemory::Size(device, shape));
			if (!_dsvHeap)
				_dsvHeap = std::make_unique<GDX12DescriptorHeap>(&device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
				    MaxLocalShadowAtlasCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
			for (UINT page = 0; page < shape.DepthOrArraySize; ++page)
			{
				auto dsv = std::make_unique<GDX12Descriptor>();
				D3D12_DEPTH_STENCIL_VIEW_DESC view{};
				view.Format = DXGI_FORMAT_D32_FLOAT;
				view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
				view.Texture2DArray.FirstArraySlice = page;
				view.Texture2DArray.ArraySize = 1;
				dsv->InitAsDSV(_depth->GetResource()->D3DResource.Get(), &view, _dsvHeap.get(), _dsvHeap->GetAvailableIndex());
				_dsvs.push_back(std::move(dsv));
			}
		}
		return true;
	}

	UINT64 Append(Graph& primary, std::vector<ID3D12Resource*>& primaryBindings, NativeWork& primaryWork,
	              Graph& remote, std::vector<ID3D12Resource*>& remoteBindings, NativeWork& remoteWork,
	              const PublishedShadows& source, std::vector<Id>& exports, std::vector<Id>& imports, std::vector<Access>& reads)
	{
		if (!_depth) return 0;
		auto* from = source.Texture->GetResource()->D3DResource.Get();
		auto* target = _depth->GetResource()->D3DResource.Get();
		UINT64 bytes = 0;
		decltype(_versions) versions;
		for (const auto& [side, faces] : _tiles)
		{
			std::vector<GDX12LocalShadowTile> dirty;
			for (const auto& face : faces)
			{
				const auto version = source.Source.ContentVersion(source.Slot, face.Light, face.Index);
				const std::array key{face.Tile.Page, face.Tile.X, face.Tile.Y, side};
				versions.emplace(key, version);
				const auto found = _versions.find(key);
				if (!version || found == _versions.end() || found->second != version) dirty.push_back(face.Tile);
			}
			bytes += UINT64(dirty.size()) * side * side * sizeof(float);
			for (size_t first = 0; first < dirty.size(); first += MaxLocalShadowAtlasCount)
			{
				if (!_pack) _pack = std::make_unique<LocalShadowTileCopy>(_submit->Device(0));
				if (!_draw) _draw = std::make_unique<LocalShadowTileCopy>(_submit->Device(_owner));
				const UINT count = UINT((std::min)(dirty.size() - first, size_t(MaxLocalShadowAtlasCount)));
				auto transfer = std::make_unique<LocalShadowTileTransfer>(*_submit, _owner, side, count, true);
				exports.push_back(transfer->Export(primary, primaryBindings, primaryWork, *_pack, from,
				    {dirty.begin() + first, dirty.begin() + first + count}));
				imports.push_back(transfer->Stage(remote, remoteBindings, remoteWork));
				std::vector<LocalShadowTileCopy::Region> regions;
				for (UINT i = 0; i < count; ++i)
				{
					const auto& tile = dirty[first + i];
					regions.push_back({transfer->PackedTile(i), tile, _dsvs[tile.Page]->CPUHandle});
				}
				transfer->Publish(remote, remoteBindings, remoteWork, *_draw, target, std::move(regions));
				_transfers.push_back(std::move(transfer));
			}
		}
		_versions.swap(versions);
		const auto id = NativeWork::ResourceFor(remote, remoteBindings, target, "published local shadow atlas");
		reads.push_back({id, Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE});
		return bytes;
	}

	GDX12Texture* Fallback(GDX12Texture& empty) { return _depth ? _depth.get() : &empty; }
	D3D12_GPU_DESCRIPTOR_HANDLE Fallback(D3D12_GPU_DESCRIPTOR_HANDLE empty)
	{ return _depth ? _depth->GetSRV()->GPUHandle : empty; }

private:
	const QueueSubmission* _submit = nullptr;
	Id _owner = 0;
	ShadowMemory::Charge _memory;
	std::unique_ptr<GDX12Texture> _depth;
	std::unique_ptr<GDX12DescriptorHeap> _dsvHeap;
	std::vector<std::unique_ptr<GDX12Descriptor>> _dsvs;
	std::unique_ptr<LocalShadowTileCopy> _pack, _draw;
	Tiles _tiles;
	std::map<std::array<UINT, 4>, uint64_t> _versions;
	std::vector<std::unique_ptr<LocalShadowTileTransfer>> _transfers;
};
}
