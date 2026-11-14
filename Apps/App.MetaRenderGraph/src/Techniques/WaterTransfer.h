#pragma once
#include "../Graph/CrossAdapterTransfer.h"
#include "Rendering/Water/GDX12FluidPass.h"

namespace Meta {
// One fixed water topology per epoch. Only cold topology/reset changes drain;
// ordinary frames update immutable inputs and lease a bounded received slot.
class WaterTransfer {
public:
    WaterTransfer(GDX12Device&, GDX12DescriptorHeap&, int secondaryAdapter);
    ~WaterTransfer();
    const std::shared_ptr<QueueSubmission>& Submissions() const;
    void Update(const std::vector<GDX12FluidInstance>&, bool reset);
    void Stop();
    GDX12FluidSurfacePass::ExternalDisplay Displays() const;
    void Append(Graph&, std::vector<ID3D12Resource*>&, PublicationSelection);
    bool Record(Id, ID3D12GraphicsCommandList*);
    std::vector<PacketWait> Waits(const Schedule&) const;
    void RetireFrame(const Schedule&, const std::vector<CompletionToken>&, bool success);
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
