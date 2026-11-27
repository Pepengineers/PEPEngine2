#include "Engine.RendererDX12/GDX12ShaderTable.h"

#include "Engine.RendererDX12/GDX12RayTracingPipelineState.h"
#include <limits>
#include <stdexcept>

GDX12ShaderTable::GDX12ShaderTable(const GDX12RayTracingPipelineState& pipeline, const GDX12ShaderTableDesc& description)
    : _device(pipeline.GetDevice())
{
    std::vector<BYTE> data;
    auto append = [&](const std::wstring* exports, size_t count)
    {
        D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE range{};
        if (!count) { return range; }
        constexpr UINT stride = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        const UINT64 offset = (UINT64(data.size()) + D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT - 1)
            & ~(UINT64(D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT) - 1);
        const auto limit = (std::numeric_limits<UINT>::max)();
        if (offset > limit || count > (limit - offset) / stride)
        { throw std::overflow_error("ERROR: Shader table exceeds upload buffer size limits."); }
        range = {offset, UINT64(stride) * count, stride};
        data.resize(static_cast<size_t>(offset + range.SizeInBytes), 0);
        for (size_t i = 0; i < count; ++i)
        {
            auto* record = data.data() + offset + i * stride;
            memcpy(record, pipeline.GetShaderIdentifier(exports[i]), stride);
        }
        return range;
    };
    const auto rayGeneration = append(&description.RayGeneration, 1);
    _description.RayGenerationShaderRecord = {rayGeneration.StartAddress, rayGeneration.SizeInBytes};
    _description.MissShaderTable = append(description.Miss.data(), description.Miss.size());
    _description.HitGroupTable = append(description.HitGroups.data(), description.HitGroups.size());
    _buffer = std::make_unique<GDX12UploadBuffer<BYTE>>(_device, static_cast<UINT>(data.size()), EBufferType::Upload, false);
    _buffer->CopyData(0, data.data(), static_cast<UINT>(data.size()));
    const auto base = _buffer->GetElementAddress(0);
    _description.RayGenerationShaderRecord.StartAddress += base;
    if (_description.MissShaderTable.SizeInBytes) { _description.MissShaderTable.StartAddress += base; }
    if (_description.HitGroupTable.SizeInBytes) { _description.HitGroupTable.StartAddress += base; }
}

D3D12_DISPATCH_RAYS_DESC GDX12ShaderTable::GetDispatchDescription(UINT width, UINT height, UINT depth) const
{
    constexpr UINT64 maxRays = D3D12_RAYTRACING_MAX_RAY_GENERATION_SHADER_THREADS;
    if (width && height && (UINT64(width) * height > maxRays
        || depth > maxRays / (UINT64(width) * height)))
    { throw std::invalid_argument("ERROR: Ray dispatch dimensions exceed the DXR ray count limit."); }
    auto description = _description;
    description.Width = width;
    description.Height = height;
    description.Depth = depth;
    return description;
}
