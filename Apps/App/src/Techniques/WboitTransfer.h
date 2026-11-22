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
    WboitResultTransfer(const QueueSubmission& submit, Id owner, const std::vector<ID3D12Resource*>& result)
    {
        _layout.resize(result.size());
        for (UINT i = 0; i < result.size(); ++i)
        {
            _layout[i] = WboitFootprint(submit, owner, result[i]->GetDesc(), Bytes);
            if (i == 0) LayerBytes = Bytes;
        }
        _shared = submit.CreateTransferBuffer(owner, Bytes);
    }
    Id Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work,
              const std::vector<ID3D12Resource*>& result, bool exporting)
    {
        auto* shared = _shared.Resources[exporting ? 1 : 0].Get();
        const auto bind = [&](auto* image){ return NativeWork::ResourceFor(graph, bindings, image, "WBOIT result"); };
        const auto usage = exporting ? Mode::Read : Mode::Write;
        const State state = exporting ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
        std::vector<Access> access{{bind(shared), exporting ? Mode::Write : Mode::Read,
            State(exporting ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE)}};
        for (auto* image : result) access.push_back({bind(image), usage, state});
        return work.Add(graph, exporting ? "owner export transparent RGB/transmittance and motion" : "GPU0 import transparent RGB/transmittance and motion",
            std::move(access), [=, layout = _layout](auto* cmd){
                for (UINT i = 0; i < result.size(); ++i)
                {
                    CD3DX12_TEXTURE_COPY_LOCATION image(result[i], 0), buffer(shared, layout[i]);
                    cmd->CopyTextureRegion(exporting ? &buffer : &image, 0, 0, 0, exporting ? &image : &buffer, nullptr);
                }
            });
    }
    UINT64 Bytes = 0, LayerBytes = 0;
  private:
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> _layout;
    QueueSubmission::TransferBuffer _shared;
};
}
