#pragma once
#include "NativeWork.h"
#include "Graph/QueueSubmission.h"

namespace BistroRender
{
// Both endpoints see one allocation. Verify each footprint on both devices, including its row padding.
inline D3D12_PLACED_SUBRESOURCE_FOOTPRINT WboitFootprint(const QueueSubmission& submit, Id owner,
    D3D12_RESOURCE_DESC shape, UINT64& bytes)
{
    const UINT64 offset = (bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                          ~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT primary{}, secondary{};
    UINT64 size = 0, secondarySize = 0;
    submit.Device(0).GetDevice()->GetCopyableFootprints(&shape, 0, 1, offset, &primary, nullptr, nullptr, &size);
    submit.Device(owner).GetDevice()->GetCopyableFootprints(&shape, 0, 1, offset, &secondary, nullptr, nullptr, &secondarySize);
    if (!size || size == UINT64_MAX || secondarySize != size || primary.Offset != secondary.Offset ||
        primary.Footprint.Format != secondary.Footprint.Format || primary.Footprint.Width != secondary.Footprint.Width ||
        primary.Footprint.Height != secondary.Footprint.Height || primary.Footprint.Depth != secondary.Footprint.Depth ||
        primary.Footprint.RowPitch != secondary.Footprint.RowPitch)
        throw std::runtime_error("Incompatible cross-adapter WBOIT texture footprint");
    bytes = offset + size;
    return primary;
}

class WboitResultTransfer
{
  public:
    WboitResultTransfer(const QueueSubmission& submit, Id owner, ID3D12Resource* layer)
    {
        _layout = WboitFootprint(submit, owner, layer->GetDesc(), Bytes);
        _shared = submit.CreateTransferBuffer(owner, Bytes);
    }

    Id Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work,
              ID3D12Resource* layer, bool exporting)
    {
        auto* shared = _shared.Resources[exporting ? 1 : 0].Get();
        const auto bind = [&](auto* image){ return NativeWork::ResourceFor(graph, bindings, image, "WBOIT result"); };
        const State read = D3D12_RESOURCE_STATE_COPY_SOURCE;
        const State write = D3D12_RESOURCE_STATE_COPY_DEST;
        return work.Add(graph, exporting ? "owner export transparent RGB/transmittance" : "GPU0 import transparent RGB/transmittance",
            {{bind(shared), exporting ? Mode::Write : Mode::Read, exporting ? write : read},
             {bind(layer), exporting ? Mode::Read : Mode::Write, exporting ? read : write}},
            [=, layout = _layout](auto* cmd) {
                const CD3DX12_TEXTURE_COPY_LOCATION image(layer, 0), buffer(shared, layout);
                cmd->CopyTextureRegion(exporting ? &buffer : &image, 0, 0, 0, exporting ? &image : &buffer, nullptr);
            });
    }

    UINT64 Bytes = 0;
  private:
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT _layout{};
    QueueSubmission::TransferBuffer _shared;
};
}
