#pragma once

#include "Graph/QueueSubmission.h"
#include "NativeWork.h"

namespace BistroRender
{
// The owner retains the buffer until the primary import packet retires.
class ShadowDepthTransfer
{
  public:
	ShadowDepthTransfer(const QueueSubmission& submit, Id owner, UINT resolution, UINT count)
	    : _owner(owner), _resolution(resolution), _footprints(count)
	{
		const auto shape = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, resolution, resolution,
		                                              UINT16(count), 1, 1, 0,
		                                              D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
		UINT64 primaryBytes = 0, ownerBytes = 0;
		std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> ownerFootprints(count);
		submit.Device(0).GetDevice()->GetCopyableFootprints(&shape, 0, count, 0, _footprints.data(),
		                                                   nullptr, nullptr, &primaryBytes);
		submit.Device(owner).GetDevice()->GetCopyableFootprints(&shape, 0, count, 0, ownerFootprints.data(),
		                                                       nullptr, nullptr, &ownerBytes);
		if (!primaryBytes || primaryBytes == UINT64_MAX || primaryBytes != ownerBytes)
			throw std::runtime_error("Incompatible shadow transfer size");
		for (UINT i = 0; i < count; ++i)
		{
			const auto& a = _footprints[i];
			const auto& b = ownerFootprints[i];
			if (a.Offset != b.Offset || a.Footprint.Format != b.Footprint.Format ||
			    a.Footprint.Width != b.Footprint.Width || a.Footprint.Height != b.Footprint.Height ||
			    a.Footprint.Depth != b.Footprint.Depth || a.Footprint.RowPitch != b.Footprint.RowPitch)
				throw std::runtime_error("Incompatible shadow transfer footprint");
		}
		_buffer = submit.CreateTransferBuffer(owner, primaryBytes);
	}

	D3D12_TEXTURE_COPY_LOCATION OwnerSlice(UINT slice) const
	{
		return CD3DX12_TEXTURE_COPY_LOCATION(_buffer.Resources[1].Get(), _footprints.at(slice));
	}

	Id Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work, ID3D12Resource* depth,
	          std::vector<UINT> slices, bool exporting) const
	{
		const auto shape = depth->GetDesc();
		if (graph.Device != (exporting ? _owner : 0) || slices.size() != _footprints.size() ||
		    shape.Format != DXGI_FORMAT_R32_TYPELESS || shape.Width != _resolution || shape.Height != _resolution ||
		    shape.MipLevels != 1 || shape.SampleDesc.Count != 1 ||
		    std::any_of(slices.begin(), slices.end(), [&](UINT slice) { return slice >= shape.DepthOrArraySize; }))
			throw std::invalid_argument("Incompatible shadow depth array transfer");
		auto* shared = _buffer.Resources[exporting ? 1 : 0].Get();
		const auto sharedId = NativeWork::ResourceFor(graph, bindings, shared, "cross-adapter shadow depth");
		const auto depthId = NativeWork::ResourceFor(graph, bindings, depth, "shadow depth array");
		const auto source = exporting ? depthId : sharedId;
		const auto destination = exporting ? sharedId : depthId;
		return work.Add(graph, exporting ? "owner export shadow depth slices" : "GPU0 import shadow depth slices",
		                {{source, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
		                 {destination, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
		                [this, depth, shared, slices = std::move(slices), exporting](auto* cmd)
		                {
			                for (size_t i = 0; i < slices.size(); ++i)
			                {
				                const CD3DX12_TEXTURE_COPY_LOCATION image(depth, slices[i]);
				                const CD3DX12_TEXTURE_COPY_LOCATION buffer(shared, _footprints[i]);
				                auto& from = exporting ? image : buffer;
				                auto& to = exporting ? buffer : image;
				                cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
			                }
		                });
	}

  private:
	Id _owner;
	UINT _resolution;
	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> _footprints;
	QueueSubmission::TransferBuffer _buffer;
};
} // namespace BistroRender
