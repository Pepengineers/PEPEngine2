#pragma once

#include "Engine.RendererDX12/FrameGraph/PassContext.h"
#include "Engine.RendererDX12/GDX12SharedAllocation.h"

struct FrameGraphBuffer
{
    GDX12Resource Resource;
    std::unique_ptr<GDX12Descriptor> SRV;
    std::unique_ptr<GDX12Descriptor> UAV;
};

struct FrameGraphPhysicalResource
{
    GDX12DeviceResources* DeviceResources = nullptr;
    ResourceDescription Description;
    std::unique_ptr<GDX12Texture> Texture;
    GDX12Texture* ImportedTexture = nullptr;
    std::vector<FrameGraphBuffer> Buffers;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> Footprints;
    uint64_t AllocationBytes = 0;
    PassBinding CreatePassBinding(ResourceUsage usages);
};

struct FrameGraphSharedResources
{
    std::vector<std::unique_ptr<GDX12SharedAllocation>> Allocations;
    std::unique_ptr<FrameGraphPhysicalResource> Producer;
    std::unique_ptr<FrameGraphPhysicalResource> Consumer;
    FrameGraphPhysicalResource& GetDeviceMapping(GDX12Device* device) const;
};

std::unique_ptr<FrameGraphPhysicalResource> CreateFrameGraphNative(GDX12DeviceResources* owner, const ResourceDescription& description);
std::unique_ptr<FrameGraphPhysicalResource> WrapFrameGraphImport(GDX12DeviceResources* owner, GDX12Texture* texture, const ResourceDescription& description);
HRESULT TryCreateFrameGraphShared(GDX12DeviceResources* producer, GDX12DeviceResources* other,
    const ResourceDescription& description, bool bufferPayload, FrameGraphSharedResources& result);
void TransitionFrameGraphResource(GDX12CommandList* commands, FrameGraphPhysicalResource& resource, D3D12_RESOURCE_STATES state);
void FrameGraphUAVBarrier(GDX12CommandList* commands, FrameGraphPhysicalResource& resource);
struct FrameGraphCopyStatistics { uint64_t Bytes = 0; uint32_t Commands = 0; };
FrameGraphCopyStatistics CopyFrameGraphResource(GDX12CommandList* commands, FrameGraphPhysicalResource& source, FrameGraphPhysicalResource& target);
// Check whether the device is capable of doing operations, described by the port
PortCapabilities QueryFrameGraphCapabilities(GDX12Device* device, const PortDescription& schema);
ResourceDescription DescribeFrameGraphTexture(GDX12Texture* texture);
