#include "Rendering/Water/GDX12FluidClock.h"
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
    struct Input
    {
        int Settings = 0;
        int Transform = 0;
        std::shared_ptr<const int> Seed;
        friend bool operator==(const Input& a, const Input& b)
        { return a.Settings == b.Settings && a.Transform == b.Transform && a.Seed == b.Seed; }
    };
    using Clock = GDX12FluidClock<Input>;
    constexpr double TickSeconds = 1.0 / 60.0;

    void Check(bool condition, const char* expression, int line)
    {
        if (!condition)
            throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
    }
#define CHECK(...) Check(static_cast<bool>(__VA_ARGS__), #__VA_ARGS__, __LINE__)

    bool Near(double a, double b) { return std::abs(a - b) < 1e-10; }

    template<class Action> void Reject(Action action)
    {
        bool rejected = false;
        try { action(); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    }

    std::vector<double> Timeline(int attemptPeriod)
    {
        Clock clock(1);
        const Input input{1, 4, std::make_shared<const int>(17)};
        std::vector<double> ticks;
        for (int frame = 1; frame <= 60; ++frame)
        {
            CHECK(clock.Append(input, TickSeconds, TickSeconds));
            if (frame % attemptPeriod) continue;
            const auto batch = clock.Prepare();
            CHECK(batch.Size() == static_cast<std::size_t>(attemptPeriod));
            for (std::size_t i = 0; i < batch.Size(); ++i)
            {
                CHECK(batch[i].Parameters == input);
                ticks.push_back(batch[i].Seconds);
            }
            CHECK(clock.Commit(batch));
            CHECK(clock.GetProgress().IssuedTicks == static_cast<std::uint64_t>(frame));
            CHECK(clock.GetProgress().CompletedTicks == static_cast<std::uint64_t>(frame - attemptPeriod));
            CHECK(clock.Prepare().Size() == 0 && clock.Complete(batch));
        }
        const auto progress = clock.GetProgress();
        CHECK(progress.CompletedTicks == 60 && Near(progress.CompletedTime, 1));
        CHECK(Near(progress.DesiredTime, 1) && Near(progress.AcceptedDebt, 0));
        return ticks;
    }

    void ReferenceCadence()
    {
        const auto everyFrame = Timeline(1), everyOther = Timeline(2);
        CHECK(everyFrame == everyOther && everyFrame.size() == 60);
        for (const double seconds : everyFrame) CHECK(Near(seconds, TickSeconds));
    }

    void ImmutableTailsAndRetry()
    {
        Clock clock(2), other(2);
        Input a{3, 7, std::make_shared<const int>(9)};
        const Input b{3, 8, a.Seed}, c{4, 8, a.Seed};
        CHECK(clock.Append(a, TickSeconds / 2, TickSeconds));
        CHECK(clock.Prepare().Size() == 0);
        CHECK(clock.Append(a, TickSeconds / 4, TickSeconds));
        CHECK(clock.GetProgress().Spans == 1);
        a.Transform = 100;
        CHECK(clock.Append(b, TickSeconds / 2, TickSeconds));
        const auto deferred = clock.Prepare();
        CHECK(deferred.Size() == 1 && Near(deferred[0].Seconds, TickSeconds * 0.75));
        CHECK(deferred[0].Parameters.Transform == 7 && *deferred[0].Parameters.Seed == 9);
        CHECK(!other.Commit(deferred) && !clock.Complete(deferred));
        CHECK(clock.GetProgress().IssuedTicks == 0);
        CHECK(clock.Append(b, TickSeconds * 1.5, TickSeconds));
        CHECK(clock.Commit(deferred) && !clock.Commit(deferred));
        CHECK(clock.GetProgress().InFlight && Near(clock.GetProgress().AcceptedDebt, 2 * TickSeconds));
        CHECK(clock.Append(c, TickSeconds / 4, TickSeconds));
        CHECK(clock.Prepare().Size() == 0);
        CHECK(clock.Complete(deferred) && !clock.Complete(deferred));
        const auto next = clock.Prepare();
        CHECK(next.Size() == 2 && next[0].Parameters == b && next[1].Parameters == b);
        CHECK(clock.Commit(next) && clock.Complete(next));
        CHECK(clock.Prepare().Size() == 0 && Near(clock.GetProgress().AcceptedDebt, TickSeconds / 4));
        CHECK(clock.Append(b, TickSeconds, TickSeconds));
        const auto afterRevision = clock.Prepare();
        CHECK(afterRevision.Size() == 2 && afterRevision[0].Parameters == c && afterRevision[1].Parameters == b);
        CHECK(Near(afterRevision[0].Seconds, TickSeconds / 4));
        CHECK(clock.Commit(afterRevision) && clock.Complete(afterRevision));
        CHECK(!clock.Commit(deferred));
    }

    void StaticOverloadAndPause()
    {
        Clock clock(3);
        const Input input{1, 0, {}};
        CHECK(clock.Append(input, 0, 0) && clock.GetProgress().Spans == 0);
        for (int i = 0; i < 600; ++i) CHECK(clock.Append(input, TickSeconds, TickSeconds));
        const auto stalled = clock.GetProgress();
        CHECK(stalled.Spans == 1 && stalled.FirstMissingInput == 0 && Near(stalled.AcceptedDebt, 10));
        const auto batch = clock.Prepare();
        CHECK(batch.Size() == Clock::TicksPerInvocation && clock.Commit(batch));
        CHECK(clock.Append(input, 0, 0) && clock.Prepare().Size() == 0);
        CHECK(Near(clock.GetProgress().DesiredTime, 10) && Near(clock.GetProgress().CompletedTime, 0));
        CHECK(clock.Complete(batch));
        CHECK(Near(clock.GetProgress().AcceptedDebt, 10 - 2 * TickSeconds));
    }

    void FullHistoryCannotReplayTheGap()
    {
        Clock clock(4);
        for (std::size_t i = 0; i < Clock::SpanCapacity; ++i)
            CHECK(clock.Append({1, static_cast<int>(i), {}}, TickSeconds, TickSeconds));
        const Input last{1, static_cast<int>(Clock::SpanCapacity - 1), {}};
        CHECK(clock.Append(last, TickSeconds / 2, TickSeconds));
        CHECK(!clock.Append({2, last.Transform, {}}, TickSeconds, TickSeconds));
        const auto full = clock.GetProgress();
        CHECK(full.Spans == Clock::SpanCapacity && full.FirstMissingInput == Clock::SpanCapacity + 2);
        CHECK(Near(full.AcceptedDebt, (Clock::SpanCapacity + 0.5) * TickSeconds));
        CHECK(Near(full.UnrepresentedElapsed, TickSeconds));

        std::vector<int> transforms;
        for (;;)
        {
            const auto batch = clock.Prepare();
            if (batch.Size() == 0) break;
            for (std::size_t i = 0; i < batch.Size(); ++i)
            {
                CHECK(batch[i].Parameters.Settings == 1);
                transforms.push_back(batch[i].Parameters.Transform);
            }
            CHECK(clock.Commit(batch) && clock.Complete(batch));
        }
        CHECK(transforms.size() == Clock::SpanCapacity + 1 && transforms.back() == last.Transform);
        for (std::size_t i = 0; i < Clock::SpanCapacity; ++i) CHECK(transforms[i] == static_cast<int>(i));
        CHECK(!clock.Append(last, 2 * TickSeconds, TickSeconds));
        const auto terminal = clock.GetProgress();
        CHECK(terminal.FirstMissingInput == full.FirstMissingInput && terminal.AcceptedDebt == 0);
        CHECK(clock.Prepare().Size() == 0 && Near(terminal.UnrepresentedElapsed, 3 * TickSeconds));
        CHECK(Near(terminal.DesiredTime - terminal.CompletedTime, terminal.UnrepresentedElapsed));
        CHECK(Near(clock.Reset(5), 3 * TickSeconds));
        CHECK(clock.GetProgress().FirstMissingInput == 0 && clock.Append(last, TickSeconds, TickSeconds));
        CHECK(clock.Prepare().Size() == 1);
    }

    void EpochInvalidatesReceipts()
    {
        Clock clock(9);
        const Input input{1, 1, {}};
        Reject([&] { clock.Append(input, -1, TickSeconds); });
        Reject([&] { clock.Append(input, TickSeconds, 0); });
        Reject([&] { clock.Append(input, (std::numeric_limits<double>::quiet_NaN)(), TickSeconds); });
        Reject([&] { clock.Reset(9); });
        CHECK(clock.GetProgress().DesiredTime == 0);
        CHECK(clock.Append(input, TickSeconds * 3, TickSeconds));
        const auto old = clock.Prepare();
        CHECK(clock.Commit(old) && Near(clock.Reset(10), TickSeconds));
        CHECK(!clock.Complete(old) && !clock.Commit(old));
        CHECK(clock.GetProgress().DesiredTime == 0 && !clock.GetProgress().InFlight);
        CHECK(clock.Append(input, TickSeconds / 2, TickSeconds / 2));
        const auto fresh = clock.Prepare();
        CHECK(fresh.Size() == 1 && Near(fresh[0].Seconds, TickSeconds / 2));
        CHECK(clock.Commit(fresh) && clock.Complete(fresh));
    }
}

int main()
{
    try
    {
        ReferenceCadence();
        ImmutableTailsAndRetry();
        StaticOverloadAndPause();
        FullHistoryCannotReplayTheGap();
        EpochInvalidatesReceipts();
        std::cout << "Water clock: logical cadence, immutable inputs, overload/debt, terminal history and receipts passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Water clock FAILED: " << error.what() << '\n';
        return 1;
    }
}
