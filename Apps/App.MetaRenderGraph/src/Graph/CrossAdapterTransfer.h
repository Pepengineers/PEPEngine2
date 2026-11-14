#pragma once
#include "MetaGraphExecutor.h"
#include <memory>
namespace Meta {
enum class PublicationSelection { StrictPublication, LatestCompleted };

// Experimental host API. Construction fixes capacity/devices; Start creates an
// independent producer. Append/Record/RetireFrame belong to the primary owner.
// RetireFrame must receive every successfully submitted reader, also on errors.
class CrossAdapterTransfer {
public:
    CrossAdapterTransfer(GDX12Device& primary, int secondaryAdapter, uint32_t frameSlots);
    ~CrossAdapterTransfer();
    const std::shared_ptr<QueueSubmission>& Submissions() const;
    void Start();
    void Stop();
    void SetSeed(uint32_t); // frozen by value for each independent recording job
    // Returns the selected sequence, or zero when the requested publication has
    // not been issued. Nonzero requiredSequence is valid only for Strict.
    uint64_t Append(Graph&, std::vector<ID3D12Resource*>&, uint32_t frameSlot,
        PublicationSelection, uint64_t requiredSequence = 0);
    bool Record(Id operation, ID3D12GraphicsCommandList*);
    std::vector<PacketWait> Waits(const Schedule&) const;
    void RetireFrame(const Schedule&, const std::vector<CompletionToken>&, bool success);
    uint64_t SubmittedSteps() const;
    // Version used by the last successfully SUBMITTED reader frame (Strict can
    // still be GPU-in-flight). Failure never advances this diagnostic.
    uint64_t DisplayedSequence() const;
    ID3D12Resource* ConsumerResult(uint32_t frameSlot, uint32_t reader) const;
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
