#pragma once

#include "Engine.RendererDX12/GDX12Skinning.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"

template<class T>
static void CheckSkinSource(GDX12Device& device, GDX12UploadBuffer<T>& source, const std::vector<T>& expected)
{
    D3D12_HEAP_PROPERTIES properties = {};
    ThrowIfFailed(source.GetResource().D3DResource->GetHeapProperties(&properties, nullptr));
    Require(properties.Type == D3D12_HEAP_TYPE_DEFAULT, "Skin shader inputs must use DEFAULT memory");
    const UINT64 bytes = UINT64(expected.size()) * sizeof(T);
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> readback;
    ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({source.GetResource().GetCopySourceBarrier()});
    commands->GetCommandList()->CopyBufferRegion(readback.Get(), 0, source.GetResource().D3DResource.Get(), 0, bytes);
    commands->ResourceBarrier({source.GetResource().GetNonPixelShaderResourceBarrier()});
    queue->ExecuteCommandList(commands); queue->Flush();
    void* mapped = nullptr;
    const D3D12_RANGE range = {0, static_cast<SIZE_T>(bytes)};
    ThrowIfFailed(readback->Map(0, &range, &mapped));
    const bool equal = std::memcmp(mapped, expected.data(), static_cast<size_t>(bytes)) == 0;
    const D3D12_RANGE noWrite = {};
    readback->Unmap(0, &noWrite);
    Require(equal, "Initial DEFAULT skin source differs from CPU input");
}

static void TestSkinPlacement(GDX12Device& device, GDX12DeviceResources& resources)
{
    bool rejected = false;
    try { GDX12UploadBuffer<Vertex> oversized(&device, UINT_MAX / sizeof(Vertex) + 1, EBufferType::Default, false); }
    catch (const std::length_error&) { rejected = true; }
    Require(rejected, "Oversized DEFAULT buffers must fail before byte-size truncation");
    GDX12UploadBuffer<UINT> bounded(&device, 2, EBufferType::Upload, false);
    bounded.TrackChanges();
    const UINT values[] = {17, 23};
    bounded.CopyData(0, values, 2);
    bounded.CopyData(UINT_MAX, values, 2);
    Require(bounded.GetCPUData()[0] == 17 && bounded.GetCPUData()[1] == 23,
        "Overflowing CPU copy ranges must leave staging unchanged");
    SubMesh source;
    source.Vertices.resize(2);
    for (auto& vertex : source.Vertices)
    {
        vertex.Position = Vector3(1, 2, 3);
        vertex.Normal = Vector3(1, 1, 0); vertex.Normal.Normalize();
        vertex.Tangent = Vector3(1, -1, 0); vertex.Tangent.Normalize();
    }
    source.Bones.resize(2);
    for (auto& bone : source.Bones) bone.InverseBind = Matrix::Identity;
    source.SkinVertices = {{0, 2}, {2, 0}};
    source.Influences = {{0, 0.25f}, {1, 0.75f}};
    const UINT slots = static_cast<UINT>(resources.FrameConstants.size());
    GDX12Skinning skinning(&device);
    auto mesh = skinning.CreateMesh(source, slots);
    CheckSkinSource(device, *mesh->Source->Vertices, source.Vertices);
    CheckSkinSource(device, *mesh->Source->Ranges, source.SkinVertices);
    CheckSkinSource(device, *mesh->Source->Influences, source.Influences);
    const MeshHandle sharedHandle(7, 2);
    auto shared = skinning.CreateMesh(source, slots, sharedHandle, 1);
    auto peer = skinning.CreateMesh(source, slots, sharedHandle, 1);
    auto revised = skinning.CreateMesh(source, slots, sharedHandle, 2);
    auto regenerated = skinning.CreateMesh(source, slots, MeshHandle(7, 3), 1);
    Require(shared->Source == peer->Source && shared->Source != revised->Source &&
        shared->Source != regenerated->Source && shared->Palettes[0] != peer->Palettes[0],
        "Skin instances must share immutable inputs only within a mesh generation/revision, with private palettes");
    std::weak_ptr<const GDX12SkinSource> released = shared->Source;
    shared.reset(); peer.reset();
    Require(released.expired(), "Skin source cache must not retain unused GPU inputs");
    for (auto& palette : mesh->Palettes)
    {
        D3D12_HEAP_PROPERTIES properties = {};
        ThrowIfFailed(palette->GetResource().D3DResource->GetHeapProperties(&properties, nullptr));
        Require(properties.Type == D3D12_HEAP_TYPE_DEFAULT, "Skin palettes must use DEFAULT memory");
    }
    mesh->OutputVertex = resources.GeometryBuffer->AllocateSkinnedVertices(2);
    auto* queue = device.GetCommandQueue();
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(UINT64(slots) * 2 * sizeof(Vertex));
    ComPtr<ID3D12Resource> readback;
    ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    for (UINT iteration = 0; iteration < 2; ++iteration)
    {
        for (UINT slot = 0; slot < slots; ++slot)
        {
            std::vector<GDX12SkinPalette> palette(3);
            for (UINT bone = 0; bone < palette.size(); ++bone)
            {
                const float x = static_cast<float>(iteration * 20 + slot * 2 + bone * 4);
                palette[bone].Current = Matrix::CreateScale(2, 3, 4) * Matrix::CreateTranslation(x, 0, 0);
                palette[bone].Previous = Matrix::CreateScale(2, 3, 4) * Matrix::CreateTranslation(x - 1, 0, 0);
            }
            auto* commands = queue->GetCommandList();
            skinning.Dispatch(commands, *resources.GeometryBuffer, *mesh, slot, palette);
            auto* vertices = resources.GeometryBuffer->BeginSkinning(commands);
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(vertices,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->GetCommandList()->ResourceBarrier(1, &barrier);
            commands->GetCommandList()->CopyBufferRegion(readback.Get(), UINT64(slot) * 2 * sizeof(Vertex),
                vertices, UINT64(mesh->OutputVertex) * sizeof(Vertex), 2 * sizeof(Vertex));
            barrier = CD3DX12_RESOURCE_BARRIER::Transition(vertices,
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commands->GetCommandList()->ResourceBarrier(1, &barrier);
            resources.GeometryBuffer->EndSkinning(commands);
            queue->ExecuteCommandList(commands);
        }
        queue->Flush();
        void* mapped = nullptr;
        const D3D12_RANGE range = {0, static_cast<SIZE_T>(desc.Width)};
        ThrowIfFailed(readback->Map(0, &range, &mapped));
        bool matches = true;
        for (UINT slot = 0; slot < slots; ++slot)
            for (UINT vertexIndex = 0; vertexIndex < 2; ++vertexIndex)
            {
                const auto& vertex = static_cast<const Vertex*>(mapped)[slot * 2 + vertexIndex];
                const float x = static_cast<float>(2 + iteration * 20 + slot * 2 + (vertexIndex ? 8 : 3));
                matches &= std::abs(vertex.Position.x - x) < 0.0001f &&
                    std::abs(vertex.PreviousPosition.x - (x - 1)) < 0.0001f &&
                    std::abs(vertex.Position.y - 6) < 0.0001f && std::abs(vertex.Position.z - 12) < 0.0001f &&
                    std::abs(vertex.Normal.x - 3 / std::sqrt(13.0f)) < 0.0001f &&
                    std::abs(vertex.Normal.y - 2 / std::sqrt(13.0f)) < 0.0001f;
            }
        const D3D12_RANGE noWrite = {};
        readback->Unmap(0, &noWrite);
        Require(matches, "Queued DEFAULT skin palettes lost current/previous pose or normals");
    }
    resources.GeometryBuffer->ReleaseSkinnedVertices(mesh->OutputVertex, 2);
    mesh.reset();
    source.Bones.clear(); source.SkinVertices.clear(); source.Influences.clear();
    mesh = skinning.CreateMesh(source, slots);
    CheckSkinSource(device, *mesh->Source->Ranges, std::vector<SkinVertexRange>(2));
    CheckSkinSource(device, *mesh->Source->Influences, std::vector<SkinInfluence>(1));
    mesh->OutputVertex = resources.GeometryBuffer->AllocateSkinnedVertices(2);
    auto* commands = queue->GetCommandList();
    skinning.Dispatch(commands, *resources.GeometryBuffer, *mesh, slots - 1,
        {{Matrix::Identity, Matrix::Identity}});
    resources.GeometryBuffer->EndSkinning(commands);
    queue->ExecuteCommandList(commands); queue->Flush();
    resources.GeometryBuffer->ReleaseSkinnedVertices(mesh->OutputVertex, 2);
    std::cout << "DEFAULT skin sources, queued palette slots, pose edits, normals and zero influences passed.\n";
}

static void BenchmarkSkinPlacement(GDX12Device& device, GDX12DeviceResources& resources)
{
    constexpr UINT vertexCount = 65536, boneCount = 128, influencesPerVertex = 5;
    SubMesh source;
    source.Vertices.resize(vertexCount);
    source.SkinVertices.resize(vertexCount);
    source.Influences.reserve(vertexCount * influencesPerVertex);
    source.Bones.resize(boneCount);
    for (auto& bone : source.Bones) bone.InverseBind = Matrix::Identity;
    for (UINT i = 0; i < vertexCount; ++i)
    {
        source.Vertices[i].Position = Vector3(static_cast<float>(i % 256), static_cast<float>(i / 256), 0);
        source.Vertices[i].Normal = Vector3::UnitZ;
        source.SkinVertices[i] = {i * influencesPerVertex, influencesPerVertex};
        for (UINT influence = 0; influence < influencesPerVertex; ++influence)
            source.Influences.push_back({(i + influence) % boneCount, 0.2f});
    }
    GDX12Skinning skinning(&device);
    const UINT slots = static_cast<UINT>(resources.FrameConstants.size());
    const auto start = std::chrono::steady_clock::now();
    auto mesh = skinning.CreateMesh(source, slots);
    const auto created = std::chrono::steady_clock::now();
    mesh->OutputVertex = resources.GeometryBuffer->AllocateSkinnedVertices(vertexCount);
    std::vector<GDX12SkinPalette> palette(boneCount + 1);
    std::cout << "PLACEMENT skin dimensions vertices=" << vertexCount << " bones=" << boneCount
        << " influences=" << source.Influences.size() << " slots=" << slots
        << " source_bytes=" << UINT64(vertexCount) * (sizeof(Vertex) + sizeof(SkinVertexRange)) +
            source.Influences.size() * sizeof(SkinInfluence)
        << " palette_bytes=" << palette.size() * sizeof(GDX12SkinPalette)
        << " create_cpu_ms=" << std::chrono::duration<double, std::milli>(created - start).count() << '\n';
    MeasurePlacement(device, "skin", slots, [&](GDX12CommandList* commands, UINT slot, UINT iteration)
    {
        for (UINT bone = 0; bone < palette.size(); ++bone)
        {
            const float offset = static_cast<float>((iteration + bone) % 64) * 0.125f;
            palette[bone].Current = Matrix::CreateTranslation(offset, 0, 0);
            palette[bone].Previous = Matrix::CreateTranslation(offset - 0.125f, 0, 0);
        }
        skinning.Dispatch(commands, *resources.GeometryBuffer, *mesh, slot, palette);
        resources.GeometryBuffer->EndSkinning(commands);
    });
    resources.GeometryBuffer->ReleaseSkinnedVertices(mesh->OutputVertex, vertexCount);
}
