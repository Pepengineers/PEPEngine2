#pragma once

#include "Engine.RendererDX12/FrameGraph/FrameGraphCompiler.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12Resource.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"

class RenderPipelineCommonData;
struct FrameGraphPhysicalResource;

struct ExternalResourceUse
{
    GDX12Resource* Resource = nullptr;
    D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;
    GDX12CompletionPoint Ready;
};

struct TextureBinding
{
    GDX12Texture* Value = nullptr;
    const ResourceDescription* Description = nullptr;
    ResourceUsage Usages = ResourceUsage::None;
    GDX12Texture* GetTexture() const
    {
        if (!Value) { throw std::logic_error("ERROR: Given texture binding is empty. Connect a texture output to the required input before executing the "
            "pass."); }
        return Value;
    }
    const ResourceDescription& GetDescription() const { GetTexture(); return *Description; }
    GDX12Descriptor* GetSRV() const { ValidateViewUsage(ResourceUsage::PixelShaderRead | ResourceUsage::NonPixelShaderRead); return GetTexture()->GetSRV(); }
    GDX12Descriptor* GetRTV(uint32_t layer = 0) const { ValidateViewUsage(ResourceUsage::RenderTarget); return GetDescription().ArraySize == 1 ? GetTexture()->GetRTV() : GetTexture()->GetRTV(layer); }
    GDX12Descriptor* GetDSV(uint32_t layer = 0) const
    {
        ValidateViewUsage(ResourceUsage::DepthWrite | ResourceUsage::DepthRead);
        if (HasUsage(Usages, ResourceUsage::DepthRead) && !HasUsage(Usages, ResourceUsage::DepthWrite))
        { return GetTexture()->GetReadOnlyDSV(layer); }
        return GetDescription().ArraySize == 1 ? GetTexture()->GetDSV() : GetTexture()->GetDSV(layer);
    }
    GDX12Descriptor* GetUAV() const { ValidateViewUsage(ResourceUsage::UnorderedAccess); return GetTexture()->GetUAV(); }
    explicit operator bool() const { return Value != nullptr; }
private:
    void ValidateViewUsage(ResourceUsage usage) const { if (!HasUsage(Usages, usage)) { throw std::logic_error("ERROR: Requested texture view usage was not declared. Add the requested view usage to the texture port in "
        "Describe()."); } }
};

struct DrawStreamBinding
{
    GDX12Resource* Arguments = nullptr;
    GDX12Resource* Count = nullptr;
    GDX12Descriptor* ArgumentUAV = nullptr;
    GDX12Descriptor* CountUAV = nullptr;
    uint32_t Capacity = 0;
};

struct BufferBinding
{
    GDX12Resource* Resource = nullptr;
    GDX12Descriptor* ShaderView = nullptr;
    GDX12Descriptor* UnorderedView = nullptr;
    GDX12Descriptor* GetSRV() const { if (!ShaderView) { throw std::logic_error("ERROR: Requested buffer SRV was not declared. Declare a shader-read usage for this buffer port in Describe()."); } return ShaderView; }
    GDX12Descriptor* GetUAV() const { if (!UnorderedView) { throw std::logic_error("ERROR: Requested buffer UAV was not declared. Declare ResourceUsage::UnorderedAccess for this buffer port in "
        "Describe()."); } return UnorderedView; }
};

struct PassBinding
{
    FrameGraphPhysicalResource* Resource = nullptr;
    TextureBinding Texture;
    DrawStreamBinding Draws;
    BufferBinding Buffer;
};

class PassInitContext
{
public:
    PassInitContext(GDX12DeviceResources* resources, const RenderPipelineCommonData* common,
        const NodeDefinition& node, const FrameGraphPlan& plan, const std::map<PinAddress, PinAddress>& sources)
        : _resources(resources), _common(common), _node(node), _plan(plan), _sources(sources) {}
    GDX12Device* GetDevice() const { return _resources->Device; }
    GDX12DeviceResources* GetDeviceResources() const { return _resources; }
    const RenderPipelineCommonData* GetFrameData() const { return _common; }
    const ResourceDescription& GetInputResourceDescription(const PinId& id) const
    {
        _node.Description.GetInputDescription(id);
        return _plan.Resources.at(_sources.at({_node.Id, id})).Description;
    }
    const ResourceDescription& GetOutputResourceDescription(const PinId& id) const
    {
        _node.Description.GetOutputDescription(id);
        return _plan.Resources.at({_node.Id, id}).Description;
    }
protected:
    GDX12DeviceResources* _resources;
    const RenderPipelineCommonData* _common;
    const NodeDefinition& _node;
    const FrameGraphPlan& _plan;
    const std::map<PinAddress, PinAddress>& _sources;
};

class PassExecutionContext : public PassInitContext
{
public:
    PassExecutionContext(const PassInitContext& init, GDX12CommandList* commands, uint64_t frameId, uint32_t frameSlotIndex,
        const std::map<PinId, PassBinding>& bindings)
        : PassInitContext(init), _commands(commands), _frameId(frameId), _frameSlotIndex(frameSlotIndex), _bindings(bindings) {}
    GDX12CommandList* GetCommandList() const { return _commands; }
    uint64_t GetFrameId() const { return _frameId; }
    uint32_t GetFrameSlotResourcesIndex() const { return _frameSlotIndex; }
    const TextureBinding& GetInputTexture(const PinId& id) const
    {
        ValidateTexturePort(id, _node.Description.GetInputDescription(id));
        return GetBinding(id).Texture;
    }
    const TextureBinding& GetOutputTexture(const PinId& id) const
    {
        ValidateTexturePort(id, _node.Description.GetOutputDescription(id));
        return GetBinding(id).Texture;
    }
    const BufferBinding& GetInputBuffer(const PinId& id) const
    {
        ValidateBufferPort(id, _node.Description.GetInputDescription(id));
        return GetBinding(id).Buffer;
    }
    const BufferBinding& GetOutputBuffer(const PinId& id) const
    {
        ValidateBufferPort(id, _node.Description.GetOutputDescription(id));
        return GetBinding(id).Buffer;
    }
    const DrawStreamBinding& GetInputDrawStream(const PinId& id) const
    {
        ValidateDrawStreamPort(id, _node.Description.GetInputDescription(id));
        return GetBinding(id).Draws;
    }
    const DrawStreamBinding& GetOutputDrawStream(const PinId& id) const
    {
        ValidateDrawStreamPort(id, _node.Description.GetOutputDescription(id));
        return GetBinding(id).Draws;
    }
    const TextureBinding* TryGetInputTexture(const PinId& id) const
    {
        ValidateTexturePort(id, _node.Description.GetInputDescription(id));
        auto* binding = FindBinding(id);
        return binding ? &binding->Texture : nullptr;
    }
    const BufferBinding* TryGetInputBuffer(const PinId& id) const
    {
        ValidateBufferPort(id, _node.Description.GetInputDescription(id));
        auto* binding = FindBinding(id);
        return binding ? &binding->Buffer : nullptr;
    }
    const DrawStreamBinding* TryGetInputDrawStream(const PinId& id) const
    {
        ValidateDrawStreamPort(id, _node.Description.GetInputDescription(id));
        auto* binding = FindBinding(id);
        return binding ? &binding->Draws : nullptr;
    }
    GDX12Descriptor* GetOutputRTV(const PinId& id, uint32_t layer = 0) const { return GetOutputTexture(id).GetRTV(layer); }
    void TransitionTextureState(const TextureBinding& binding, D3D12_RESOURCE_STATES state) const
    { _commands->ResourceBarrier({binding.GetTexture()->GetResource()->GetTransitionBarrier(state)}); }
private:
    void ThrowBindingTypeError(const PinId& id, ResourceType actual, const char* expected) const
    {
        throw std::invalid_argument("ERROR: " + _node.Id + "." + id + ": accessor expects " + expected
            + ", but the port declares " + GetResourceTypeName(actual) + ". Use an accessor matching the declared resource type.");
    }
    void ValidateTexturePort(const PinId& id, const PortDescription& port) const
    { if (!IsTexture(port.Type)) { ThrowBindingTypeError(id, port.Type, "a texture"); } }
    void ValidateBufferPort(const PinId& id, const PortDescription& port) const
    { if (port.Type != ResourceType::RawBuffer && port.Type != ResourceType::StructuredBuffer) { ThrowBindingTypeError(id, port.Type, "a raw or structured buffer"); } }
    void ValidateDrawStreamPort(const PinId& id, const PortDescription& port) const
    { if (port.Type != ResourceType::DrawStream) { ThrowBindingTypeError(id, port.Type, "DrawStream"); } }
    const PassBinding* FindBinding(const PinId& id) const
    {
        auto binding = _bindings.find(id);
        return binding == _bindings.end() ? nullptr : &binding->second;
    }
    const PassBinding& GetBinding(const PinId& id) const
    {
        auto* binding = FindBinding(id);
        if (!binding)
        { throw std::logic_error("ERROR: " + _node.Id + "." + id + ": required resource binding is missing. Connect a compatible resource before compiling the graph."); }
        return *binding;
    }
    GDX12CommandList* _commands;
    uint64_t _frameId;
    uint32_t _frameSlotIndex;
    const std::map<PinId, PassBinding>& _bindings;
};

class PassResourceRequirementsContext
{
public:
    PassResourceRequirementsContext(GDX12DeviceResources* resources, const RenderPipelineCommonData* common,
        NodeDefinition& node, FrameGraphCompileData& data)
        : _resources(resources), _common(common), _node(node), _compileData(data) {}
    GDX12Device* GetDevice() const { return _resources->Device; }
    GDX12DeviceResources* GetDeviceResources() const { return _resources; }
    const RenderPipelineCommonData* GetFrameData() const { return _common; }
    UInt2 GetDisplaySize() const { return _compileData.DisplaySize; }
    void RequireTextureSize(const PinId& id, UInt2 size)
    { _node.Description.GetPortDescription(id); _compileData.Ports.at({_node.Id, id}).RequiredTextureSize = size; }
    void SetOutputBufferCapacity(const PinId& id, uint64_t capacity)
    {
        auto output = _node.Description.Outputs.find(id);
        if (output == _node.Description.Outputs.end() || IsTexture(output->second.Type) || !capacity)
        { throw std::invalid_argument("ERROR: " + _node.Id + "." + id + ": buffer capacity targets a texture, an undeclared output, or a zero capacity. Call SetOutputBufferCapacity() "
            "for a buffer output with a capacity greater than zero."); }
        output->second.Capacity = capacity;
    }
private:
    GDX12DeviceResources* _resources;
    const RenderPipelineCommonData* _common;
    NodeDefinition& _node;
    FrameGraphCompileData& _compileData;
};
