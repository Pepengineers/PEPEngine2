#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 619; __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }
void Check(HRESULT result) { if (FAILED(result)) { std::cerr << "HRESULT=" << std::hex << result << std::dec << '\n'; throw std::runtime_error("D3D12 call failed"); } }
int main(int argc, char** argv)
{
    try
    {
        std::cout.setf(std::ios::unitbuf);
        if (argc != 6) throw std::runtime_error("usage: intel|nvidia direct|draw|constant upload|default 1|20 debug|gbv");
        const UINT vendor = std::string(argv[1]) == "intel" ? 0x8086 : 0x10de;
        const std::string mode = argv[2], heapMode = argv[3];
        const UINT words = static_cast<UINT>(std::stoul(argv[4]));
        ComPtr<ID3D12Debug> debug; Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
        if (std::string(argv[5]) == "gbv") { ComPtr<ID3D12Debug1> gbv; Check(debug.As(&gbv)); gbv->SetEnableGPUBasedValidation(TRUE); }
        ComPtr<IDXGIFactory6> factory; Check(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 description; Check(candidate->GetDesc1(&description));
            if (description.VendorId == vendor && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && (vendor != 0x8086 || std::wstring(description.Description).find(L"B570") != std::wstring::npos))
            { adapter = candidate; std::wcout << description.Description << L'\n'; break; }
        }
        if (!adapter) throw std::runtime_error("Requested physical adapter not found");
        ComPtr<ID3D12Device> device; Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC queueDesc{}; queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue; Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12CommandAllocator> allocator; Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list; Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        auto makeBuffer = [&](UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
        {
            D3D12_HEAP_PROPERTIES properties{}; properties.Type = heap;
            D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes;
            desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> result; Check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&result))); return result;
        };
        auto writeBuffer = [&](ID3D12Resource* resource, const void* data, size_t bytes)
        { void* address = nullptr; D3D12_RANGE empty{0,0}; Check(resource->Map(0, &empty, &address)); std::memcpy(address, data, bytes); resource->Unmap(0, nullptr); };
        auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        { D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after}; list->ResourceBarrier(1, &barrier); };
        D3D12_ROOT_PARAMETER1 parameter{}; parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants = {0,0,words}; parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc{}; rootDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rootDesc.Desc_1_1.NumParameters = 1; rootDesc.Desc_1_1.pParameters = &parameter;
        rootDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> rootBlob, errors; Check(D3D12SerializeVersionedRootSignature(&rootDesc, &rootBlob, &errors));
        ComPtr<ID3D12RootSignature> root; Check(device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(&root)));
        const char* shader = "cbuffer C:register(b0){uint InstanceID;} float4 VS(uint id:SV_VertexID):SV_POSITION {float2 v[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};return float4(v[id],0.5,1);} float PS():SV_TARGET {return (InstanceID+1)/4.0;}";
        ComPtr<ID3DBlob> vs, ps;
        Check(D3DCompile(shader, std::strlen(shader), nullptr, nullptr, nullptr, "VS", "vs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors));
        Check(D3DCompile(shader, std::strlen(shader), nullptr, nullptr, nullptr, "PS", "ps_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{}; pipeline.pRootSignature = root.Get();
        pipeline.VS = {vs->GetBufferPointer(),vs->GetBufferSize()}; pipeline.PS = {ps->GetBufferPointer(),ps->GetBufferSize()};
        pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pipeline.RasterizerState.DepthClipEnable = TRUE;
        auto& blend = pipeline.BlendState.RenderTarget[0]; blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE; blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD; blend.LogicOp = D3D12_LOGIC_OP_NOOP; blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pipeline.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS}; pipeline.DepthStencilState.BackFace = pipeline.DepthStencilState.FrontFace;
        pipeline.SampleMask = UINT_MAX; pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.NumRenderTargets = 1; pipeline.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT; pipeline.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> pso; Check(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&pso)));
        D3D12_RESOURCE_DESC image{}; image.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; image.Width = image.Height = 4;
        image.DepthOrArraySize = image.MipLevels = 1; image.Format = DXGI_FORMAT_R32_FLOAT; image.SampleDesc.Count = 1; image.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES defaultHeap{}; defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> target; Check(device->CreateCommittedResource(&defaultHeap,D3D12_HEAP_FLAG_NONE,&image,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&target)));
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{}; heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heapDesc.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> rtv; Check(device->CreateDescriptorHeap(&heapDesc,IID_PPV_ARGS(&rtv)));
        const auto handle = rtv->GetCPUDescriptorHandleForHeapStart(); device->CreateRenderTargetView(target.Get(),nullptr,handle);
        const UINT indices[] = {0,1,2}; auto indexBuffer = makeBuffer(sizeof(indices),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ); writeBuffer(indexBuffer.Get(),indices,sizeof(indices));
        const UINT arguments[] = {2,3,1,0,0,0}; const bool constant = mode == "constant";
        const UINT argumentBytes = constant ? 24 : 20; const void* argumentData = arguments + (constant ? 0 : 1);
        auto upload = makeBuffer(argumentBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ); writeBuffer(upload.Get(),argumentData,argumentBytes);
        ComPtr<ID3D12Resource> gpuArguments = upload;
        if (heapMode == "default")
        { gpuArguments = makeBuffer(argumentBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST); list->CopyBufferRegion(gpuArguments.Get(),0,upload.Get(),0,argumentBytes); transition(gpuArguments.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT); }
        D3D12_INDIRECT_ARGUMENT_DESC descriptors[2]{};
        descriptors[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT; descriptors[0].Constant = {0,0,1}; descriptors[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC signatureDesc{}; signatureDesc.ByteStride = argumentBytes; signatureDesc.NumArgumentDescs = constant ? 2 : 1; signatureDesc.pArgumentDescs = descriptors + (constant ? 0 : 1);
        ComPtr<ID3D12CommandSignature> signature; Check(device->CreateCommandSignature(&signatureDesc,constant ? root.Get() : nullptr,IID_PPV_ARGS(&signature)));
        const D3D12_VIEWPORT viewport{0,0,4,4,0,1}; const D3D12_RECT scissor{0,0,4,4}; const float clear[4]{};
        list->SetPipelineState(pso.Get()); list->SetGraphicsRootSignature(root.Get()); list->RSSetViewports(1,&viewport); list->RSSetScissorRects(1,&scissor);
        list->ClearRenderTargetView(handle,clear,0,nullptr); list->OMSetRenderTargets(1,&handle,FALSE,nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_INDEX_BUFFER_VIEW ib{indexBuffer->GetGPUVirtualAddress(),sizeof(indices),DXGI_FORMAT_R32_UINT}; list->IASetIndexBuffer(&ib);
        std::array<UINT,20> constants{}; constants[0] = 2; if (words != 1 && words != 20) throw std::runtime_error("root size must be 1 or 20");
        list->SetGraphicsRoot32BitConstants(0,words,constants.data(),0);
        std::cout << "record " << mode << " heap=" << heapMode << " words=" << words << " validation=" << argv[5] << '\n';
        if (mode == "direct") list->DrawIndexedInstanced(3,1,0,0,0);
        else list->ExecuteIndirect(signature.Get(),1,gpuArguments.Get(),0,nullptr,0);
        transition(target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 total = 0; device->GetCopyableFootprints(&image,0,1,0,&footprint,nullptr,nullptr,&total);
        auto readback = makeBuffer(total,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = readback.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = target.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&destination,0,0,0,&source,nullptr); Check(list->Close());
        ID3D12CommandList* lists[] = {list.Get()}; queue->ExecuteCommandLists(1,lists); Check(queue->Signal(fence.Get(),1));
        const HANDLE event = CreateEventW(nullptr,FALSE,FALSE,nullptr); Check(fence->SetEventOnCompletion(1,event));
        const DWORD wait = WaitForSingleObject(event,30000); CloseHandle(event); if(wait != WAIT_OBJECT_0) throw std::runtime_error("GPU fence did not complete within 30s");
        Check(device->GetDeviceRemovedReason());
        void* bytes = nullptr; D3D12_RANGE range{0,static_cast<SIZE_T>(total)}; Check(readback->Map(0,&range,&bytes));
        float value; std::memcpy(&value,static_cast<char*>(bytes) + footprint.Offset + 2*footprint.Footprint.RowPitch + 2*sizeof(float),sizeof(value));
        D3D12_RANGE noWrite{0,0}; readback->Unmap(0,&noWrite); std::cout << "pixel=" << value << " expected=0.75\n";
        ComPtr<ID3D12InfoQueue> info; Check(device.As(&info));
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i) { SIZE_T length=0; Check(info->GetMessage(i,nullptr,&length)); std::string storage(length,'\0'); auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); Check(info->GetMessage(i,message,&length)); if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription); }
        return std::abs(value-0.75f)<0.001f ? 0 : 1;
    }
    catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}