#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

// The input value holds the immutable water settings/seed. Equality defines solver compatibility.
template<class Input>
class GDX12FluidClock
{
public:
    static constexpr std::size_t SpanCapacity = 16;
    static constexpr std::size_t TicksPerInvocation = 2;

    struct Tick
    {
        Input Parameters{};
        double Seconds = 0;
    };

    class Batch
    {
    public:
        std::size_t Size() const { return _count; }
        const Tick& operator[](std::size_t index) const { return _ticks.at(index); }
        std::uint64_t EndTick() const { return _firstTick + _count; }
        double EndTime() const { return _endTime; }
    private:
        friend class GDX12FluidClock;
        const GDX12FluidClock* _owner = nullptr;
        std::array<Tick, TicksPerInvocation> _ticks{};
        std::uint64_t _epoch = 0, _firstTick = 0;
        std::size_t _count = 0;
        double _endTime = 0;
    };

    struct Progress
    {
        double DesiredTime, IssuedTime, CompletedTime, AcceptedDebt, UnrepresentedElapsed;
        std::uint64_t IssuedTicks, CompletedTicks, FirstMissingInput;
        std::size_t Spans;
        bool InFlight;
    };

    explicit GDX12FluidClock(std::uint64_t epoch) : _epoch(epoch)
    {
        if (epoch == 0) throw std::invalid_argument("Fluid clock requires a valid epoch");
    }
    GDX12FluidClock(const GDX12FluidClock&) = delete;
    GDX12FluidClock& operator=(const GDX12FluidClock&) = delete;

    // False latches InputHistoryFull for this epoch. Only the represented prefix may drain.
    bool Append(const Input& input, double elapsed, double quantum)
    {
        if (!std::isfinite(elapsed) || elapsed < 0)
            throw std::invalid_argument("Invalid fluid elapsed time");
        if (elapsed == 0) return _firstMissing == 0;
        if (!std::isfinite(quantum) || quantum <= 0)
            throw std::invalid_argument("Invalid fluid logical tick");
        const double desired = _desired + elapsed;
        if (!std::isfinite(desired) || desired <= _desired || _inputSequence == MaxId)
            throw std::overflow_error("Fluid input clock exhausted");

        const auto sequence = _inputSequence + 1;
        const bool compatible = _count && At(_count - 1).Quantum == quantum &&
            At(_count - 1).Parameters == input;
        if (_firstMissing || (!compatible && _count == SpanCapacity))
        {
            if (!_firstMissing) _firstMissing = sequence;
            _unrepresented += elapsed;
        }
        else if (compatible) At(_count - 1).Remaining += elapsed;
        else
        {
            At(_count) = {input, elapsed, quantum};
            ++_count;
        }
        _inputSequence = sequence;
        _desired = desired;
        return _firstMissing == 0;
    }

    Batch Prepare() const
    {
        Batch batch;
        if (_inFlight) return batch;
        batch._owner = this;
        batch._epoch = _epoch;
        batch._firstTick = _issuedTicks;
        batch._endTime = _issued;
        for (std::size_t offset = 0; offset < _count && batch._count < TicksPerInvocation; ++offset)
        {
            const auto& span = At(offset);
            double remaining = span.Remaining;
            const bool closed = offset + 1 != _count || _firstMissing != 0;
            while (remaining > 0 && batch._count < TicksPerInvocation)
            {
                if (remaining < span.Quantum && !closed) return batch;
                const double seconds = remaining < span.Quantum ? remaining : span.Quantum;
                const double endTime = batch._endTime + seconds;
                if (_issuedTicks >= MaxId - batch._count || !std::isfinite(endTime) || endTime <= batch._endTime)
                    throw std::overflow_error("Fluid solver clock exhausted");
                batch._ticks[batch._count++] = {span.Parameters, seconds};
                batch._endTime = endTime;
                remaining -= seconds;
            }
        }
        return batch;
    }

    // Commit only after a successful solver receipt. Preparing/retrying never consumes inputs.
    bool Commit(const Batch& batch)
    {
        if (_inFlight || !Owns(batch) || batch._firstTick != _issuedTicks) return false;
        for (std::size_t i = 0; i < batch._count; ++i)
        {
            auto& span = At(0);
            span.Remaining -= batch[i].Seconds;
            if (span.Remaining > 0) continue;
            span = {};
            _head = (_head + 1) % SpanCapacity;
            --_count;
        }
        _issuedTicks = batch.EndTick();
        _issued = batch.EndTime();
        _inFlight = true;
        return true;
    }

    // The render owner observes the real solver CompletionPoint before acknowledging this batch.
    bool Complete(const Batch& batch)
    {
        if (!_inFlight || !Owns(batch) || batch._firstTick != _completedTicks ||
            batch.EndTick() != _issuedTicks || batch.EndTime() != _issued) return false;
        _completedTicks = _issuedTicks;
        _completed = _issued;
        _inFlight = false;
        return true;
    }

    Progress GetProgress() const
    {
        double accepted = 0;
        for (std::size_t i = 0; i < _count; ++i) accepted += At(i).Remaining;
        return {_desired, _issued, _completed, accepted, _unrepresented,
            _issuedTicks, _completedTicks, _firstMissing, _count, _inFlight};
    }

    // Native invocation owners retire separately. Resetting the clock does not release GPU storage.
    double Reset(std::uint64_t epoch)
    {
        if (epoch <= _epoch) throw std::invalid_argument("Fluid clock epoch must advance");
        const auto progress = GetProgress();
        _epoch = epoch;
        _spans = {};
        _head = _count = 0;
        _desired = _issued = _completed = _unrepresented = 0;
        _inputSequence = _firstMissing = _issuedTicks = _completedTicks = 0;
        _inFlight = false;
        return progress.AcceptedDebt + progress.UnrepresentedElapsed;
    }

private:
    struct Span
    {
        Input Parameters{};
        double Remaining = 0, Quantum = 0;
    };
    static constexpr auto MaxId = (std::numeric_limits<std::uint64_t>::max)();
    Span& At(std::size_t offset) { return _spans[(_head + offset) % SpanCapacity]; }
    const Span& At(std::size_t offset) const { return _spans[(_head + offset) % SpanCapacity]; }
    bool Owns(const Batch& batch) const
    { return batch._owner == this && batch._epoch == _epoch && batch._count != 0; }

    std::array<Span, SpanCapacity> _spans{};
    std::uint64_t _epoch;
    std::uint64_t _inputSequence = 0, _firstMissing = 0, _issuedTicks = 0, _completedTicks = 0;
    double _desired = 0, _issued = 0, _completed = 0, _unrepresented = 0;
    std::size_t _head = 0, _count = 0;
    bool _inFlight = false;
};
