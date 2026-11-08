#pragma once
#include "Engine.RendererDX12/D3DHelpers.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12FrameConstants.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include "Engine.RendererDX12/GDX12Texture.h"
#include "Engine.RendererDX12/GDX12Material.h"
#include "Engine.RendererDX12/GDX12GeometryBuffer.h"
#include "Engine.RendererDX12/GDX12Particles.h"
#include "Engine.RendererDX12/GDX12LocalLights.h"
#include "Engine.RendererDX12/GDX12RenderGeometry.h"
#include <optional>

class GDX12Device;
class GameTimer;
class IRenderGeometrySource;
class GDX12RenderGraphBuilder;
struct GDX12GeometryContext;

class GDX12DeviceResources
{
public:
    GDX12DeviceResources() : CurrFrameConstantsIndex(0), Device(nullptr) {}

    std::unordered_map<std::string, std::vector<D3D12_INPUT_ELEMENT_DESC>> InputLayouts;
    std::unique_ptr<GDX12GeometryBuffer> GeometryBuffer;
    UINT SkeletalBatchCount = 0;
    UINT SkeletalInstanceCount = 0;

    // All heaps created in one high-capacity instance
    std::unique_ptr<GDX12DescriptorHeap> RTVHeap;
    std::unique_ptr<GDX12DescriptorHeap> SRV_UAV_Heap;
    std::unique_ptr<GDX12DescriptorHeap> DSVHeap;

    std::vector<std::unique_ptr<GDX12FrameConstants>> FrameConstants;
    UINT CurrFrameConstantsIndex;

    std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>> IndirectCommandsCache;

    std::unordered_map<std::string, ComPtr<ID3DBlob>> Shaders;
    std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> PSOs;
    std::unordered_map<std::string, std::unique_ptr<GDX12RootSignature>> RootSignatures;
    std::unordered_map <std::string, ComPtr<ID3D12CommandSignature>> CommandSignatures;
    std::unordered_map<std::string, std::unique_ptr<GDX12Texture>> Textures;

    std::vector<IRenderGeometrySource*> GeometrySources; // Borrowed from the pipeline owner.
    GDX12RenderPrimitiveRegistry Primitives;
    void BeginMeshPrimitiveUpdate();
    GDX12RenderPrimitiveHandle PublishMeshPrimitive(UINT instance, const GDX12RenderPrimitive& primitive);
    bool RetainMeshPrimitive(UINT instance, GDX12RenderPrimitiveHandle handle);
    void RemoveMeshPrimitive(UINT instance);
    void EndMeshPrimitiveUpdate();
    void GatherGeometryBatches(const GDX12GeometryContext&, const GDX12RenderBatch& mesh, std::vector<GDX12RenderBatch>&) const;
    void EmitGeometry(GDX12CommandList*, const GDX12GeometryContext&, const GDX12RenderBatch& mesh);
    std::vector<GDX12RenderGraph::BufferAccess> GetGeometryBufferAccesses(GDX12GeometryStage stage) const;
    uint64_t GeometryRevision = 0;
    GDX12Device* Device;
    GDX12MainConstants Lighting;
    GDX12SkyState Sky;
    GDX12Descriptor SkyTransmittanceSRV;
    GDX12Descriptor SkyViewSRV;
    GDX12Descriptor SkyAerialRadianceSRV;
    GDX12Descriptor SkyAerialTransmittanceSRV;
    std::vector<GDX12ParticleEmitter> ParticleEmitters;
    std::unique_ptr<GDX12ParticleSimulation> ParticleSimulation;
    GDX12Texture* ParticleMotion = nullptr; // Borrowed for this frame only, owned by the particles pass.
    std::vector<GDX12ReflectionProbe> ReflectionProbes;
    std::vector<GDX12IndirectDrawArgs> ReflectionPreviewDraws;
    GDX12Descriptor ReflectionRadianceSRV;
    uint64_t ReflectionWorldRevision = 0;
    uint64_t ReflectionStaticRevision = 0;
    uint64_t ReflectionDynamicRevision = 0;
    uint64_t ReflectionEnvironmentRevision = 0;
    BoundingBox ReflectionSceneBounds = { XMFLOAT3(0, 0, 0), XMFLOAT3(10000, 10000, 10000) };
    GDX12ShadowConstants ShadowSettings;
    std::vector<GDX12LocalLightConstants> LocalLights;
    // Complete current caster snapshot; standalone clients without one retain revision-based invalidation.
    std::optional<std::vector<GDX12LocalShadowCaster>> LocalShadowCasters;
    std::vector<GDX12LocalShadowVisibility> LocalShadowVisibility;
    void Initialize(GDX12Device* device);
    void UpdateMainCB(UINT width, UINT height, GameTimer* timer);
    void UpdateMaterialCB(std::unordered_map<std::string, std::unique_ptr<GDX12Material>>& materials);
    void PrepareLocalShadowData();
    void UploadLocalShadowData(GDX12CommandList* commands);
private:
    struct MeshPrimitive { GDX12RenderPrimitiveHandle Handle; uint64_t Epoch = 0; };
    // Instance indices already address the dense GPU instance buffers.
    std::vector<MeshPrimitive> _meshPrimitives;
    uint64_t _meshPrimitiveEpoch = 0;
};
