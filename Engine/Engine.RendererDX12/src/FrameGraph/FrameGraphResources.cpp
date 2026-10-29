#include "Engine.RendererDX12/FrameGraph/FrameGraphResources.h"

#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"

#include <limits>

namespace
{
    uint64_t AddBytes(uint64_t a, uint64_t b)
    {
        if (a > (std::numeric_limits<uint64_t>::max)() - b) { throw std::overflow_error("ERROR: FrameGraph allocation byte count exceeds uint64_t. Reduce resource capacities or texture dimensions so "
            "the total byte count fits in uint64_t."); }
        return a + b;
    }

    uint64_t BufferBytes(uint64_t capacity, uint32_t stride, bool raw)
    {
        if (!capacity || (!raw && (!stride || stride > D3D12_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES || stride % 4)))
        { throw std::invalid_argument("ERROR: Given buffer capacity or structured stride is invalid. Use positive Capacity and a structured Stride "
            "divisible by four and no greater than D3D12_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES."); }
        if (raw)
        {
            if (capacity % 4) { throw std::invalid_argument("ERROR: Raw buffer byte capacity must be a multiple of four. Set the raw-buffer Capacity to a positive byte "
                "count divisible by four."); }
            return capacity;
        }
        if (capacity > (std::numeric_limits<uint64_t>::max)() / stride)
        { throw std::overflow_error("ERROR: FrameGraph buffer byte capacity exceeds uint64_t. Reduce Capacity or Stride so their byte-size product "
            "fits in uint64_t."); }
        return capacity * stride;
    }

    D3D12_RESOURCE_FLAGS TextureFlags(const ResourceDescription& description)
    {
        auto flags = D3D12_RESOURCE_FLAG_NONE;
        if (HasUsage(description.Usages, ResourceUsage::RenderTarget)) { flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET; }
        if (HasUsage(description.Usages, ResourceUsage::DepthWrite) || HasUsage(description.Usages, ResourceUsage::DepthRead))
        { flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL; }
        if (HasUsage(description.Usages, ResourceUsage::UnorderedAccess)) { flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; }
        return flags;
    }

    D3D12_RESOURCE_DESC NativeTextureDescription(const ResourceDescription& description)
    {
        return CD3DX12_RESOURCE_DESC::Tex2D(description.IsDepth() ? DXGI_FORMAT_R32_TYPELESS : description.Format,
            description.Width, description.Height, static_cast<UINT16>(description.ArraySize),
            1, 1, 0, TextureFlags(description));
    }

    GDX12TextureDesc TextureDescription(GDX12DeviceResources* owner, const ResourceDescription& description,
        ComPtr<ID3D12Resource> external = nullptr)
    {
        GDX12TextureDesc desc;
        desc.Device = owner->Device;
        desc.Width = description.Width;
        desc.Height = description.Height;
        desc.ArraySize = description.ArraySize;
        desc.IsCube = description.Type == ResourceType::TextureCube;
        desc.Format = description.Format;
        desc.ResourceFormat = description.IsDepth() ? DXGI_FORMAT_R32_TYPELESS : description.Format;
        desc.ExternalResource = external;
        desc.CreateSRV = HasUsage(description.Usages, ResourceUsage::PixelShaderRead) || HasUsage(description.Usages, ResourceUsage::NonPixelShaderRead);
        desc.CreateRTV = HasUsage(description.Usages, ResourceUsage::RenderTarget);
        desc.CreateDSV = HasUsage(description.Usages, ResourceUsage::DepthWrite) || HasUsage(description.Usages, ResourceUsage::DepthRead);
        desc.CreateUAV = HasUsage(description.Usages, ResourceUsage::UnorderedAccess);
        desc.ClearValue = description.IsDepth() ? XMFLOAT4{0.f, 0.f, 0.f, 0.f} : XMFLOAT4{0.f, 0.f, 0.f, 1.f};
        if (desc.CreateSRV || desc.CreateUAV) { desc.SRV_UAV_Heap = owner->SRV_UAV_Heap.get(); }
        if (desc.CreateSRV)
        {
            desc.SRVHeapIndex = owner->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
            desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.SRVDesc.Format = description.IsDepth() ? DXGI_FORMAT_R32_FLOAT : description.Format;
            if (description.Type == ResourceType::TextureCube)
            {
                desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                desc.SRVDesc.TextureCube.MipLevels = 1;
            }
            else if (description.Type == ResourceType::Texture2DArray)
            {
                desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                desc.SRVDesc.Texture2DArray.ArraySize = description.ArraySize;
                desc.SRVDesc.Texture2DArray.MipLevels = 1;
            }
            else
            {
                desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                desc.SRVDesc.Texture2D.MipLevels = 1;
            }
        }
        if (desc.CreateRTV)
        {
            desc.RTVHeap = owner->RTVHeap.get();
            desc.RTVHeapIndex = owner->RTVHeap->GetAvailableIndex();
            desc.RTVDesc.Format = description.Format;
            desc.RTVDesc.ViewDimension = description.ArraySize == 1 ? D3D12_RTV_DIMENSION_TEXTURE2D : D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            if (description.ArraySize > 1) { desc.RTVDesc.Texture2DArray.ArraySize = description.ArraySize; }
        }
        if (desc.CreateDSV)
        {
            desc.DSVHeap = owner->DSVHeap.get();
            desc.DSVHeapIndex = owner->DSVHeap->GetAvailableIndex();
            desc.DSVDesc.Format = DXGI_FORMAT_D32_FLOAT;
            desc.DSVDesc.ViewDimension = description.ArraySize == 1 ? D3D12_DSV_DIMENSION_TEXTURE2D : D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            if (description.ArraySize > 1) { desc.DSVDesc.Texture2DArray.ArraySize = description.ArraySize; }
        }
        if (desc.CreateUAV)
        {
            desc.UAVHeapIndex = owner->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
            desc.UAVDesc.Format = description.Format;
            desc.UAVDesc.ViewDimension = description.ArraySize == 1 ? D3D12_UAV_DIMENSION_TEXTURE2D : D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            if (description.ArraySize > 1) { desc.UAVDesc.Texture2DArray.ArraySize = description.ArraySize; }
        }
        return desc;
    }

    std::unique_ptr<GDX12Texture> CreateTexture(GDX12DeviceResources* owner, const ResourceDescription& description,
        ComPtr<ID3D12Resource> external = nullptr)
    {
        auto desc = TextureDescription(owner, description, external);
        // The heap allocator reserves indices before resource creation can fail.
        // Retain a lease until the texture has taken ownership of every descriptor.
        std::vector<std::unique_ptr<GDX12Descriptor>> leases;
        auto lease = [&](GDX12DescriptorHeap* heap, UINT index)
        {
            auto descriptor = std::make_unique<GDX12Descriptor>();
            descriptor->_heap = heap;
            descriptor->HeapIndex = index;
            leases.push_back(std::move(descriptor));
        };
        if (desc.CreateSRV) { lease(desc.SRV_UAV_Heap, desc.SRVHeapIndex); }
        if (desc.CreateUAV) { lease(desc.SRV_UAV_Heap, desc.UAVHeapIndex); }
        if (desc.CreateRTV) { lease(desc.RTVHeap, desc.RTVHeapIndex); }
        if (desc.CreateDSV) { lease(desc.DSVHeap, desc.DSVHeapIndex); }
        auto texture = std::make_unique<GDX12Texture>();
        texture->Initialize(desc);
        for (auto& descriptor : leases) { descriptor->HeapIndex = UINT(-1); }
        return texture;
    }

    void BufferViews(FrameGraphBuffer& buffer, GDX12DeviceResources* owner, uint64_t capacity,
        uint32_t stride, bool raw, ResourceUsage usages)
    {
        const uint64_t elements = raw ? capacity / 4 : capacity;
        if (!elements || elements > (std::numeric_limits<UINT>::max)())
        { throw std::invalid_argument("ERROR: FrameGraph buffer view element count exceeds UINT_MAX. Reduce the view element count to UINT_MAX or "
            "split the buffer into smaller resources."); }
        if (HasUsage(usages, ResourceUsage::GraphicsShaderRead))
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            desc.Format = raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
            desc.Buffer.NumElements = static_cast<UINT>(elements);
            desc.Buffer.StructureByteStride = raw ? 0 : stride;
            desc.Buffer.Flags = raw ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
            buffer.SRV = std::make_unique<GDX12Descriptor>();
            buffer.SRV->InitAsSRV(buffer.Resource.D3DResource.Get(), &desc, owner->SRV_UAV_Heap.get(),
                owner->SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        }
        if (HasUsage(usages, ResourceUsage::UnorderedAccess))
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
            desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            desc.Format = raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
            desc.Buffer.NumElements = static_cast<UINT>(elements);
            desc.Buffer.StructureByteStride = raw ? 0 : stride;
            desc.Buffer.Flags = raw ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE;
            buffer.UAV = std::make_unique<GDX12Descriptor>();
            buffer.UAV->InitAsUAV(buffer.Resource.D3DResource.Get(), nullptr, &desc, owner->SRV_UAV_Heap.get(),
                owner->SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        }
    }

    void Footprints(GDX12Device* device, const ResourceDescription& description,
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>& footprints, uint64_t& bytes)
    {
        const auto native = NativeTextureDescription(description);
        const UINT count = description.ArraySize;
        if (!count || count > D3D12_REQ_SUBRESOURCES) { throw std::invalid_argument("ERROR: Given texture subresource count is zero or exceeds the D3D12 limit. Use a positive layer/mip count "
            "within D3D12_REQ_SUBRESOURCES."); }
        footprints.resize(count);
        std::vector<UINT> rows(count);
        std::vector<UINT64> rowBytes(count);
        device->GetDevice()->GetCopyableFootprints(&native, 0, count, 0, footprints.data(), rows.data(), rowBytes.data(), &bytes);
        if (!bytes || bytes == (std::numeric_limits<UINT64>::max)()) { throw std::runtime_error("ERROR: GetCopyableFootprints() returned an invalid allocation byte count. Use a valid texture description "
            "supported by the executing device before querying copy footprints."); }
        uint64_t lastEnd = 0;
        for (UINT i = 0; i < count; ++i)
        {
            const auto& p = footprints[i];
            if (p.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT ||
                p.Footprint.RowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT ||
                !rows[i] || !rowBytes[i] || rowBytes[i] > p.Footprint.RowPitch || p.Offset < lastEnd)
            { throw std::runtime_error("ERROR: Given texture copy footprint has an invalid alignment or row size. Use device-reported copy footprints "
                "with valid row sizes and D3D12 alignment."); }
            const uint64_t rowSpan = uint64_t(p.Footprint.RowPitch) * (rows[i] - 1);
            lastEnd = AddBytes(p.Offset, AddBytes(rowSpan, rowBytes[i]));
            if (lastEnd > bytes) { throw std::overflow_error("ERROR: Texture copy footprint exceeds the allocated payload buffer. Allocate the full payload byte count "
                "returned by GetCopyableFootprints()."); }
        }
    }

    void ValidateFootprints(GDX12Device* device, const ResourceDescription& description,
        const std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>& canonical, uint64_t bytes)
    {
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> other;
        uint64_t otherBytes = 0;
        Footprints(device, description, other, otherBytes);
        if (otherBytes != bytes || other.size() != canonical.size())
        { throw std::runtime_error("ERROR: Adapters report different total byte counts for the texture copy footprints. Use a texture description "
            "with compatible copy footprints on both adapters or keep the resource local."); }
        for (size_t i = 0; i < canonical.size(); ++i)
        {
            const auto& a = canonical[i];
            const auto& b = other[i];
            if (a.Offset != b.Offset || a.Footprint.Format != b.Footprint.Format || a.Footprint.Width != b.Footprint.Width ||
                a.Footprint.Height != b.Footprint.Height || a.Footprint.Depth != b.Footprint.Depth || a.Footprint.RowPitch != b.Footprint.RowPitch)
            { throw std::runtime_error("ERROR: Adapters report incompatible texture copy footprints. Use matching canonical copy footprints on both "
                "adapters or keep the resource local."); }
        }
    }

    uint64_t AllocationSize(GDX12Device* device, const D3D12_RESOURCE_DESC& desc)
    {
        const auto info = device->GetDevice()->GetResourceAllocationInfo(0, 1, &desc);
        if (!info.SizeInBytes || info.SizeInBytes == (std::numeric_limits<UINT64>::max)() ||
            !info.Alignment || (info.Alignment & (info.Alignment - 1)))
        { throw std::runtime_error("ERROR: Device rejected the FrameGraph resource allocation description. Use supported resource dimensions, "
            "format, flags, and alignment for the allocation device."); }
        return info.SizeInBytes;
    }

    GDX12Texture* ResolveTexture(FrameGraphPhysicalResource& resource)
    { return resource.ImportedTexture ? resource.ImportedTexture : resource.Texture.get(); }

    void CheckOwner(GDX12DeviceResources* owner)
    {
        if (!owner || !owner->Device || !owner->Device->IsInitialized())
        { throw std::invalid_argument("ERROR: FrameGraph resource creation requires initialized device resources. Initialize the device and its "
            "descriptor heaps before creating FrameGraph resources."); }
    }
}

PassBinding FrameGraphPhysicalResource::CreatePassBinding(ResourceUsage usages)
{
    if (!Footprints.empty()) { throw std::logic_error("ERROR: Texture-footprint payload cannot be used as a pass binding. Bind a native texture or buffer to the "
        "pass and use footprint payloads only for transport."); }
    PassBinding result;
    result.Resource = this;
    if (!Description.IsBuffer())
    {
        auto* texture = ImportedTexture ? ImportedTexture : Texture.get();
        result.Texture.Value = texture;
        result.Texture.Description = &Description;
        result.Texture.Usages = usages;
    }
    else if (Description.Type == ResourceType::DrawStream)
    {
        if (Buffers.size() != 2) { throw std::logic_error("ERROR: DrawStream binding does not contain both argument and count buffers. Create both the indirect-argument "
            "buffer and its draw-count buffer."); }
        result.Draws.Arguments = &Buffers[0].Resource;
        result.Draws.Count = &Buffers[1].Resource;
        result.Draws.ArgumentUAV = HasUsage(usages, ResourceUsage::UnorderedAccess) ? Buffers[0].UAV.get() : nullptr;
        result.Draws.CountUAV = HasUsage(usages, ResourceUsage::UnorderedAccess) ? Buffers[1].UAV.get() : nullptr;
        result.Draws.Capacity = static_cast<uint32_t>(Description.Capacity);
    }
    else
    {
        if (Buffers.size() != 1) { throw std::logic_error("ERROR: GPU buffer binding does not contain exactly one native buffer. Provide exactly one native buffer for a "
            "buffer port."); }
        result.Buffer.Resource = &Buffers[0].Resource;
        result.Buffer.ShaderView = HasUsage(usages, ResourceUsage::GraphicsShaderRead)
            ? Buffers[0].SRV.get() : nullptr;
        result.Buffer.UnorderedView = HasUsage(usages, ResourceUsage::UnorderedAccess) ? Buffers[0].UAV.get() : nullptr;
    }
    return result;
}

std::unique_ptr<FrameGraphPhysicalResource> CreateFrameGraphNative(GDX12DeviceResources* owner, const ResourceDescription& description)
{
    CheckOwner(owner);
    auto result = std::make_unique<FrameGraphPhysicalResource>();
    result->DeviceResources = owner;
    result->Description = description;
    if (!description.IsBuffer())
    {
        result->Texture = CreateTexture(owner, description);
        result->AllocationBytes = AllocationSize(owner->Device, result->Texture->GetResource()->D3DResource->GetDesc());
        return result;
    }
    const size_t components = description.Type == ResourceType::DrawStream ? 2 : 1;
    result->Buffers.resize(components);
    for (size_t i = 0; i < components; ++i)
    {
        const bool raw = description.Type == ResourceType::RawBuffer;
        const uint64_t capacity = i == 1 ? 1 : description.Capacity;
        const uint32_t stride = i == 1 ? sizeof(uint32_t) : description.Stride;
        const uint64_t bytes = BufferBytes(capacity, stride, raw);
        const auto flags = HasUsage(description.Usages, ResourceUsage::UnorderedAccess)
            ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes, flags);
        const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(owner->Device->GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result->Buffers[i].Resource.D3DResource)));
        result->Buffers[i].Resource.ResetState();
        result->AllocationBytes = AddBytes(result->AllocationBytes, AllocationSize(owner->Device, desc));
        BufferViews(result->Buffers[i], owner, capacity, stride, raw, description.Usages);
    }
    return result;
}

std::unique_ptr<FrameGraphPhysicalResource> WrapFrameGraphImport(GDX12DeviceResources* owner, GDX12Texture* texture,
    const ResourceDescription& description)
{
    CheckOwner(owner);
    if (!texture || !texture->IsInitialized() || texture->GetDevice() != owner->Device)
    { throw std::invalid_argument("ERROR: Imported FrameGraph texture is missing or belongs to another device. Return an initialized texture "
        "owned by the device registered for this import."); }
    const auto actual = DescribeFrameGraphTexture(texture);
    if (actual.Type != description.Type || actual.IsDepth() != description.IsDepth() || actual.Format != description.Format || actual.GetSize() != description.GetSize() ||
        actual.ArraySize != description.ArraySize)
    { throw std::invalid_argument("ERROR: Imported FrameGraph texture allocation or description differs from the compiled contract. Rebuild the "
        "frame resources with matching imported targets, or compile a new graph for a changed type or format."); }
    if (((HasUsage(description.Usages, ResourceUsage::PixelShaderRead) || HasUsage(description.Usages, ResourceUsage::NonPixelShaderRead)) && !texture->HasSRV()) ||
        (HasUsage(description.Usages, ResourceUsage::RenderTarget) && !texture->HasRTV()) ||
        ((HasUsage(description.Usages, ResourceUsage::DepthRead) || HasUsage(description.Usages, ResourceUsage::DepthWrite)) && !texture->HasDSV()) ||
        (HasUsage(description.Usages, ResourceUsage::UnorderedAccess) && !texture->HasUAV()))
    { throw std::invalid_argument("ERROR: Imported FrameGraph texture is missing one or more declared views. Create the texture with the SRV, "
        "RTV, DSV, or UAV views required by its connected ports."); }
    auto result = std::make_unique<FrameGraphPhysicalResource>();
    result->DeviceResources = owner;
    result->Description = description;
    result->ImportedTexture = texture;
    result->AllocationBytes = AllocationSize(owner->Device, texture->GetResource()->D3DResource->GetDesc());
    return result;
}

FrameGraphPhysicalResource& FrameGraphSharedResources::GetDeviceMapping(GDX12Device* device) const
{
    if (Producer && Producer->DeviceResources->Device == device) { return *Producer; }
    if (Consumer && Consumer->DeviceResources->Device == device) { return *Consumer; }
    throw std::invalid_argument("ERROR: Given device has no mapping for this shared resource. Request the producer or consumer device mapping "
        "created for this shared resource.");
}

HRESULT TryCreateFrameGraphShared(GDX12DeviceResources* producer, GDX12DeviceResources* other,
    const ResourceDescription& description, bool bufferPayload, FrameGraphSharedResources& result)
{
    CheckOwner(producer);
    CheckOwner(other);
    FrameGraphSharedResources created;
    if (bufferPayload && description.IsBuffer()) { throw std::invalid_argument("ERROR: GPU buffer transport was configured with a texture-footprint payload. Transport GPU buffers directly "
        "without enabling a texture-footprint payload."); }
    if (!description.IsBuffer() && !bufferPayload &&
        (description.Type != ResourceType::Texture2D || description.ArraySize != 1 || description.IsDepth()))
    { throw std::invalid_argument("ERROR: Direct ROW_MAJOR texture binding is unsupported for this texture type or depth format. Use a color "
        "Texture2D or select Local storage/access so FrameGraph can stage the texture through a buffer."); }
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    uint64_t payloadBytes = 0;
    if (bufferPayload)
    {
        Footprints(producer->Device, description, footprints, payloadBytes);
        ValidateFootprints(other->Device, description, footprints, payloadBytes);
    }
    created.Producer = std::make_unique<FrameGraphPhysicalResource>();
    created.Producer->DeviceResources = producer;
    if (other->Device != producer->Device)
    {
        created.Consumer = std::make_unique<FrameGraphPhysicalResource>();
        created.Consumer->DeviceResources = other;
    }
    for (auto* mapping : {created.Producer.get(), created.Consumer.get()})
    {
        if (!mapping) { continue; }
        mapping->Description = description;
        mapping->Footprints = footprints;
    }
    const size_t components = description.Type == ResourceType::DrawStream ? 2 : 1;
    for (size_t i = 0; i < components; ++i)
    {
        D3D12_RESOURCE_DESC desc;
        const bool raw = description.Type == ResourceType::RawBuffer;
        const uint64_t capacity = i == 1 ? 1 : description.Capacity;
        const uint32_t stride = i == 1 ? sizeof(uint32_t) : description.Stride;
        if (bufferPayload) { desc = CD3DX12_RESOURCE_DESC::Buffer(payloadBytes); }
        else if (description.IsBuffer())
        {
            const auto flags = HasUsage(description.Usages, ResourceUsage::UnorderedAccess)
                ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
            desc = CD3DX12_RESOURCE_DESC::Buffer(BufferBytes(capacity, stride, raw), flags);
        }
        else
        {
            desc = NativeTextureDescription(description);
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        }
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        auto allocation = std::make_unique<GDX12SharedAllocation>();
        const HRESULT initialized = allocation->Initialize(producer->Device, other->Device, desc);
        if (FAILED(initialized)) { return initialized; }
        for (auto* mapping : {created.Producer.get(), created.Consumer.get()})
        {
            if (!mapping) { continue; }
            auto* resource = allocation->GetResource(mapping->DeviceResources->Device);
            mapping->AllocationBytes = AddBytes(mapping->AllocationBytes, allocation->GetHeapSize());
            if (bufferPayload || description.IsBuffer())
            {
                mapping->Buffers.emplace_back();
                auto& buffer = mapping->Buffers.back();
                buffer.Resource.D3DResource = resource->D3DResource;
                if (!bufferPayload) { BufferViews(buffer, mapping->DeviceResources, capacity, stride, raw, description.Usages); }
            }
            else
            {
                mapping->Texture = CreateTexture(mapping->DeviceResources, description, resource->D3DResource);
            }
        }
        created.Allocations.push_back(std::move(allocation));
    }
    result = std::move(created);
    return S_OK;
}

void TransitionFrameGraphResource(GDX12CommandList* commands, FrameGraphPhysicalResource& resource, D3D12_RESOURCE_STATES state)
{
    if (auto* texture = ResolveTexture(resource)) { commands->ResourceBarrier({texture->GetResource()->GetTransitionBarrier(state)}); }
    else
    {
        for (auto& buffer : resource.Buffers) { commands->ResourceBarrier({buffer.Resource.GetTransitionBarrier(state)}); }
    }
}

void FrameGraphUAVBarrier(GDX12CommandList* commands, FrameGraphPhysicalResource& resource)
{
    if (auto* texture = ResolveTexture(resource)) { commands->ResourceBarrier({texture->GetResource()->GetUAVBarrier()}); }
    else
    {
        for (auto& buffer : resource.Buffers) { commands->ResourceBarrier({buffer.Resource.GetUAVBarrier()}); }
    }
}

FrameGraphCopyStatistics CopyFrameGraphResource(GDX12CommandList* commands, FrameGraphPhysicalResource& source,
    FrameGraphPhysicalResource& target)
{
    if (source.DeviceResources->Device != target.DeviceResources->Device)
    { throw std::invalid_argument("ERROR: Copy source or destination mapping belongs to another device. Use source and destination resource "
        "mappings owned by the device executing the copy."); }
    const auto& a = source.Description;
    const auto& b = target.Description;
    if (a.Type != b.Type || a.Format != b.Format || a.GetSize() != b.GetSize() || a.ArraySize != b.ArraySize ||
        a.Capacity != b.Capacity || a.Stride != b.Stride || a.Layout != b.Layout)
    { throw std::invalid_argument("ERROR: FrameGraph transport source and destination resource descriptions differ. Match the source and "
        "destination resource types, formats, dimensions, layers, capacities, and strides."); }
    FrameGraphCopyStatistics result;
    auto* sourceTexture = ResolveTexture(source);
    auto* targetTexture = ResolveTexture(target);
    if (sourceTexture && targetTexture)
    {
        const auto src = sourceTexture->GetResource()->D3DResource;
        const auto dst = targetTexture->GetResource()->D3DResource;
        if (src.Get() == dst.Get()) { return result; }
        commands->CopyResource(dst.Get(), src.Get());
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
        Footprints(source.DeviceResources->Device, a, footprints, result.Bytes);
        result.Commands = 1;
        return result;
    }
    if (sourceTexture || targetTexture)
    {
        auto& payload = sourceTexture ? target : source;
        auto* native = sourceTexture ? sourceTexture : targetTexture;
        if (payload.Footprints.empty() || payload.Buffers.size() != 1 || payload.Footprints.size() != a.ArraySize)
        { throw std::invalid_argument("ERROR: Texture transport has no complete canonical whole-resource copy footprints. Create whole-resource copy "
            "footprints for every texture subresource before transferring it."); }
        for (UINT i = 0; i < payload.Footprints.size(); ++i)
        {
            D3D12_TEXTURE_COPY_LOCATION textureLocation = {};
            textureLocation.pResource = native->GetResource()->D3DResource.Get();
            textureLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            textureLocation.SubresourceIndex = i;
            D3D12_TEXTURE_COPY_LOCATION bufferLocation = {};
            bufferLocation.pResource = payload.Buffers[0].Resource.D3DResource.Get();
            bufferLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            bufferLocation.PlacedFootprint = payload.Footprints[i];
            const auto* src = sourceTexture ? &textureLocation : &bufferLocation;
            const auto* dst = sourceTexture ? &bufferLocation : &textureLocation;
            commands->GetCommandList()->CopyTextureRegion(dst, 0, 0, 0, src, nullptr);
            ++result.Commands;
        }
        result.Bytes = payload.Buffers[0].Resource.D3DResource->GetDesc().Width;
        return result;
    }
    if (source.Buffers.size() != target.Buffers.size()) { throw std::invalid_argument("ERROR: GPU transport source and destination component counts differ. Match the source and destination "
        "component counts, including both DrawStream buffers."); }
    for (size_t i = 0; i < source.Buffers.size(); ++i)
    {
        auto* src = source.Buffers[i].Resource.D3DResource.Get();
        auto* dst = target.Buffers[i].Resource.D3DResource.Get();
        if (src == dst) { continue; }
        if (src->GetDesc().Width != dst->GetDesc().Width) { throw std::invalid_argument("ERROR: GPU transport source and destination buffer byte sizes differ. Allocate source and destination "
            "transport buffers with identical byte sizes."); }
        commands->CopyResource(dst, src);
        result.Bytes = AddBytes(result.Bytes, src->GetDesc().Width);
        ++result.Commands;
    }
    return result;
}

PortCapabilities QueryFrameGraphCapabilities(GDX12Device* device, const PortDescription& schema)
{
    if (!device || !device->IsInitialized()) { return {false, false}; }
    PortCapabilities result{schema.AllowLocal, schema.AllowShared};
    const bool buffer = schema.Type == ResourceType::RawBuffer || schema.Type == ResourceType::StructuredBuffer || schema.Type == ResourceType::DrawStream;
    if (buffer) { return result; }
    if (schema.IsDepth() || schema.Type != ResourceType::Texture2D) { result.Shared = false; }
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    if (FAILED(device->GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) ||
        !options.CrossAdapterRowMajorTextureSupported) { result.Shared = false; }
    bool anySupportedFormat = schema.Formats.empty();
    for (auto format : schema.Formats)
    {
        if (schema.IsDepth() && format == DXGI_FORMAT_D32_FLOAT &&
            !HasUsage(schema.Usages, ResourceUsage::DepthWrite) && !HasUsage(schema.Usages, ResourceUsage::DepthRead))
        { format = DXGI_FORMAT_R32_FLOAT; }
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {format};
        bool supported = SUCCEEDED(device->GetDevice()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
        supported &= (support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) != 0;
        if (HasUsage(schema.Usages, ResourceUsage::RenderTarget)) { supported &= (support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) != 0; }
        if (HasUsage(schema.Usages, ResourceUsage::DepthWrite) || HasUsage(schema.Usages, ResourceUsage::DepthRead))
        { supported &= (support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) != 0; }
        if (HasUsage(schema.Usages, ResourceUsage::PixelShaderRead) || HasUsage(schema.Usages, ResourceUsage::NonPixelShaderRead))
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT shader = {schema.IsDepth() ? DXGI_FORMAT_R32_FLOAT : format};
            supported &= SUCCEEDED(device->GetDevice()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &shader, sizeof(shader)));
            supported &= (shader.Support1 & (D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE)) != 0;
        }
        if (HasUsage(schema.Usages, ResourceUsage::UnorderedAccess))
        {
            supported &= (support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0;
            supported &= (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
        }
        anySupportedFormat |= supported;
    }
    if (!anySupportedFormat) { result.Local = result.Shared = false; }
    return result;
}

ResourceDescription DescribeFrameGraphTexture(GDX12Texture* texture)
{
    if (!texture || !texture->IsInitialized() || !texture->GetResource() || !texture->GetResource()->D3DResource)
    { throw std::invalid_argument("ERROR: Given FrameGraph texture is missing or has no native allocation. Provide an initialized GDX12Texture."); }
    const auto& wrapper = texture->GetDescription();
    const auto actual = texture->GetResource()->D3DResource->GetDesc();
    if (actual.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || actual.MipLevels != 1 || actual.SampleDesc.Count != 1)
    { throw std::invalid_argument("ERROR: Given imported texture allocation is not Texture2D, has multiple mips, or uses MSAA. Import a "
        "two-dimensional allocation with one mip and one sample."); }
    ResourceDescription result;
    result.Type = wrapper.IsCube ? ResourceType::TextureCube : actual.DepthOrArraySize > 1 ? ResourceType::Texture2DArray : ResourceType::Texture2D;
    result.Format = wrapper.Format;
    result.Width = static_cast<uint32_t>(actual.Width);
    result.Height = actual.Height;
    result.ArraySize = actual.DepthOrArraySize;
    if (wrapper.CreateSRV) { result.Usages |= ResourceUsage::PixelShaderRead | ResourceUsage::NonPixelShaderRead; }
    if (wrapper.CreateRTV) { result.Usages |= ResourceUsage::RenderTarget; }
    if (wrapper.CreateDSV) { result.Usages |= ResourceUsage::DepthWrite | ResourceUsage::DepthRead; }
    if (wrapper.CreateUAV) { result.Usages |= ResourceUsage::UnorderedAccess; }
    return result;
}
