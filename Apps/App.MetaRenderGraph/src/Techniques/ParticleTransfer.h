#pragma once
#include "../Graph/CrossAdapterTransfer.h"
#include "../Graph/SimulationDomain.h"
#include "Engine.RendererDX12/GDX12Particles.h"

namespace Meta {
class ParticleTransfer {
public:
    ParticleTransfer(std::shared_ptr<SimulationDomain>, GDX12DeviceResources&);
    ~ParticleTransfer();
    void Update(GDX12ParticleSimulation&, GDX12ParticleInteractionField, float clockScale, bool reset);
    void Bind(GDX12ParticleSimulation&);
    GDX12Resource* Display() const;
    const std::vector<uint64_t>& Emitters() const;
    void Append(Graph&, std::vector<ID3D12Resource*>&, PublicationSelection);
    bool Record(Id, ID3D12GraphicsCommandList*);
    std::vector<PacketWait> Waits(const Schedule&) const;
    void RetireFrame(const Schedule&, const std::vector<CompletionToken>&, bool success);
    void Stop();
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
