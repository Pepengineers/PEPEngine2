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
// test
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Body/BodyLock.h>

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

    // test
    CreateFloor();
    CreateBox();
}

void JoltPhysicsBackend::Uninitialize()
{
    if (!_bInitialized) { return; }

    // test
    DestroyBody(_boxBody);
    _boxBody = {};

    DestroyBody(_floorBody);
    _floorBody = {};

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
    
    // test
    ReadBoxPosition();
}

PhysicsBodyHandle JoltPhysicsBackend::CreateBoxBody(const PhysicsBoxBodyDesc& desc)
{
    if (!_bInitialized || !_physicsSystem)
    {
        Logger::Error("CreateBoxBody(): backend isnt initialized");
        return {};
    }
    
    JPH::EMotionType motionType;

    switch (desc.MotionType)
    {
    case PhysicsMotionType::Static:
        motionType = JPH::EMotionType::Static;
        break;
    case PhysicsMotionType::Dynamic:
        motionType = JPH::EMotionType::Dynamic;
        break;
    case PhysicsMotionType::Kinematic:
        motionType = JPH::EMotionType::Kinematic;
        break;
        
    default:
        Logger::Error("CreateBoxBody(): invalid motion type");
        return {};
    }
    
    const bool isStatic = desc.MotionType == PhysicsMotionType::Static;
    
    JPH::BodyCreationSettings settings(
        new JPH::BoxShape(JPH::Vec3(desc.HalfExtent.x, desc.HalfExtent.y, desc.HalfExtent.z)),
        JPH::RVec3(desc.Position.x, desc.Position.y, desc.Position.z),
        JPH::Quat(desc.Rotation.x, desc.Rotation.y, desc.Rotation.z, desc.Rotation.w),
        motionType,
        isStatic ? ObjectLayers::Static : ObjectLayers::Dynamic
    );
    
    JPH::BodyInterface& bodyInterface = _physicsSystem->GetBodyInterface();
    
    JPH::Body* body = bodyInterface.CreateBody(settings);
    if (!body)
    {
        Logger::Error("CreateBoxBody(): failed to create");
        return {};
    }
    
    const JPH::BodyID& bodyID = body->GetID();
    bodyInterface.AddBody(bodyID, isStatic ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
    
    return PhysicsBodyHandle{ bodyID.GetIndexAndSequenceNumber() };
}

void JoltPhysicsBackend::DestroyBody(PhysicsBodyHandle handle)
{
    if (!_bInitialized || !_physicsSystem || !handle.IsValid())
    {
        Logger::Error("DestroyBody(): backend isn't initialized or invalid handle");
        return;
    }
    
    const JPH::BodyID bodID(handle.Value);
    
    JPH::BodyInterface& bodyInterface = _physicsSystem->GetBodyInterface();
    
    bodyInterface.RemoveBody(bodID);
    bodyInterface.DestroyBody(bodID);
}

bool JoltPhysicsBackend::TryGetBodyTransform(PhysicsBodyHandle handle, DirectX::SimpleMath::Vector3& outPosition,
    DirectX::SimpleMath::Quaternion& outRotation) const
{
    if (!_bInitialized || !_physicsSystem || !handle.IsValid()) { return false; }
    
    const JPH::BodyID bodID(handle.Value);
    JPH::BodyLockRead lock(_physicsSystem->GetBodyLockInterface(), bodID);
    
    if (!lock.Succeeded()) { return false; }
    
    const JPH::Body& body = lock.GetBody();
    const JPH::RVec3 position = body.GetPosition();
    const JPH::Quat rotation = body.GetRotation();
    
    outPosition = DirectX::SimpleMath::Vector3(
        static_cast<float>(position.GetX()),
        static_cast<float>(position.GetY()),
        static_cast<float>(position.GetZ()));
    
    outRotation = DirectX::SimpleMath::Quaternion(
       rotation.GetX(),
       rotation.GetY(),
       rotation.GetZ(),
       rotation.GetW());
    
    return true;
}

// test
void JoltPhysicsBackend::CreateFloor()
{
    PhysicsBoxBodyDesc desc;
    desc.HalfExtent = DirectX::SimpleMath::Vector3{ 20.0f, 1.0f, 20.0f };
    desc.Position = DirectX::SimpleMath::Vector3{ 0.0f, -1.0f, 0.0f };
    desc.MotionType = PhysicsMotionType::Static;
    
    _floorBody = CreateBoxBody(desc);
}

void JoltPhysicsBackend::CreateBox()
{
    PhysicsBoxBodyDesc desc;
    desc.HalfExtent = DirectX::SimpleMath::Vector3{ 1.0f, 1.0f, 1.0f };
    desc.Position = DirectX::SimpleMath::Vector3{ 0.0f, 20.0f, 0.0f };
    desc.MotionType = PhysicsMotionType::Dynamic;

    _boxBody = CreateBoxBody(desc);
}

void JoltPhysicsBackend::ReadBoxPosition()
{
    if (!_boxBody.IsValid())
    {
        return;
    }

    const JPH::BodyID boxBodyID(_boxBody.Value);
    
    JPH::BodyInterface& bodyInterface = _physicsSystem->GetBodyInterface();
    
    JPH::RVec3 boxPosition = bodyInterface.GetPosition(boxBodyID);
    JPH::Vec3 outLinearVelocity;
    JPH::Vec3 outRotationalVelocity;
    bodyInterface.GetLinearAndAngularVelocity(boxBodyID, outLinearVelocity, outRotationalVelocity);
    
    Logger::Info(
        "Box:\n"
        "x={}, y={}, z={}\n"
        "vx={}, vy={}, vz={}\n"
        "ax={}, ay={}, az={}\n",
        static_cast<float>(boxPosition.GetX()),
        static_cast<float>(boxPosition.GetY()),
        static_cast<float>(boxPosition.GetZ()),
        static_cast<float>(outLinearVelocity.GetX()),
        static_cast<float>(outLinearVelocity.GetY()),
        static_cast<float>(outLinearVelocity.GetZ()),
        static_cast<float>(outRotationalVelocity.GetX()),
        static_cast<float>(outRotationalVelocity.GetY()),
        static_cast<float>(outRotationalVelocity.GetZ())
        );
    
}
