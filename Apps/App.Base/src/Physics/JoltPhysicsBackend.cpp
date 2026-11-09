#include "App.Base/Physics/JoltPhysicsBackend.h"

#include <cstdarg>
#include <cstdio>
#include <stdexcept>
#include "App.Base/Physics/JoltJobSystem.h"
#include <cmath>
#include <type_traits>
#include <algorithm>
#include <map>
#include <tuple>
#include <limits>

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/AABBTree/TriangleCodec/TriangleCodecIndexed8BitPackSOA4Flags.h>

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

    JPH::Shape::ShapeResult CreateShape(const PhysicsShapeDesc& desc, const DirectX::SimpleMath::Vector3& scale)
    {
        if (!std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z) ||
            scale.x <= 0 || scale.y <= 0 || scale.z <= 0)
        {
            JPH::Shape::ShapeResult result;
            result.SetError("Invalid shape scale");
            return result;
        }
        return std::visit([&scale](const auto& shape) -> JPH::Shape::ShapeResult
        {
            using T = std::decay_t<decltype(shape)>;

            const auto isPositive = [](float value) { return std::isfinite(value) && value > 0.0f; };

            const auto invalidShape = []()
            {
                JPH::Shape::ShapeResult result;
                result.SetError("Invalid shape dimensions");
                return result;
            };

            if constexpr (std::is_same_v<T, BoxShapeDesc>)
            {
                DirectX::SimpleMath::Vector3 halfExtent(
                    shape.HalfExtent.x * scale.x,
                    shape.HalfExtent.y * scale.y,
                    shape.HalfExtent.z * scale.z);
                if (!isPositive(halfExtent.x) || !isPositive(halfExtent.y) || !isPositive(halfExtent.z) ||
                    !std::isfinite(shape.ConvexRadiusMeters) || shape.ConvexRadiusMeters < 0) return invalidShape();
                return JPH::BoxShapeSettings(JPH::Vec3(halfExtent.x, halfExtent.y, halfExtent.z),
                    (std::min)({shape.ConvexRadiusMeters, halfExtent.x * 0.1f, halfExtent.y * 0.1f, halfExtent.z * 0.1f})).Create();
            }

            else if constexpr (std::is_same_v<T, SphereShapeDesc>)
            {
                const float radius = shape.Radius * scale.x;
                if (!isPositive(radius)) { return invalidShape(); }
                return JPH::SphereShapeSettings(radius).Create();
            }

            else if constexpr (std::is_same_v<T, CylinderShapeDesc>)
            {
                const float radius = shape.Radius * scale.x;
                const float halfHeight = shape.HalfHeight * scale.y;
                if (!isPositive(radius) || !isPositive(halfHeight)) { return invalidShape(); }
                return JPH::CylinderShapeSettings(halfHeight, radius,
                    (std::min)({0.05f, radius * 0.1f, halfHeight * 0.1f})).Create();
            }

            else if constexpr (std::is_same_v<T, CapsuleShapeDesc>)
            {
                static_assert(std::is_same_v<T, CapsuleShapeDesc>, "Unsupported shape type");
                const float radius = shape.Radius * scale.x;
                const float halfCylinderHeight = shape.HalfCylinderHeight * scale.y;
                if (!isPositive(radius) || (!std::isfinite(halfCylinderHeight) || halfCylinderHeight < 0)) { return invalidShape(); }
                return JPH::CapsuleShapeSettings(halfCylinderHeight, radius).Create();
            }
            else if constexpr (std::is_same_v<T, TriangleMeshShapeDesc>)
            {
                if (!shape.Geometry || shape.Geometry->Triangles.empty()) return invalidShape();
                JPH::VertexList vertices;
                vertices.reserve(shape.Geometry->Vertices.size());
                for (const auto& p : shape.Geometry->Vertices)
                {
                    const auto position = p * scale;
                    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) return invalidShape();
                    vertices.emplace_back(position.x, position.y, position.z);
                }
                JPH::IndexedTriangleList triangles;
                triangles.reserve(shape.Geometry->Triangles.size());
                JPH::AABox bounds;
                for (const auto& t : shape.Geometry->Triangles)
                {
                    if (t[0] >= vertices.size() || t[1] >= vertices.size() || t[2] >= vertices.size()) return invalidShape();
                    triangles.emplace_back(t[0], t[1], t[2]);
                    for (const auto index : t) bounds.Encapsulate(JPH::Vec3(vertices[index]));
                }
                // A nonzero source area can collapse after 21-bit compression and body-space transforms.
                const float precision = (std::max)(bounds.GetSize().Length() / JPH::TriangleCodecIndexed8BitPackSOA4Flags::COMPONENT_MASK,
                    JPH::Vec3::sMax(bounds.mMin.Abs(), bounds.mMax.Abs()).ReduceMax() * std::numeric_limits<float>::epsilon());
                if (!std::isfinite(precision)) return invalidShape();
                const float minimumHeight = 8 * precision;
                const auto end = std::remove_if(triangles.begin(), triangles.end(), [&](const auto& triangle)
                {
                    const JPH::Vec3 a(vertices[triangle.mIdx[0]]), b(vertices[triangle.mIdx[1]]), c(vertices[triangle.mIdx[2]]);
                    const auto ab = b-a, ac = c-a, bc = c-b;
                    const float edgeSquared = (std::max)({ab.LengthSq(), ac.LengthSq(), bc.LengthSq()});
                    const float areaSquared = ab.Cross(ac).LengthSq();
                    // Keep a margin above Jolt's 1e-12 squared-normal cutoff, even after rotations.
                    return !std::isfinite(areaSquared) || areaSquared <= (std::max)(16e-12f, edgeSquared * minimumHeight * minimumHeight);
                });
                if (end != triangles.end()) Logger::Warn("Removed {} numerically unstable collision triangles", triangles.end()-end);
                triangles.erase(end, triangles.end());
                return JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)).Create();
            }
            else if constexpr (std::is_same_v<T, ConvexCompoundShapeDesc>)
            {
                if (!shape.Geometry || shape.Geometry->Hulls.empty() || !std::isfinite(shape.Geometry->ConvexRadiusMeters) ||
                    shape.Geometry->ConvexRadiusMeters < 0) return invalidShape();
                JPH::StaticCompoundShapeSettings compound;
                for (const auto& hull : shape.Geometry->Hulls)
                {
                    if (hull.size() < 3) return invalidShape();
                    auto minimum = hull.front(), maximum = hull.front();
                    for (const auto& p : hull)
                    {
                        minimum = DirectX::SimpleMath::Vector3((std::min)(minimum.x,p.x), (std::min)(minimum.y,p.y), (std::min)(minimum.z,p.z));
                        maximum = DirectX::SimpleMath::Vector3((std::max)(maximum.x,p.x), (std::max)(maximum.y,p.y), (std::max)(maximum.z,p.z));
                    }
                    const auto center = (minimum + maximum) * 0.5f;
                    JPH::Array<JPH::Vec3> points;
                    for (const auto& p : hull) points.emplace_back((p.x-center.x) * scale.x, (p.y-center.y) * scale.y, (p.z-center.z) * scale.z);
                    // Cook near the origin: baked world coordinates lose precision on thin furniture parts.
                    JPH::ConvexHullShapeSettings settings(points, shape.Geometry->ConvexRadiusMeters);
                    settings.mHullTolerance = 0.0001f;
                    const auto result = settings.Create();
                    if (result.HasError())
                    {
                        Logger::Error("Convex hull failed at center ({}, {}, {}), extent ({}, {}, {})", center.x, center.y, center.z,
                            maximum.x-minimum.x, maximum.y-minimum.y, maximum.z-minimum.z);
                        return result;
                    }
                    compound.AddShape(JPH::Vec3(center.x*scale.x,center.y*scale.y,center.z*scale.z), JPH::Quat::sIdentity(), result.Get());
                }
                return compound.Create();
            }
            else
            {
                if (!shape.Geometry || shape.Geometry->Children.empty()) return invalidShape();
                const float uniform = (std::max)({scale.x, scale.y, scale.z});
                JPH::StaticCompoundShapeSettings compound;
                for (const auto& child : shape.Geometry->Children)
                {
                    const auto& p = child.Position;
                    const auto& q = child.Rotation;
                    const float lengthSquared = q.LengthSquared();
                    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
                        !std::isfinite(lengthSquared) || lengthSquared <= 0) return invalidShape();
                    const auto result = std::visit([&](const auto& primitive)
                    {
                        return CreateShape(primitive, DirectX::SimpleMath::Vector3(uniform));
                    }, child.Shape);
                    if (result.HasError()) return result;
                    const auto position = p * uniform;
                    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) return invalidShape();
                    compound.AddShape(JPH::Vec3(position.x, position.y, position.z),
                        JPH::Quat(q.x, q.y, q.z, q.w).Normalized(), result.Get());
                }
                const auto result = compound.Create();
                if (result.HasError()) return result;
                // Parent nonuniform scale must also transform rotated child axes.
                const JPH::Vec3 residual(scale.x / uniform, scale.y / uniform, scale.z / uniform);
                if (!result.Get()->IsValidScale(residual)) return invalidShape();
                return result.Get()->ScaleShape(residual);
            }

        },
        desc);
    }
}

struct JoltShapeCache
{
    struct Key
    {
        const void* Geometry;
        size_t Type;
        DirectX::SimpleMath::Vector3 Scale;
        bool operator<(const Key& other) const
        {
            if (Geometry != other.Geometry) return std::less<const void*>{}(Geometry, other.Geometry);
            return std::tie(Type, Scale.x, Scale.y, Scale.z) < std::tie(other.Type, other.Scale.x, other.Scale.y, other.Scale.z);
        }
    };
    struct Entry
    {
        std::shared_ptr<const void> Geometry;
        JPH::Shape::ShapeResult Shape;
    };
    std::map<Key, Entry> Shapes;

    JPH::Shape::ShapeResult Get(const PhysicsShapeDesc& desc, const DirectX::SimpleMath::Vector3& scale)
    {
        const auto geometry = std::visit([](const auto& shape) -> std::shared_ptr<const void>
        {
            using T = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<T, TriangleMeshShapeDesc> || std::is_same_v<T, ConvexCompoundShapeDesc> || std::is_same_v<T, CompoundShapeDesc>)
                return shape.Geometry;
            else return {};
        }, desc);
        if (!geometry || !std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z) ||
            scale.x <= 0 || scale.y <= 0 || scale.z <= 0) return CreateShape(desc, scale);
        const Key key{geometry.get(), desc.index(), scale};
        const auto found = Shapes.find(key);
        if (found != Shapes.end()) return found->second.Shape;
        auto result = CreateShape(desc, scale);
        if (!result.HasError()) Shapes.emplace(key, Entry{geometry, result});
        return result;
    }
};

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

struct JoltRuntime
{
    JoltRuntime()
    {
        if (!JPH::VerifyJoltVersionID()) throw std::runtime_error("Jolt library and client build definitions do not match");
        JPH::RegisterDefaultAllocator();
        if (JPH::Factory::sInstance) throw std::logic_error("Jolt factory is already owned by another runtime");
        JPH::Trace = JoltTraceImpl;
#ifdef JPH_ENABLE_ASSERTS
        JPH::AssertFailed = JoltAssertFailedImpl;
#endif
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
    ~JoltRuntime()
    {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
};

void JoltPhysicsBackend::Initialize(JobSystem& jobs, const PhysicsWorldSettings& settings)
{
    if (_bInitialized) return;
    if (!std::isfinite(settings.WorldUnitsPerMeter) || settings.WorldUnitsPerMeter <= 0 ||
        !std::isfinite(settings.PenetrationSlopMeters) || settings.PenetrationSlopMeters <= 0 || settings.PenetrationSlopMeters > 1 ||
        settings.CollisionSteps == 0 || settings.CollisionSteps > 8 ||
        settings.MaxBodies == 0 || settings.MaxBodies > 65536 || settings.MaxBodyPairs == 0 ||
        settings.MaxContactConstraints == 0 || settings.MaxContactConstraints > UINT32_MAX / 512)
        throw std::invalid_argument("Invalid physics world settings");
    _settings = settings;
    static std::weak_ptr<JoltRuntime> runtime;
    _runtime = runtime.lock();
    if (!_runtime) runtime = _runtime = std::make_shared<JoltRuntime>();
    _shapeCache = std::make_unique<JoltShapeCache>();

    _tempAllocator = std::make_unique<JPH::TempAllocatorImplWithMallocFallback>(
        (std::max)(10u * 1024 * 1024, settings.MaxContactConstraints * 512));

    _jobSystem = std::make_unique<JoltJobSystem>(jobs);

    _broadPhaseLayerInterface = std::make_unique<BroadPhaseLayerInterfaceImpl>();
    _objectLayerPairFilter = std::make_unique<ObjectLayerPairFilterImpl>();
    _objectVsBroadPhaseLayerFilter = std::make_unique<ObjectVsBroadPhaseLayerFilterImpl>();

    _physicsSystem = std::make_unique<JPH::PhysicsSystem>();
    _physicsSystem->Init(settings.MaxBodies, 0, settings.MaxBodyPairs, settings.MaxContactConstraints,
        *_broadPhaseLayerInterface, *_objectVsBroadPhaseLayerFilter, *_objectLayerPairFilter);
    auto physicsSettings = _physicsSystem->GetPhysicsSettings();
    physicsSettings.mPenetrationSlop = settings.PenetrationSlopMeters;
    physicsSettings.mSpeculativeContactDistance = (std::min)(physicsSettings.mSpeculativeContactDistance, 2 * settings.PenetrationSlopMeters);
    _physicsSystem->SetPhysicsSettings(physicsSettings);
    _bInitialized = true;
    Logger::Info("Jolt physics backend initialized");
}

void JoltPhysicsBackend::Uninitialize()
{
    _physicsSystem.reset();
    _shapeCache.reset();
    _objectLayerPairFilter.reset();
    _objectVsBroadPhaseLayerFilter.reset();
    _broadPhaseLayerInterface.reset();

    _jobSystem.reset();
    _tempAllocator.reset();

    _runtime.reset();

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

    if (!std::isfinite(dt) || dt <= 0) return;
    const auto error = _physicsSystem->Update(dt, static_cast<int>(_settings.CollisionSteps), _tempAllocator.get(), _jobSystem.get());
    if (error != JPH::EPhysicsUpdateError::None) throw std::runtime_error("Jolt physics capacity exceeded during update");

}

#ifdef _DEBUG
void JoltPhysicsBackend::CollectCollisionTriangles(std::vector<DirectX::SimpleMath::Vector3>& statics,
    std::vector<DirectX::SimpleMath::Vector3>& moving) const
{
    if (!_physicsSystem) return;
    JPH::BodyIDVector bodies;
    _physicsSystem->GetBodies(bodies);
    JPH::AllHitCollisionCollector<JPH::TransformedShapeCollector> leaves;
    constexpr int batchSize = 64;
    JPH::Float3 vertices[3 * batchSize];
    const float units = _settings.WorldUnitsPerMeter;
    for (const auto id : bodies)
    {
        JPH::BodyLockRead lock(_physicsSystem->GetBodyLockInterface(), id);
        if (!lock.SucceededAndIsInBroadPhase()) continue;
        const auto& body = lock.GetBody();
        const auto bounds = body.GetWorldSpaceBounds();
        auto& output = body.IsStatic() ? statics : moving;
        leaves.Reset();
        body.GetTransformedShape().CollectTransformedShapes(bounds, leaves);
        for (const auto& leaf : leaves.mHits)
        {
            JPH::Shape::GetTrianglesContext context;
            leaf.GetTrianglesStart(context, bounds, JPH::RVec3::sZero());
            while (const int count = leaf.GetTrianglesNext(context, batchSize, vertices))
                for (int i = 0; i < 3 * count; ++i)
                    output.emplace_back(vertices[i].x * units, vertices[i].y * units, vertices[i].z * units);
        }
    }
}
#endif

PhysicsBodyHandle JoltPhysicsBackend::CreateBody(const PhysicsBodyDesc& desc)
{
    if (!_bInitialized || !_physicsSystem)
    {
        Logger::Error("CreateBody(): backend isnt initialized");
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
        Logger::Error("CreateBody(): invalid motion type");
        return {};
    }

    const bool isStatic = desc.MotionType == PhysicsMotionType::Static;

    const auto shapeResult = _shapeCache->Get(desc.Shape, desc.Scale / _settings.WorldUnitsPerMeter);
    if (shapeResult.HasError())
    {
        Logger::Error("CreateBody(): {}", shapeResult.GetError().c_str());
        return {};
    }
    JPH::ShapeRefC shape = shapeResult.Get();
    if (!isStatic && shape->MustBeStatic())
        throw std::invalid_argument("Triangle mesh bodies and compounds must be static");

    JPH::BodyCreationSettings settings(
        shape,
        JPH::RVec3(desc.Position.x, desc.Position.y, desc.Position.z) / _settings.WorldUnitsPerMeter,
        JPH::Quat(desc.Rotation.x, desc.Rotation.y, desc.Rotation.z, desc.Rotation.w),
        motionType,
        isStatic ? ObjectLayers::Static : ObjectLayers::Dynamic
    );

    if (desc.ContinuousCollision && !isStatic) settings.mMotionQuality = JPH::EMotionQuality::LinearCast;

    JPH::BodyInterface& bodyInterface = _physicsSystem->GetBodyInterface();

    JPH::Body* body = bodyInterface.CreateBody(settings);
    if (!body)
    {
        Logger::Error("CreateBody(): failed to create");
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
        static_cast<float>(position.GetX()) * _settings.WorldUnitsPerMeter,
        static_cast<float>(position.GetY()) * _settings.WorldUnitsPerMeter,
        static_cast<float>(position.GetZ()) * _settings.WorldUnitsPerMeter);

    outRotation = DirectX::SimpleMath::Quaternion(
       rotation.GetX(),
       rotation.GetY(),
       rotation.GetZ(),
       rotation.GetW());

    return true;
}

void JoltPhysicsBackend::MoveKinematicBody(PhysicsBodyHandle handle, const DirectX::SimpleMath::Vector3& targetPosition,
    const DirectX::SimpleMath::Quaternion& targetRotation, float dt)
{
    if (!_bInitialized || !_physicsSystem || !handle.IsValid() || !std::isfinite(dt) || dt <= 0) { return; }

    const JPH::BodyID bodID(handle.Value);
    auto& bodyInterface = _physicsSystem->GetBodyInterface();

    if (bodyInterface.GetMotionType(bodID) != JPH::EMotionType::Kinematic) { return; }

    bodyInterface.MoveKinematic(
        bodID,
        JPH::RVec3(
            targetPosition.x / _settings.WorldUnitsPerMeter,
            targetPosition.y / _settings.WorldUnitsPerMeter,
            targetPosition.z / _settings.WorldUnitsPerMeter),
        JPH::Quat(
            targetRotation.x,
            targetRotation.y,
            targetRotation.z,
            targetRotation.w),
        dt);
}

bool JoltPhysicsBackend::OverlapsBox(const DirectX::SimpleMath::Vector3& center,
    const DirectX::SimpleMath::Vector3& halfExtent) const
{
    if (!_physicsSystem) throw std::logic_error("Physics backend is not initialized");
    const auto shape = CreateShape(BoxShapeDesc{halfExtent}, DirectX::SimpleMath::Vector3(1 / _settings.WorldUnitsPerMeter));
    if (shape.HasError()) throw std::invalid_argument("Invalid placement box");
    JPH::AnyHitCollisionCollector<JPH::CollideShapeCollector> collector;
    JPH::CollideShapeSettings settings;
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    const JPH::RVec3 position(center.x / _settings.WorldUnitsPerMeter,
        center.y / _settings.WorldUnitsPerMeter, center.z / _settings.WorldUnitsPerMeter);
    _physicsSystem->GetNarrowPhaseQuery().CollideShape(shape.Get(), JPH::Vec3::sReplicate(1),
        JPH::RMat44::sTranslation(position), settings, position, collector);
    return collector.HadHit();
}

bool JoltPhysicsBackend::OverlapsBody(const PhysicsBodyDesc& desc)
{
    if (!_physicsSystem) throw std::logic_error("Physics backend is not initialized");
    const auto& p = desc.Position;
    const auto& q = desc.Rotation;
    const JPH::Quat rotation(q.x, q.y, q.z, q.w);
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
        !std::isfinite(q.LengthSquared()) || !rotation.IsNormalized())
        throw std::invalid_argument("Invalid placement transform");
    const auto result = _shapeCache->Get(desc.Shape, desc.Scale / _settings.WorldUnitsPerMeter);
    if (result.HasError() || result.Get()->MustBeStatic())
        throw std::invalid_argument("Placement query requires a valid movable shape");
    const auto* shape = result.Get().GetPtr();
    const JPH::RVec3 position = JPH::RVec3(p.x, p.y, p.z) / _settings.WorldUnitsPerMeter;
    // Jolt queries take a COM transform; descriptors and ECS transforms use the original mesh pivot.
    const auto transform = JPH::RMat44::sRotationTranslation(rotation, position).PreTranslated(shape->GetCenterOfMass());
    JPH::CollideShapeSettings settings;
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::AnyHitCollisionCollector<JPH::CollideShapeCollector> collector;
    _physicsSystem->GetNarrowPhaseQuery().CollideShape(shape, JPH::Vec3::sReplicate(1),
        transform, settings, transform.GetTranslation(), collector);
    return collector.HadHit();
}

bool JoltPhysicsBackend::AddBodyVelocityImpulse(PhysicsBodyHandle handle,
    const DirectX::SimpleMath::Vector3& deltaVelocity, float maxSpeed)
{
    if (!std::isfinite(deltaVelocity.LengthSquared()) || !std::isfinite(maxSpeed) || maxSpeed <= 0 || maxSpeed > 1000000)
        throw std::invalid_argument("Invalid body drive");
    if (!_physicsSystem || !handle.IsValid()) return false;
    const JPH::BodyID id(handle.Value);
    {
        JPH::BodyLockWrite lock(_physicsSystem->GetBodyLockInterface(), id);
        if (!lock.Succeeded() || !lock.GetBody().IsDynamic()) return false;
        auto& body = lock.GetBody();
        auto* motion = body.GetMotionProperties();
        if (motion->GetInverseMass() <= 0) return false;
        const float speed = maxSpeed / _settings.WorldUnitsPerMeter;
        motion->SetMaxLinearVelocity(speed);
        body.SetLinearVelocityClamped(body.GetLinearVelocity());
        const auto previous = body.GetLinearVelocity();
        auto target = previous + JPH::Vec3(deltaVelocity.x, deltaVelocity.y, deltaVelocity.z) / _settings.WorldUnitsPerMeter;
        if (target.LengthSq() > speed * speed) target = target.Normalized() * speed;
        const auto impulse = (target - previous) / motion->GetInverseMass();
        body.AddImpulse(impulse);
    }
    _physicsSystem->GetBodyInterface().ActivateBody(id);
    return true;
}

bool JoltPhysicsBackend::RayCastStatic(const DirectX::SimpleMath::Vector3& origin,
    const DirectX::SimpleMath::Vector3& displacement, PhysicsRayHit& hit) const
{
    if (!std::isfinite(origin.LengthSquared()) || !std::isfinite(displacement.LengthSquared()) || displacement.LengthSquared() <= 0)
        throw std::invalid_argument("Invalid static ray query");
    if (!_physicsSystem) throw std::logic_error("Physics backend is not initialized");
    const JPH::RRayCast ray(JPH::RVec3(origin.x,origin.y,origin.z)/_settings.WorldUnitsPerMeter,
        JPH::Vec3(displacement.x,displacement.y,displacement.z)/_settings.WorldUnitsPerMeter);
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
    _physicsSystem->GetNarrowPhaseQuery().CastRay(ray,JPH::RayCastSettings{},collector,{},JPH::SpecifiedObjectLayerFilter(ObjectLayers::Static));
    if (!collector.HadHit()) return false;
    JPH::BodyLockRead lock(_physicsSystem->GetBodyLockInterface(),collector.mHit.mBodyID);
    if (!lock.Succeeded()) return false;
    const auto position=ray.GetPointOnRay(collector.mHit.mFraction);
    const auto normal=lock.GetBody().GetWorldSpaceSurfaceNormal(collector.mHit.mSubShapeID2,position);
    hit.Position=DirectX::SimpleMath::Vector3(position.GetX(),position.GetY(),position.GetZ())*_settings.WorldUnitsPerMeter;
    hit.Normal=DirectX::SimpleMath::Vector3(normal.GetX(),normal.GetY(),normal.GetZ());
    return true;
}

bool JoltPhysicsBackend::TryGetBodyVelocity(PhysicsBodyHandle handle,
    DirectX::SimpleMath::Vector3& velocity, bool& active) const
{
    if (!_physicsSystem || !handle.IsValid()) return false;
    JPH::BodyLockRead lock(_physicsSystem->GetBodyLockInterface(), JPH::BodyID(handle.Value));
    if (!lock.Succeeded()) return false;
    const auto& body = lock.GetBody();
    const auto v = body.GetLinearVelocity();
    velocity = DirectX::SimpleMath::Vector3(v.GetX(), v.GetY(), v.GetZ()) * _settings.WorldUnitsPerMeter;
    active = body.IsActive();
    return true;
}
