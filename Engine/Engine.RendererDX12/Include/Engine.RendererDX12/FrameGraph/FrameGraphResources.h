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
    // Both device mappings of shared memory use the producer mapping's fence history.
    FrameGraphPhysicalResource* SharedProducer = nullptr;
    ResourceDescription Description;
    std::unique_ptr<GDX12Texture> Texture;
    GDX12Texture* ImportedTexture = nullptr;
    std::vector<FrameGraphBuffer> Buffers;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> Footprints;
    // Device-reported byte spans, including row padding, for partial texture transfers.
    std::vector<uint64_t> SubresourceTransferBytes;
    uint64_t AllocationBytes = 0;
    uint64_t TransferBytes = 0; // Copyable footprint size of a shared texture.
    PassBinding CreatePassBinding(ResourceUsage usages);
};

struct FrameGraphSharedResources
{
    std::vector<std::unique_ptr<GDX12SharedAllocation>> Allocations;
    std::unique_ptr<FrameGraphPhysicalResource> Producer;
    std::unique_ptr<FrameGraphPhysicalResource> Consumer;
    FrameGraphPhysicalResource& GetDeviceMapping(GDX12Device* device) const;
};

// Physical representations and readiness of one logical output, per slot or reused across frames.
// Alias outputs can refer to the same local or shared allocation.
struct FrameGraphMaterialization
{
    std::unique_ptr<FrameGraphPhysicalResource> LocalResource;
    FrameGraphPhysicalResource* ProducerResource = nullptr;
    FrameGraphSharedResources Shared;
    FrameGraphSharedResources* SharedAllocationAlias = nullptr;
    std::map<EDeviceRole, std::unique_ptr<FrameGraphPhysicalResource>> LocalCopies;
    GDX12CompletionPoint ProducerReady;
    GDX12CompletionPoint SharedReady;
    std::array<GDX12CompletionPoint, 2> LocalCopyReady{};
    // Copy contents persist between frames; initialized replicas accept partial updates.
    bool SharedInitialized = false;
    std::array<bool, 2> LocalCopyInitialized{};
    bool HasUpdatedTextureArrayElements = false;
    std::vector<UINT> UpdatedTextureArrayElements;
};

std::unique_ptr<FrameGraphPhysicalResource> CreateFrameGraphNative(GDX12DeviceResources* owner, const ResourceDescription& description);
std::unique_ptr<FrameGraphPhysicalResource> WrapFrameGraphImport(GDX12DeviceResources* owner, GDX12Texture* texture, const ResourceDescription& description);
HRESULT TryCreateFrameGraphShared(GDX12DeviceResources* producer, GDX12DeviceResources* other,
    const ResourcePlan& plan, FrameGraphSharedResources& result);
void TransitionFrameGraphResource(GDX12CommandList* commands, FrameGraphPhysicalResource& resource, D3D12_RESOURCE_STATES state);
void FrameGraphUAVBarrier(GDX12CommandList* commands, FrameGraphPhysicalResource& resource);
struct FrameGraphCopyStatistics { uint64_t Bytes = 0; uint32_t Commands = 0; };
// Null selects the whole resource. A validated list selects complete texture array elements, including all mips.
FrameGraphCopyStatistics CopyFrameGraphResource(GDX12CommandList* commands, FrameGraphPhysicalResource& source,
    FrameGraphPhysicalResource& target, const std::vector<UINT>* arrayElements = nullptr);
// Check whether the device is capable of doing operations, described by the port
PortCapabilities QueryFrameGraphCapabilities(GDX12Device* device, const PortDescription& schema);
ResourceDescription DescribeFrameGraphTexture(GDX12Texture* texture);
