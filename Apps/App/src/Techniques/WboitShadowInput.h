#pragma once

#include "SceneLighting.h"
#include "WboitTransfer.h"

namespace BistroRender
{
class WboitShadowInput
{
  public:
	UINT64 Bytes = 0, LocalBytes = 0;
	Id Export = Invalid, Import = Invalid;

	// The caller retires both queues before reusing this per-atlas storage.
	ID3D12Resource* Prepare(const QueueSubmission& submit, Id owner, Graph& primary,
	                        std::vector<ID3D12Resource*>& primaryBindings, NativeWork& primaryWork,
	                        Graph& remote, std::vector<ID3D12Resource*>& remoteBindings, NativeWork& remoteWork,
	                        ID3D12Resource* primaryAtlas, const std::vector<SceneLighting::ShadowCopy>& copies, bool sun)
	{
		const auto shape = primaryAtlas->GetDesc();
		if (!owner || remote.Device != owner || primary.Device != 0 || copies.empty() ||
		    shape.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || shape.Format != DXGI_FORMAT_R32_TYPELESS ||
		    shape.Width != shape.Height || shape.MipLevels != 1 || shape.SampleDesc.Count != 1 ||
		    (sun && (shape.DepthOrArraySize != 1 || shape.Height % 2 != 0)))
			throw std::invalid_argument("Invalid WBOIT shadow atlas");

		const UINT size = shape.Height, slices = shape.DepthOrArraySize;
		if (!_atlas || _owner != owner || _atlas->GetDesc().Width != size || _atlas->GetDesc().DepthOrArraySize != slices)
		{
			const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(shape.Format, size, size, UINT16(slices), 1);
			_atlas = submit.Device(owner).CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
			_primaryScratch.Reset();
			_shared = {};
			_capacity = 0;
			_owner = owner;
		}

		Bytes = LocalBytes = 0;
		Export = Invalid;
		const UINT tile = sun ? size / 2 : size;
		const UINT count = sun ? 4u : slices;
		std::vector<bool> covered(count);
		_readers = copies;
		for (const auto& copy : _readers)
		{
			const UINT index = sun ? copy.X / tile + 2 * (copy.Y / tile) : copy.Slice;
			if (!copy.Lifetime || !copy.Source.pResource || index >= count || covered[index] ||
			    copy.X != (sun ? (index % 2) * tile : 0u) || copy.Y != (sun ? (index / 2) * tile : 0u) ||
			    copy.Slice != (sun ? 0u : index) || copy.Width != tile || copy.Height != tile)
				throw std::invalid_argument("Invalid published WBOIT shadow region");
			covered[index] = true;
			LocalBytes += UINT64(tile) * tile * sizeof(float);
		}

		struct Missing
		{
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint;
			UINT X, Y, Slice;
		};
		std::vector<Missing> missing;
		const auto tileDesc = CD3DX12_RESOURCE_DESC::Tex2D(shape.Format, tile, tile, 1, 1);
		for (UINT i = 0; i < count; ++i)
			if (!covered[i])
				missing.push_back({WboitFootprint(submit, owner, tileDesc, Bytes),
				    sun ? (i % 2) * tile : 0u, sun ? (i / 2) * tile : 0u, sun ? 0u : i});

		const auto primaryId = [&](auto* resource, const char* name)
		{ return NativeWork::ResourceFor(primary, primaryBindings, resource, name); };
		const auto remoteId = [&](auto* resource, const char* name)
		{ return NativeWork::ResourceFor(remote, remoteBindings, resource, name); };
		auto* atlas = _atlas.Get();
		std::vector<Access> reads{{remoteId(atlas, "WBOIT owner shadow atlas"), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}};
		const auto read = [&](ID3D12Resource* resource)
		{
			const Id id = remoteId(resource, "WBOIT shadow source");
			if (std::none_of(reads.begin(), reads.end(), [id](const auto& a) { return a.Resource == id; }))
				reads.push_back({id, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE});
		};
		ID3D12Resource* remoteShared = nullptr;
		if (!missing.empty())
		{
			if (_capacity < Bytes)
			{
				_shared = submit.CreateTransferBuffer(owner, Bytes);
				_capacity = Bytes;
			}
			auto* primaryShared = _shared.Resources[0].Get();
			remoteShared = _shared.Resources[1].Get();
			read(remoteShared);
			auto* source = primaryAtlas;
			if (sun)
			{
				// D3D12 permits only whole-subresource copies from depth-stencil resources.
				if (!_primaryScratch)
					_primaryScratch = submit.Device(0).CreateResource(D3D12_HEAP_TYPE_DEFAULT, atlas->GetDesc(),
					    D3D12_RESOURCE_STATE_COMMON);
				source = _primaryScratch.Get();
				primaryWork.Add(primary, "stage primary CSM for cropped WBOIT tiles",
				    {{primaryId(primaryAtlas, "primary CSM"), Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
				     {primaryId(source, "WBOIT CSM scratch"), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
				    [source, primaryAtlas](auto* cmd) { cmd->CopyResource(source, primaryAtlas); });
			}
			Export = primaryWork.Add(primary, "export missing WBOIT shadow regions",
			    {{primaryId(source, "primary WBOIT shadow source"), Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
			     {primaryId(primaryShared, "shared WBOIT shadow regions"), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
			    [source, primaryShared, missing, sun, tile](auto* cmd)
			    {
				    for (const auto& region : missing)
				    {
					    const CD3DX12_TEXTURE_COPY_LOCATION from(source, region.Slice), to(primaryShared, region.Footprint);
					    const D3D12_BOX box{region.X, region.Y, 0, region.X + tile, region.Y + tile, 1};
					    cmd->CopyTextureRegion(&to, 0, 0, 0, &from, sun ? &box : nullptr);
				    }
			    });
		}
		for (const auto& copy : _readers) read(copy.Source.pResource);
		Import = remoteWork.Add(remote, "assemble published WBOIT shadow atlas", std::move(reads),
		    [this, atlas, remoteShared, missing](auto* cmd)
		    {
			    for (const auto& region : missing)
			    {
				    const CD3DX12_TEXTURE_COPY_LOCATION from(remoteShared, region.Footprint), to(atlas, region.Slice);
				    cmd->CopyTextureRegion(&to, region.X, region.Y, 0, &from, nullptr);
			    }
			    for (const auto& copy : _readers)
			    {
				    const CD3DX12_TEXTURE_COPY_LOCATION to(atlas, copy.Slice);
				    cmd->CopyTextureRegion(&to, copy.X, copy.Y, 0, &copy.Source, nullptr);
			    }
		    });
		return atlas;
	}

  private:
	Id _owner = Invalid;
	GDX12ResourceHandle _atlas, _primaryScratch;
	QueueSubmission::TransferBuffer _shared;
	UINT64 _capacity = 0;
	std::vector<SceneLighting::ShadowCopy> _readers;
};
} // namespace BistroRender
