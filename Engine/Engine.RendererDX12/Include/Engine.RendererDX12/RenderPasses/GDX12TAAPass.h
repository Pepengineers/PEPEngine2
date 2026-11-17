#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include <array>

// One viewport stream; frame slots protect bindings, never select the history.
class GDX12TAAPass final : public GDX12RenderPass
{
public:
    struct Parameters
    {
        XMFLOAT4X4 PreviousViewProjection;
        UINT Width, Height, Valid;
        float HistoryWeight, VarianceGamma;
        XMFLOAT2 DepthJitterDeltaUV;
        float Padding = 0;
        UINT InputWidth, InputHeight;
        XMFLOAT2 InputJitterUV;
    };
    struct Frame
    {
        Parameters Constants{};
        std::array<GDX12Texture*, 5> Reads{}; // color, depth, complete motion, history color/depth
        GDX12Texture* Color = nullptr;
        GDX12Texture* Depth = nullptr;
        GDX12Resource* Camera = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS CameraAddress = 0;
        Matrix ViewProjection;
        XMFLOAT2 Jitter{};
        uint64_t Revision = 0;
        UINT Write = 0;
    };
    explicit GDX12TAAPass(float renderScale = 1.0f);
    void Setup(GDX12DeviceResources*, RenderPipelineCommonData*) override;
    void QueryRenderTargetResolution() override;
    void Initialize() override;
    void Resize() override;
    void PrepareRecording(); // owner, after camera upload and before publishing output
    const Frame& PreparedFrame() const { return _frame; }
    void RecordCommands(GDX12CommandList*, const Frame&) const;
    void Execute(GDX12CommandList* commands) override { RecordCommands(commands, _frame); }
    void Submitted();
    void CancelRecording() { _valid = false; _pending = false; }
    std::array<GDX12Texture, 2>& ColorHistory() { return _color; }
    std::array<GDX12Texture, 2>& DepthHistory() { return _depth; }
private:
    const float _renderScale;
    std::array<GDX12Texture, 2> _color, _depth;
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _resolve;
    Frame _frame;
    Matrix _previousViewProjection = Identity4x4();
    XMFLOAT2 _previousJitter{};
    uint64_t _revision = 0;
    UINT _write = 0;
    bool _valid = false, _pending = false;
};
