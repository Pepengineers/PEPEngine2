#pragma once

#include "Graph/QueueSubmission.h"
#include "NativeWork.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"

namespace BistroRender
{
// The caller owns cross-graph waits and retains this transfer until its last packet retires.
class ProbeTransfer
{
  public:
    ProbeTransfer(const QueueSubmission& submissions, Id remote, UINT resolution)
        : _remote(remote), _resolution(resolution), _mips(GDX12RenderHelpers::MipCount(resolution))
    {
        if (!remote || !resolution || resolution > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            (resolution & (resolution - 1)))
            throw std::invalid_argument("Invalid remote reflection probe shape or adapter");

        const auto shape = CD3DX12_RESOURCE_DESC::Tex2D(Format, resolution, resolution, 6, UINT16(_mips));
        _footprints.resize(6 * _mips);
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> remoteFootprints(_footprints.size());
        UINT64 remoteBytes = 0;
        submissions.Device(0).GetDevice()->GetCopyableFootprints(&shape, 0, UINT(_footprints.size()), 0,
                                                  _footprints.data(), nullptr, nullptr, &_bytes);
        submissions.Device(remote).GetDevice()->GetCopyableFootprints(&shape, 0, UINT(remoteFootprints.size()), 0,
                                                                        remoteFootprints.data(), nullptr, nullptr,
                                                                        &remoteBytes);
        if (!_bytes || _bytes == UINT64_MAX || _bytes != remoteBytes)
            throw std::runtime_error("Invalid cross-adapter reflection probe transfer size");
        for (size_t i = 0; i < _footprints.size(); ++i)
        {
            const auto& a = _footprints[i];
            const auto& b = remoteFootprints[i];
            if (a.Offset != b.Offset || a.Footprint.RowPitch != b.Footprint.RowPitch ||
                a.Footprint.Width != b.Footprint.Width || a.Footprint.Height != b.Footprint.Height ||
                a.Footprint.Depth != b.Footprint.Depth || a.Footprint.Format != b.Footprint.Format)
                throw std::runtime_error("Reflection probe adapters need matching footprints");
        }

        _buffer = submissions.CreateTransferBuffer(remote, _bytes);
    }

    UINT MipCount() const { return _mips; }
    UINT64 TransferBytes() const { return _bytes; }

    Id AppendExport(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work,
                    ID3D12Resource* filtered, UINT firstFace, UINT faceCount, UINT sourceCube = 0) const
    {
        return AppendSharedCopy(graph, bindings, work, filtered, firstFace, faceCount, sourceCube, true);
    }

    Id AppendImport(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work,
                    ID3D12Resource* published, UINT firstFace, UINT faceCount, UINT cubeIndex) const
    {
        return AppendSharedCopy(graph, bindings, work, published, firstFace, faceCount, cubeIndex, false);
    }

  private:
    static constexpr DXGI_FORMAT Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    static constexpr State CopySource = D3D12_RESOURCE_STATE_COPY_SOURCE;
    static constexpr State CopyDest = D3D12_RESOURCE_STATE_COPY_DEST;

    UINT BaseMip(const D3D12_RESOURCE_DESC& shape) const
    {
        if (shape.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || shape.Format != Format ||
            shape.SampleDesc.Count != 1 || shape.MipLevels < _mips)
            throw std::invalid_argument("Incompatible reflection probe transfer texture");
        const UINT baseMip = shape.MipLevels - _mips;
        if ((shape.Width >> baseMip) != _resolution || (shape.Height >> baseMip) != _resolution)
            throw std::invalid_argument("Incompatible reflection probe transfer resolution");
        return baseMip;
    }

    Id AppendSharedCopy(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work,
                        ID3D12Resource* texture, UINT firstFace, UINT faceCount, UINT cubeIndex, bool exporting) const
    {
        if (graph.Device != (exporting ? _remote : 0) ||
            firstFace >= 6 || !faceCount || faceCount > 6 - firstFace)
            throw std::invalid_argument("Invalid reflection probe transfer face range or graph");
        const auto shape = texture->GetDesc();
        if (cubeIndex >= shape.DepthOrArraySize / 6)
            throw std::invalid_argument("Reflection probe transfer cube index is out of range");
        const UINT textureMips = shape.MipLevels;
        const UINT baseMip = BaseMip(shape);
        auto* shared = _buffer.Resources[exporting ? 1 : 0].Get();
        const Id sharedId = NativeWork::ResourceFor(graph, bindings, shared, "shared reflection probe radiance");
        const Id textureId = NativeWork::ResourceFor(graph, bindings, texture,
            exporting ? "remote filtered reflection probe" : "published reflection probes");
        const std::vector<Access> accesses = exporting
            ? std::vector<Access>{{textureId, Mode::Read, CopySource}, {sharedId, Mode::Write, CopyDest}}
            : std::vector<Access>{{sharedId, Mode::Read, CopySource}, {textureId, Mode::Write, CopyDest}};
        const auto operation = work.Add(graph, exporting ? "remote export reflection probe faces" : "GPU0 publish remote reflection probe faces",
                        accesses, [this, shared, texture, firstFace, faceCount, textureMips, baseMip, cubeIndex,
                                   exporting](auto* cmd) {
                            for (UINT face = firstFace; face < firstFace + faceCount; ++face)
                                for (UINT mip = 0; mip < _mips; ++mip)
                                {
                                    const CD3DX12_TEXTURE_COPY_LOCATION buffer(shared, _footprints[face * _mips + mip]);
                                    const CD3DX12_TEXTURE_COPY_LOCATION image(texture,
                                        (cubeIndex * 6 + face) * textureMips + baseMip + mip);
                                    const auto& src = exporting ? image : buffer;
                                    const auto& dst = exporting ? buffer : image;
                                    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                                }
                        });
        auto& bytes = graph.Operations[operation].TransferBytes;
        for (UINT face = firstFace; face < firstFace + faceCount; ++face)
            for (UINT mip = 0; mip < _mips; ++mip)
            {
                const auto& footprint = _footprints[face * _mips + mip].Footprint;
                bytes += UINT64(footprint.Width) * footprint.Height * 8;
            }
        return operation;
    }

    Id _remote;
    UINT _resolution, _mips;
    UINT64 _bytes = 0;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> _footprints;
    QueueSubmission::TransferBuffer _buffer;
};
} // namespace BistroRender
