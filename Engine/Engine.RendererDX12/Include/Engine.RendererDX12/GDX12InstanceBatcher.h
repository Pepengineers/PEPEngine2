#pragma once

#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12ConstantStructures.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include "Engine.RendererDX12/GDX12UploadBuffer.h"
#include <algorithm>
#include <stdexcept>

// Own one batcher per waited frame/view; sequential faces may reuse it after their draw.
class GDX12InstanceBatcher
{
public:
    explicit GDX12InstanceBatcher(GDX12Device* device) : _device(device)
    {
        CD3DX12_ROOT_PARAMETER parameters[8];
        parameters[0].InitAsConstants(3, 0);
        parameters[1].InitAsShaderResourceView(0);
        parameters[2].InitAsShaderResourceView(1);
        for (UINT slot = 0; slot < 5; ++slot) parameters[slot + 3].InitAsUnorderedAccessView(slot);
        CD3DX12_ROOT_SIGNATURE_DESC description(8, parameters);
        ComPtr<ID3DBlob> serialized, errors;
        ThrowIfFailed(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized, &errors));
        ThrowIfFailed(device->GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&_root)));
        const char* entries[] = {"ClearCS", "AppendCS", "FinalizeCS"};
        ComPtr<ID3D12PipelineState>* pipelines[] = {&_clear, &_append, &_finalize};
        for (UINT index = 0; index < 3; ++index)
        {
            const auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(device,
                SHADERS_FOLDER "InstanceBatching.hlsl", nullptr, entries[index], "cs");
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
            pipeline.pRootSignature = _root.Get();
            pipeline.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
            ThrowIfFailed(device->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(pipelines[index]->GetAddressOf())));
        }
    }

    void Build(GDX12CommandList* commands, GDX12UploadBuffer<GDX12InstanceData>& instances,
        GDX12UploadBuffer<GDX12IndirectDrawArgs>& inputCommands, GDX12UploadBuffer<UINT>& counter,
        UINT batchCount, UINT visibleInstanceCount, UINT counterOffset = 0)
    {
        if (counterOffset >= counter.GetElementCount() || batchCount > visibleInstanceCount ||
            visibleInstanceCount > instances.GetElementCount())
            throw std::invalid_argument("Invalid skeletal batch ranges");
        assert(_commands.get() != &inputCommands);
        const UINT capacity = inputCommands.GetElementCount();
        Ensure(_groups, batchCount);
        Ensure(_commands, capacity);
        Ensure(_visible, visibleInstanceCount);
        Ensure(_sourceCount, 1);
        GDX12_SCOPED_EVENT(commands, "Skeletal draw batching", Colors::CornflowerBlue);
        commands->ResourceBarrier({inputCommands.GetResource().GetNonPixelShaderResourceBarrier(),
            counter.GetResource().GetUnorderedAccessBarrier(), _groups->GetResource().GetUnorderedAccessBarrier(),
            _commands->GetResource().GetUnorderedAccessBarrier(), _visible->GetResource().GetUnorderedAccessBarrier(),
            _sourceCount->GetResource().GetUnorderedAccessBarrier()});
        commands->ResourceBarrier({counter.GetResource().GetUAVBarrier(), _groups->GetResource().GetUAVBarrier(),
            _sourceCount->GetResource().GetUAVBarrier()});
        auto* list = commands->GetCommandList().Get();
        list->SetComputeRootSignature(_root.Get());
        const UINT parameters[] = {capacity, batchCount, counterOffset};
        list->SetComputeRoot32BitConstants(0, 3, parameters, 0);
        list->SetComputeRootShaderResourceView(1, instances.GetElementAddress(0));
        list->SetComputeRootShaderResourceView(2, inputCommands.GetElementAddress(0));
        list->SetComputeRootUnorderedAccessView(3, _groups->GetElementAddress(0));
        list->SetComputeRootUnorderedAccessView(4, _commands->GetElementAddress(0));
        list->SetComputeRootUnorderedAccessView(5, counter.GetElementAddress(0));
        list->SetComputeRootUnorderedAccessView(6, _visible->GetElementAddress(0));
        list->SetComputeRootUnorderedAccessView(7, _sourceCount->GetElementAddress(0));
        commands->SetPipelineState(_clear);
        Dispatch(commands, (std::max)(batchCount, 1u));
        commands->ResourceBarrier({_groups->GetResource().GetUAVBarrier(), counter.GetResource().GetUAVBarrier(),
            _sourceCount->GetResource().GetUAVBarrier()});
        commands->SetPipelineState(_append);
        Dispatch(commands, capacity);
        commands->ResourceBarrier({_groups->GetResource().GetUAVBarrier(), _commands->GetResource().GetUAVBarrier(),
            counter.GetResource().GetUAVBarrier()});
        commands->SetPipelineState(_finalize);
        Dispatch(commands, batchCount);
        commands->ResourceBarrier({_commands->GetResource().GetIndirectArgsBarrier(),
            counter.GetResource().GetIndirectArgsBarrier(), _visible->GetResource().GetNonPixelShaderResourceBarrier()});
        commands->InvalidateCachedState();
    }

    GDX12UploadBuffer<GDX12IndirectDrawArgs>& Commands() { return *_commands; }
    D3D12_GPU_VIRTUAL_ADDRESS VisibleInstancesAddress() { return _visible->GetElementAddress(0); }

private:
    template<class T> void Ensure(std::unique_ptr<GDX12UploadBuffer<T>>& buffer, UINT count)
    {
        count = (std::max)(count, 1u);
        if (!buffer || buffer->GetElementCount() < count)
            buffer = std::make_unique<GDX12UploadBuffer<T>>(_device, count, EBufferType::Default, false);
    }

    static void Dispatch(GDX12CommandList* commands, UINT elements)
    {
        if (!elements) return;
        const UINT groups = (elements + 63) / 64;
        commands->Dispatch((std::min)(groups, 65535u), (groups + 65534) / 65535, 1);
    }

    GDX12Device* _device;
    ComPtr<ID3D12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _clear, _append, _finalize;
    std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>> _groups, _commands;
    std::unique_ptr<GDX12UploadBuffer<UINT>> _visible, _sourceCount;
};
