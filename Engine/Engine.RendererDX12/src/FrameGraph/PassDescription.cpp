#include "Engine.RendererDX12/FrameGraph/PassDescription.h"

#include <algorithm>
#include <set>

const char* GetMemoryTypeName(MemoryType type)
{
    switch (type)
    {
    case MemoryType::Auto: return "Auto";
    case MemoryType::Local: return "Local";
    case MemoryType::Shared: return "Shared";
    default: return "Unknown";
    }
}

const char* GetResourceTypeName(ResourceType type)
{
    switch (type)
    {
    case ResourceType::Texture2D: return "Texture2D";
    case ResourceType::Texture2DArray: return "Texture2DArray";
    case ResourceType::TextureCube: return "TextureCube";
    case ResourceType::RawBuffer: return "RawBuffer";
    case ResourceType::StructuredBuffer: return "StructuredBuffer";
    case ResourceType::DrawStream: return "DrawStream";
    default: return "Unknown";
    }
}

std::string GetTextureFormatName(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_UNKNOWN: return "DXGI_FORMAT_UNKNOWN";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "DXGI_FORMAT_R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "DXGI_FORMAT_B8G8R8A8_UNORM";
    case DXGI_FORMAT_R16_FLOAT: return "DXGI_FORMAT_R16_FLOAT";
    case DXGI_FORMAT_R16G16_FLOAT: return "DXGI_FORMAT_R16G16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "DXGI_FORMAT_R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R32_FLOAT: return "DXGI_FORMAT_R32_FLOAT";
    case DXGI_FORMAT_R32_UINT: return "DXGI_FORMAT_R32_UINT";
    case DXGI_FORMAT_D32_FLOAT: return "DXGI_FORMAT_D32_FLOAT";
    default: return "DXGI_FORMAT(" + std::to_string(uint32_t(format)) + ")";
    }
}

std::string FormatTextureSize(UInt2 size)
{ return std::to_string(size.x) + "x" + std::to_string(size.y); }

PortDescription& PassDescription::AddPort(PinId id, bool input, ResourceType type)
{
    if (id.empty() || Inputs.count(id) || Outputs.count(id))
    { throw std::invalid_argument("ERROR: Port name is empty or already declared: " + id + ". Use a non-empty port name that is unique across the pass inputs and outputs."); }
    PortDescription port;
    port.Type = type;
    port.Access = input ? DataAccess::ReadOnly : DataAccess::WriteOnly;
    if (type == ResourceType::TextureCube) { port.Layers = 6; }
    if (type == ResourceType::DrawStream)
    {
        port.Stride = DrawStreamStride;
        port.Layout = "pepe.indirect.draw-indexed.v1";
    }
    return (input ? Inputs : Outputs).emplace(std::move(id), std::move(port)).first->second;
}

PortDescription& PassDescription::AddInputTexture(PinId id, ResourceType type)
{
    if (!IsTexture(type)) { throw std::invalid_argument("ERROR: AddInputTexture() received a resource type that is not a texture. Pass ResourceType::Texture2D, "
        "ResourceType::Texture2DArray, or ResourceType::TextureCube."); }
    return AddPort(std::move(id), true, type);
}
PortDescription& PassDescription::AddOutputTexture(PinId id, ResourceType type)
{
    if (!IsTexture(type)) { throw std::invalid_argument("ERROR: AddOutputTexture() received a resource type that is not a texture. Pass ResourceType::Texture2D, "
        "ResourceType::Texture2DArray, or ResourceType::TextureCube."); }
    return AddPort(std::move(id), false, type);
}
PortDescription& PassDescription::AddInputDrawStream(PinId id) { return AddPort(std::move(id), true, ResourceType::DrawStream); }
PortDescription& PassDescription::AddOutputDrawStream(PinId id) { return AddPort(std::move(id), false, ResourceType::DrawStream); }
PortDescription& PassDescription::AddInputBuffer(PinId id, ResourceType type)
{
    if (type != ResourceType::RawBuffer && type != ResourceType::StructuredBuffer) { throw std::invalid_argument("ERROR: AddInputBuffer() received a resource type that is not a buffer. Pass ResourceType::RawBuffer or "
        "ResourceType::StructuredBuffer to AddInputBuffer()."); }
    return AddPort(std::move(id), true, type);
}
PortDescription& PassDescription::AddOutputBuffer(PinId id, ResourceType type)
{
    if (type != ResourceType::RawBuffer && type != ResourceType::StructuredBuffer) { throw std::invalid_argument("ERROR: AddOutputBuffer() received a resource type that is not a buffer. Pass ResourceType::RawBuffer or "
        "ResourceType::StructuredBuffer to AddOutputBuffer()."); }
    return AddPort(std::move(id), false, type);
}

const PortDescription& PassDescription::GetPortDescription(const PinId& id) const
{
    auto input = Inputs.find(id);
    if (input != Inputs.end()) { return input->second; }
    auto output = Outputs.find(id);
    if (output != Outputs.end()) { return output->second; }
    throw std::invalid_argument("ERROR: Port name is not declared: " + id + ". Declare the port before requesting its description or use an existing port name.");
}

const PortDescription& PassDescription::GetInputDescription(const PinId& id) const
{
    auto port = Inputs.find(id);
    if (port == Inputs.end())
    { throw std::invalid_argument("ERROR: Input port is not declared: " + id + ". Declare the input in Describe() or use an existing input name."); }
    return port->second;
}

const PortDescription& PassDescription::GetOutputDescription(const PinId& id) const
{
    auto port = Outputs.find(id);
    if (port == Outputs.end())
    { throw std::invalid_argument("ERROR: Output port is not declared: " + id + ". Declare the output in Describe() or use an existing output name."); }
    return port->second;
}

void PassDescription::ValidatePorts(const NodeId& node) const
{
    std::set<PinId> aliasInputs;
    for (const auto* ports : {&Inputs, &Outputs})
    {
        for (const auto& entry : *ports)
        {
            const auto& port = entry.second;
            const auto label = node + "." + entry.first;
            if (entry.first.empty() || (ports == &Inputs && Outputs.count(entry.first)))
            { throw std::invalid_argument("ERROR: " + label + ": port name is empty or duplicated. Use non-empty port names that are unique across the pass inputs and "
                "outputs."); }
            if (port.Usages == ResourceUsage::None || (!port.AllowLocal && !port.AllowShared))
            { throw std::invalid_argument("ERROR: " + label + ": port declares no resource usage or no supported storage/access mode. Declare resource Usages and enable "
                "AllowLocal or AllowShared for this port."); }
            const auto writes = ResourceUsage::RenderTarget | ResourceUsage::DepthWrite
                | ResourceUsage::UnorderedAccess | ResourceUsage::CopyDest;
            if (port.Access == DataAccess::ReadOnly && HasUsage(port.Usages, writes))
            { throw std::invalid_argument("ERROR: " + label + ": ReadOnly input declares a write usage. Remove write usages or declare the input as DataAccess::ReadWrite or "
                "DataAccess::WriteOnly."); }
            if (!port.AliasInput.empty())
            {
                auto input = Inputs.find(port.AliasInput);
                if (ports != &Outputs || input == Inputs.end() || input->second.Type != port.Type
                    || input->second.Access == DataAccess::ReadOnly)
                { throw std::invalid_argument("ERROR: " + label + ": AliasInput does not name a compatible writable input. Set AliasInput on an output to a compatible input "
                    "with writable DataAccess."); }
                if (!aliasInputs.insert(port.AliasInput).second)
                { throw std::invalid_argument("ERROR: " + label + ": more than one alias output refers to the same writable input. Publish exactly one alias output for each "
                    "mutable input."); }
            }
            if (port.Size.Type == Extent::Source::Input && !Inputs.count(port.Size.Input))
            { throw std::invalid_argument("ERROR: " + label + ": Extent::SameAs() refers to an undeclared input. Pass the name of a declared input to Extent::SameAs()."); }
            if (port.Type == ResourceType::DrawStream && (!port.Stride || port.Layout.empty()))
            { throw std::invalid_argument("ERROR: " + label + ": DrawStream argument/count ABI layout or stride is missing. Use AddInputDrawStream()/AddOutputDrawStream() "
                "to declare the argument/count ABI and DrawStreamStride."); }
            for (auto format : port.Formats)
            {
                if (format == DXGI_FORMAT_UNKNOWN || IsDepthFormat(format) != port.IsDepth()
                    || (IsDepthFormat(format) && format != DXGI_FORMAT_D32_FLOAT))
                { throw std::invalid_argument("ERROR: " + label + ": texture format list contains unsupported or incompatible format families. Declare one compatible format "
                    "family; use DXGI_FORMAT_D32_FLOAT for depth ports."); }
            }
        }
    }
    for (const auto& group : SameTextureSizeGroups)
    {
        if (group.size() < 2) { throw std::invalid_argument("ERROR: " + node + ": RequireSameTextureSize() contains fewer than two ports. Pass at least two declared texture-port names to "
            "RequireSameTextureSize()."); }
        for (const auto& id : group) { GetPortDescription(id); }
    }
}
