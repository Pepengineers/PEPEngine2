#include "App.Base/Physics/JoltPhysicsBackend.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <thread>

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "Common/Logger.h"

#include <Windows.h>

namespace
{
    void JoltTraceImpl(const char* format, ...)
    {
        va_list args;
        va_start(args, format);

        char buffer[1024];
        vsnprintf(buffer, sizeof(buffer), format, args);

        va_end(args);

        Logger::Warn("Jolt: {}", buffer);
    }

#ifdef JPH_ENABLE_ASSERTS
    bool JoltAssertFailedImpl(const char* expression, const char* message, const char* file, JPH::uint line)
    {
        Logger::Error(
            "Jolt assert failed: {}:{}: ({}) {}",
            file,
            line,
            expression ? expression : "",
            message ? message : "");

        return IsDebuggerPresent() != FALSE;
    }
#endif
}

namespace ObjectLayers
{
    static constexpr JPH::ObjectLayer Static = 0;
    static constexpr JPH::ObjectLayer Dynamic = 1;
    static constexpr JPH::ObjectLayer Count = 2;
}

namespace BroadPhaseLayers
{
    static constexpr JPH::BroadPhaseLayer Static(0);
    static constexpr JPH::BroadPhaseLayer Dynamic(1);
    static constexpr uint32_t Count = 2;
}

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
    {
        switch (a)
        {
        case ObjectLayers::Static:
            return b == ObjectLayers::Dynamic;

        case ObjectLayers::Dynamic:
            return true;

        default:
            JPH_ASSERT(false);
            return false;
        }
    }
};

class BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
{
    public:
    BroadPhaseLayerInterfaceImpl()
    {
        _mapping[ObjectLayers::Static] = BroadPhaseLayers::Static;
        _mapping[ObjectLayers::Dynamic] = BroadPhaseLayers::Dynamic;
    }
    
    JPH::uint GetNumBroadPhaseLayers() const override
    {
        return BroadPhaseLayers::Count;
    }
    
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        JPH_ASSERT(layer < ObjectLayers::Count);
        return _mapping[layer];
    }
    
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
    {
        switch ((JPH::BroadPhaseLayer::Type)layer)
        {
        case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::Static:
            return "Static";

        case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::Dynamic:
            return "Dynamic";

        default:
            JPH_ASSERT(false);
            return "Invalid";
        }
    }
#endif
    
private:
    JPH::BroadPhaseLayer _mapping[ObjectLayers::Count];
};

class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer objectLayer, JPH::BroadPhaseLayer broadPhaseLayer) const override
    {
        switch (objectLayer)
        {
            case ObjectLayers::Static:
                return broadPhaseLayer == BroadPhaseLayers::Dynamic;
            case ObjectLayers::Dynamic:
                return true;
            default:
                JPH_ASSERT(false);
                return false;
        }
    }
};

void JoltPhysicsBackend::Initialize()
{
    if (_bInitialized) { return; }
    JPH::Trace = JoltTraceImpl;
#ifdef JPH_ENABLE_ASSERTS
    JPH::AssertFailed = JoltAssertFailedImpl;
#endif

    JPH::RegisterDefaultAllocator();

    JPH_ASSERT(JPH::Factory::sInstance == nullptr);
    if (JPH::Factory::sInstance != nullptr)
    {
        Logger::Error("Jolt physics backend initialization failed: JPH::Factory::sInstance is already initialized");
        return;
    }

    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    _tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10*1024*1024);

    uint32_t hardwareThreads = std::thread::hardware_concurrency();
    int workerThreads = hardwareThreads > 1 ? static_cast<int>(hardwareThreads - 1) : 1;

    _jobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, workerThreads);

    _broadPhaseLayerInterface = std::make_unique<BroadPhaseLayerInterfaceImpl>();
    _objectLayerPairFilter = std::make_unique<ObjectLayerPairFilterImpl>();
    _objectVsBroadPhaseLayerFilter = std::make_unique<ObjectVsBroadPhaseLayerFilterImpl>();

    constexpr uint32_t MaxBodies = 1024;
    constexpr uint32_t NumBodyMutexes = 0;
    constexpr uint32_t MaxBodyPairs = 1024;
    constexpr uint32_t MaxContactConstraints = 1024;

    _physicsSystem = std::make_unique<JPH::PhysicsSystem>();

    _physicsSystem->Init(MaxBodies, NumBodyMutexes, MaxBodyPairs, MaxContactConstraints, *_broadPhaseLayerInterface, *_objectVsBroadPhaseLayerFilter, *_objectLayerPairFilter);
    _bInitialized = true;
    Logger::Info("Jolt physics backend initialized");
}

void JoltPhysicsBackend::Uninitialize()
{
    if (!_bInitialized) { return; }

    _physicsSystem.reset(); // tmp. its better to RemoveBody -> DestroyBody
    _objectLayerPairFilter.reset();
    _objectVsBroadPhaseLayerFilter.reset();
    _broadPhaseLayerInterface.reset();

    _jobSystem.reset();
    _tempAllocator.reset();

    JPH::UnregisterTypes();

    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;

    _bInitialized = false;
}

JoltPhysicsBackend::JoltPhysicsBackend() = default;

JoltPhysicsBackend::~JoltPhysicsBackend()
{
    Uninitialize();
}

void JoltPhysicsBackend::Step(float dt)
{
    if (!_bInitialized || !_physicsSystem)
    {
        return;
    }

    constexpr int CollisionSteps = 1;
    _physicsSystem->Update(dt, CollisionSteps, _tempAllocator.get(), _jobSystem.get());
}
