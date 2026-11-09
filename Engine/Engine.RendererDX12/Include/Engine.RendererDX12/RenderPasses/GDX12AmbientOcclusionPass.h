#pragma once
#include <cstdint>
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12AmbientOcclusion.h"

namespace XeGTAO { struct GTAOConstants; }
struct FFX_CACAO_D3D12Context;

class GDX12AmbientOcclusionPass final : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Ambient occlusion");
        graph.AsyncCompute();
        graph.Read(SharedResource::Scene);
        graph.ReadInputs(_inputs, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        graph.Write(_outputs[0], "Ambient occlusion", Lifetime::Dynamic, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    GDX12AmbientOcclusionPass();
    ~GDX12AmbientOcclusionPass() override;
    void Initialize() override;
    void Execute(GDX12CommandList* commands) override;
    bool IsAsyncComputeEnabled() const override { return IsAmbientOcclusionAsyncComputeEnabled(); }
    std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override;
    void Resize() override;
    void ClearDenendencies() override;

    struct FrameInput
    {
        GDX12AOSettings Settings;
        UINT Width, Height;
        Matrix View, JitteredProjection;
        UINT NoiseIndex, LocalCameraIndex;
    };

    // All descriptors refer to the native resource represented by the sole state tracker.
    struct PlacedTexture
    {
        GDX12Resource* Resource = nullptr;
        D3D12_GPU_DESCRIPTOR_HANDLE SRV{}, UAV{};
    };
    struct PlacedInputs
    {
        PlacedTexture Depth, Normals, PreparedDepth, PreparedNormals, Visibility, Output;
    };
    enum class PlacedStage { Pack, Evaluate, Finalize };
    struct PlacedAccess
    {
        GDX12Resource* Resource;
        GDX12ResourceHandle Native;
        GDX12RenderGraph::Mode Mode;
        D3D12_RESOURCE_STATES State;
    };
    class PlacedSlot;

    FrameInput CaptureFrameInput();
    void RecordLocalFrame(GDX12CommandList* commands, const FrameInput& input);
    void UpdateHistory(const FrameInput& input, bool placed);
    // Initialize a secondary recorder before creating any slots for it.
    void InitializePlacedDevice(GDX12DeviceResources& resources);
    std::shared_ptr<PlacedSlot> CreatePlacedSlot(PlacedStage stage, UINT width, UINT height, GDX12AOResolution resolution);
    // The slot, its recorder and the supplied texture/descriptor owners must outlive GPU work.
    // Reuse is allowed only after retirement of the previous invocation.
    void PreparePlacedSlot(const std::shared_ptr<PlacedSlot>& slot, const FrameInput& frame, const PlacedInputs& inputs);
    const std::vector<PlacedAccess>& GetPlacedAccesses(const PlacedSlot& slot) const;
    void RecordPlacedSlot(GDX12CommandList* commands, const PlacedSlot& slot);

private:
    friend struct GDX12AOTestAccess;

    struct ShaderConstants
    {
        float Radius, Intensity, Bias, HorizonBias;
        float FadeStart, FadeEnd, Sharpness;
        UINT Samples;
        UINT Directions, Steps, DenoisePasses, Implementation;
        float Slices, SliceSteps;
        UINT Half, Padding;
    };
    enum Pipeline : std::uint8_t
    {
        PrepareNormals, SSAO, HBAO, Blur, Resolve, XePrefilter, XeMain, XeDenoise, Neutral, PrepareHalf, Upsample, PadCACAO,
        PackFull, PipelineCount
    };
    struct WorkGrid
    {
        GDX12Texture Depth, Normals, Scratch[2], XeDepth, XeTerms[2], XeEdges, Visibility;
        std::array<GDX12Descriptor, 5> DepthUAVs;
        GDX12Texture CacaoDepth, CacaoNormals, CacaoVisibility;
        std::vector<uint8_t> CacaoMemory;
        FFX_CACAO_D3D12Context* Cacao = nullptr;
        bool CacaoScreenReady = false;
    };
    struct RecordingConstants;
    struct ConstantBindings
    {
        D3D12_GPU_VIRTUAL_ADDRESS View, AO, Xe;
    };
    class ScratchStates;

    void CreateTextures();
    void CreateGrid(WorkGrid& grid, UINT width, UINT height, bool half);
    void CreateScratch(WorkGrid& grid, UINT width, UINT height);
    void CreateTexture(GDX12Texture& texture, DXGI_FORMAT format, UINT width, UINT height, UINT mips = 1);
    static PlacedTexture BindTexture(GDX12Texture& texture);
    static RecordingConstants MakeRecordingConstants(const FrameInput& frame);
    void Bind(GDX12CommandList* commands, ConstantBindings constants);
    void Dispatch(GDX12CommandList* commands, Pipeline pipeline, UINT width, UINT height);
    void RecordPrepare(GDX12CommandList* commands, UINT width, UINT height, Pipeline pipeline,
        PlacedTexture depth, PlacedTexture normals, PlacedTexture preparedDepth, PlacedTexture preparedNormals);
    void RecordEvaluate(GDX12CommandList* commands, WorkGrid& grid, const RecordingConstants& frame, ConstantBindings bindings,
        PlacedTexture depth, PlacedTexture normals, PlacedTexture resolved, ScratchStates& states);
    void RecordUpsample(GDX12CommandList* commands, UINT width, UINT height,
        const PlacedInputs& inputs);
    void PrepareCACAO(WorkGrid& grid, PlacedTexture depth, PlacedTexture normals, bool placed);
    GDX12Texture& DrawCACAO(GDX12CommandList* commands, WorkGrid& grid, const RecordingConstants& frame, ScratchStates& states);
    static void DestroyCACAO(WorkGrid& grid);

    GDX12Texture _output;
    std::array<WorkGrid, 2> _grids;
    std::array<ComPtr<ID3D12PipelineState>, PipelineCount> _pipelines;
    std::unique_ptr<GDX12RootSignature> _root;
    std::unique_ptr<GDX12UploadBuffer<ShaderConstants>> _constants;
    std::unique_ptr<GDX12UploadBuffer<XeGTAO::GTAOConstants>> _xeConstants;
    GDX12AOSettings _previous = {};
    bool _hasPrevious = false;
    bool _previousPlaced = false;
    UINT _frameIndex = 0;
};
