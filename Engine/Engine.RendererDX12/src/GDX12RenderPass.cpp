#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

std::vector<ExternalResourceUse> GDX12RenderPass::GetExternalResources(PassExecutionContext& context)
{
    std::vector<ExternalResourceUse> uses;
    auto* resources = context.GetDeviceResources();
    auto& frame = *resources->FrameConstants.at(context.GetFrameSlotIndex());
    if (_flags & RENDER_PASS_FLAG_USE_INSTANCES)
    {
        uses.push_back({&frame.InstanceCache->GetResource(), D3D12_RESOURCE_STATE_GENERIC_READ, {}});
        uses.push_back({&frame.TransformCache->GetResource(), D3D12_RESOURCE_STATE_GENERIC_READ, {}});
    }
    if (_flags & RENDER_PASS_FLAG_USE_MATERIALS)
    { uses.push_back({&frame.MaterialCache->GetResource(), D3D12_RESOURCE_STATE_GENERIC_READ, {}}); }
    if (_flags & RENDER_PASS_FLAG_USE_CAMERAS)
    {
        uses.push_back({&frame.CameraCB->GetResource(), D3D12_RESOURCE_STATE_GENERIC_READ, {}});
        uses.push_back({&frame.MainCB->GetResource(), D3D12_RESOURCE_STATE_GENERIC_READ, {}});
    }
    if (_flags & RENDER_PASS_FLAG_USE_LIGHTING)
    { uses.push_back({&frame.LightCB->GetResource(), D3D12_RESOURCE_STATE_GENERIC_READ, {}}); }
    if (_flags & RENDER_PASS_FLAG_USE_GEOMETRY)
    {
        uses.push_back({&resources->IndirectCommandsCache->GetResource(), D3D12_RESOURCE_STATE_GENERIC_READ, {}});
        _vertexResource.D3DResource = resources->GeometryBuffer->_vertexBuffer;
        _indexResource.D3DResource = resources->GeometryBuffer->_indexBuffer;
        uses.push_back({&_vertexResource, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, {}});
        uses.push_back({&_indexResource, D3D12_RESOURCE_STATE_INDEX_BUFFER, {}});
    }
    return uses;
}
