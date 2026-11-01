#include "Engine.Core/ECS/ECSStorage.h"
#include "Engine.Core/Jobs/JobSystem.h"
#include <atomic>
#include <iostream>
#include <stdexcept>

struct Position { double X = 0; };
struct Velocity { double X = 0; };
class World { public: ECSStorage<Position, Velocity> Data; };
#include "Engine.Core/System.h"

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class Movement final : public System
{
public:
    std::atomic<size_t> RangeCalls{ 0 };
    void Tick(World& world, float dt) override { TickRange(world, dt, 0, GetWorkCount(world)); }
protected:
    size_t GetWorkCount(World& world) const override { return world.Data.GetPool<Position>().Size(); }
    void TickRange(World& world, float dt, size_t begin, size_t end) override
    {
        ++RangeCalls;
        world.Data.ForEachRange<Position, Velocity>(begin, end,
            [dt](Entity, Position& position, Velocity& velocity) { position.X += velocity.X * dt; });
    }
};

class Verify final : public System
{
public:
    void Tick(World& world, float) override
    {
        size_t index = 0;
        world.Data.ForEach<Position>([&](Entity, Position& position) {
            const double expected = static_cast<double>(index) + (index % 2 == 0 ? 1.0 : 0.0);
            Require(position.X == expected, "System B observed an unfinished or incorrect System A");
            ++index;
        });
    }
};

static void Check(unsigned threads)
{
    World world;
    for (size_t i = 0; i < 4097; ++i)
    {
        auto entity = world.Data.CreateEntity();
        entity.AddComponent<Position>(Position{ static_cast<double>(i) });
        if (i % 2 == 0) entity.AddComponent<Velocity>(Velocity{ 4.0 });
    }
    JobSystem jobs(threads, 37);
    Movement movement;
    Verify verify;
    auto first = movement.Schedule(jobs, world, 0.25f, 63);
    verify.Schedule(jobs, world, 0, 1, first).Complete();
    Require(movement.RangeCalls > 0 && movement.RangeCalls < 4097, "System must receive whole batches in TickRange");
    movement.RangeCalls = 0;
    const auto defaultWork = movement.Schedule(jobs, world, 0);
    verify.Schedule(jobs, world, 0, defaultWork).Complete();
    Require(movement.RangeCalls > 0 && movement.RangeCalls < 4097, "Default batches must reach TickRange intact");
    movement.RangeCalls = 0;
    const auto unsplit = movement.Schedule(jobs, world, 0, 4097);
    verify.Schedule(jobs, world, 0, unsplit).Complete();
    Require(movement.RangeCalls == 1, "One batch must produce exactly one TickRange call");
    World empty;
    movement.Schedule(jobs, empty, 0).Complete();
    Require(movement.RangeCalls == 1, "An empty system must not invoke TickRange");
    bool rejected = false;
    try { world.Data.ForEachRange<Position>(0, 4098, [](Entity, Position&) {}); }
    catch (const std::out_of_range&) { rejected = true; }
    Require(rejected, "Invalid query ranges must be rejected");
    world.Data.ForEachRange<Position>(4097, 4097, [](Entity, Position&) { throw std::runtime_error("Empty range executed"); });
}

int main()
{
    try
    {
        Check(1);
        Check(4);
        std::cout << "Sparse ECS range coverage and ordered system jobs passed (1/4 threads).\n";
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
