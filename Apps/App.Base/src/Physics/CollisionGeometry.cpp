#include "App.Base/Physics/CollisionGeometry.h"
#include "Engine.Core/IO/MeshCache.h"
#include "Engine.Core/Logging/Logger.h"
#include "../../../../External/External.Libraries/meshoptimizer/meshoptimizer.h"
#define ENABLE_VHACD_IMPLEMENTATION 1
#include "../../../../External/External.Libraries/vhacd/VHACD.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace
{
    using namespace DirectX;
    using namespace DirectX::SimpleMath;
    using namespace Engine::Core;

    struct Geometry
    {
        std::vector<Vector3> Points;
        std::vector<uint32_t> Indices;
        Vector3 Origin;
    };

    bool Finite(const Vector3& point)
    { return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z); }

    Geometry Gather(const Mesh& mesh, const std::vector<uint32_t>& selected, float error)
    {
        if (!std::isfinite(error) || error <= 0) throw std::invalid_argument("Collision error must be finite and positive");
        Geometry result;
        for (const auto index : selected)
        {
            if (index >= mesh.GetSubMeshCount()) throw std::invalid_argument("Invalid collision submesh index");
            const auto& part = mesh.GetSubMesh(index);
            if (part.Indices.size() % 3 || part.Vertices.size() > UINT32_MAX - result.Points.size())
                throw std::invalid_argument("Invalid collision mesh size");
            const auto offset = static_cast<uint32_t>(result.Points.size());
            for (const auto& vertex : part.Vertices)
            {
                if (!Finite(vertex.Position)) throw std::invalid_argument("Nonfinite collision vertex");
                result.Points.push_back(vertex.Position);
            }
            for (auto vertex : part.Indices)
            {
                if (vertex >= part.Vertices.size()) throw std::invalid_argument("Invalid collision vertex index");
                result.Indices.push_back(offset + vertex);
            }
        }
        if (result.Points.empty() || result.Indices.empty()) throw std::invalid_argument("Empty collision geometry");
        BoundingBox bounds;
        BoundingBox::CreateFromPoints(bounds, result.Points.size(), result.Points.data(), sizeof(Vector3));
        result.Origin = bounds.Center;
        if (!Finite(result.Origin)) throw std::invalid_argument("Collision bounds overflow");
        for (auto& point : result.Points) point -= result.Origin;
        std::vector<uint32_t> remap(result.Points.size());
        meshopt_generatePositionRemap(remap.data(), &result.Points.front().x, result.Points.size(), sizeof(Vector3));
        size_t count = 0;
        for (size_t i = 0; i < result.Indices.size(); i += 3)
        {
            const uint32_t a = remap[result.Indices[i]], b = remap[result.Indices[i + 1]], c = remap[result.Indices[i + 2]];
            if (a == b || b == c || c == a || (result.Points[b] - result.Points[a]).Cross(result.Points[c] - result.Points[a]).LengthSquared() == 0) continue;
            result.Indices[count++] = a; result.Indices[count++] = b; result.Indices[count++] = c;
        }
        result.Indices.resize(count);
        if (!count) throw std::invalid_argument("Collision geometry contains no triangles");
        result.Points.resize(meshopt_optimizeVertexFetch(result.Points.data(), result.Indices.data(), count,
            result.Points.data(), result.Points.size(), sizeof(Vector3)));
        return result;
    }

    void Simplify(Geometry& geometry, float error)
    {
        const auto before = geometry.Indices.size();
        std::vector<uint32_t> reduced(before);
        reduced.resize(meshopt_simplify(reduced.data(), geometry.Indices.data(), before, &geometry.Points.front().x,
            geometry.Points.size(), sizeof(Vector3), 0, error,
            meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute, nullptr));
        if (reduced.empty()) throw std::runtime_error("Collision simplification removed all geometry");
        geometry.Indices = std::move(reduced);
        geometry.Points.resize(meshopt_optimizeVertexFetch(geometry.Points.data(), geometry.Indices.data(), geometry.Indices.size(),
            geometry.Points.data(), geometry.Points.size(), sizeof(Vector3)));
        Logger::Info("Collision mesh simplified: {} -> {} triangles", before / 3, geometry.Indices.size() / 3);
    }

    std::filesystem::path CachePath(const Geometry& geometry, float error, unsigned hulls)
    {
        uint64_t hash = 14695981039346656037ull;
        const auto add = [&](const void* data, size_t size)
        {
            const auto* bytes = static_cast<const unsigned char*>(data);
            for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
        };
        constexpr char version[] = "collision-v5-meshopt1.3-vhacd4-32vertices";
        add(version, sizeof(version)); add(&error, sizeof(error)); add(&hulls, sizeof(hulls));
        add(&geometry.Origin, sizeof(geometry.Origin));
        const uint64_t vertices = geometry.Points.size(), indices = geometry.Indices.size();
        add(&vertices, sizeof(vertices)); add(&indices, sizeof(indices));
        add(geometry.Points.data(), geometry.Points.size() * sizeof(Vector3));
        add(geometry.Indices.data(), geometry.Indices.size() * sizeof(uint32_t));
        std::ostringstream name;
        name << std::hex << std::setfill('0') << std::setw(16) << hash << ".mesh";
        const auto folder = std::filesystem::path(SOLUTION_DIR) / "Build/AssetCache/Collision";
        std::error_code failure;
        std::filesystem::create_directories(folder, failure);
        return failure ? std::filesystem::path{} : folder / name.str();
    }

    SubMesh Store(const Geometry& geometry)
    {
        SubMesh result;
        result.Vertices.resize(geometry.Points.size());
        for (size_t i = 0; i < geometry.Points.size(); ++i) result.Vertices[i].Position = geometry.Points[i] + geometry.Origin;
        result.Indices = geometry.Indices;
        BoundingBox::CreateFromPoints(result.Bounds, result.Vertices.size(), &result.Vertices.front().Position, sizeof(Vertex));
        return result;
    }

    void Save(const std::filesystem::path& path, std::vector<SubMesh> parts)
    {
        if (!path.empty()) SaveMeshCache(path, Mesh(std::move(parts), {}));
    }

    bool ValidParts(const Mesh& mesh, unsigned maxHulls, bool convex = false)
    {
        if (mesh.GetSubMeshCount() == 0 || mesh.GetSubMeshCount() > maxHulls) return false;
        for (const auto& part : mesh.GetSubMeshes())
        {
            if (part.Vertices.empty() || part.Indices.empty() || part.Indices.size() % 3) return false;
            if (convex && (part.Vertices.size() < 4 || part.Vertices.size() > 32)) return false;
            for (const auto& vertex : part.Vertices) if (!Finite(vertex.Position)) return false;
        }
        return true;
    }

    double Volume(const Geometry& geometry)
    {
        double sum = 0;
        for (size_t i = 0; i < geometry.Indices.size(); i += 3)
        {
            const auto& a = geometry.Points[geometry.Indices[i]];
            const auto& b = geometry.Points[geometry.Indices[i + 1]];
            const auto& c = geometry.Points[geometry.Indices[i + 2]];
            sum += double(a.x) * (double(b.y) * c.z - double(b.z) * c.y) +
                double(a.y) * (double(b.z) * c.x - double(b.x) * c.z) + double(a.z) * (double(b.x) * c.y - double(b.y) * c.x);
        }
        return std::abs(sum) / 6;
    }

    std::optional<Geometry> NearConvex(const Geometry& geometry, float error, unsigned& hullBudget)
    {
        std::vector<VHACD::Vertex> points;
        for (const auto& point : geometry.Points) points.emplace_back(point.x, point.y, point.z);
        VHACD::QuickHull quickHull;
        if (!quickHull.ComputeConvexHull(points, 32)) return {};
        Geometry hull;
        hull.Origin = geometry.Origin;
        for (const auto& point : quickHull.GetVertices())
            hull.Points.emplace_back(static_cast<float>(point.mX), static_cast<float>(point.mY), static_cast<float>(point.mZ));
        for (const auto& triangle : quickHull.GetIndices())
            hull.Indices.insert(hull.Indices.end(), {triangle.mI0, triangle.mI1, triangle.mI2});
        if (Volume(hull) <= 0) return {};
        std::vector<VHACD::Triangle> triangles;
        for (size_t i = 0; i < geometry.Indices.size(); i += 3)
            triangles.emplace_back(geometry.Indices[i], geometry.Indices[i+1], geometry.Indices[i+2]);
        const VHACD::AABBTree source(points, triangles);
        double concavity = 0;
        for (const auto& triangle : quickHull.GetIndices())
        {
            const VHACD::Vect3 a(quickHull.GetVertices()[triangle.mI0]);
            const VHACD::Vect3 b(quickHull.GetVertices()[triangle.mI1]);
            const VHACD::Vect3 c(quickHull.GetVertices()[triangle.mI2]);
            for (const VHACD::Vect3 sample : {(a+b+c)/3.0, (a+b)/2.0, (b+c)/2.0, (c+a)/2.0})
            {
                VHACD::Vect3 closest;
                if (!source.GetClosestPointWithinDistance(sample, 1e30, closest)) return {};
                concavity = (std::max)(concavity, (sample-closest).GetNorm());
            }
        }
        if (concavity > error)
        {
            hullBudget = static_cast<unsigned>((std::min)(double(hullBudget), 2 * std::ceil(concavity / error)));
            return {};
        }
        // A volume ratio or a few face samples cannot detect small, off-center openings.
        size_t samples = 0;
        for (const auto& triangle : quickHull.GetIndices())
        {
            const VHACD::Vect3 a(quickHull.GetVertices()[triangle.mI0]);
            const VHACD::Vect3 b(quickHull.GetVertices()[triangle.mI1]);
            const VHACD::Vect3 c(quickHull.GetVertices()[triangle.mI2]);
            const double steps = std::ceil((std::max)({(a-b).GetNorm(), (b-c).GetNorm(), (c-a).GetNorm()}) / (error * 0.5));
            // Reject this fast path when certifying it would cost more than decomposition.
            if (steps > 256 || (samples += size_t(steps+1)*size_t(steps+2)/2) > 200000) return {};
            const unsigned divisions = (std::max)(1u, static_cast<unsigned>(steps));
            for (unsigned i = 0; i <= divisions; ++i) for (unsigned j = 0; j <= divisions-i; ++j)
            {
                const auto sample = a + (b-a)*(double(i)/divisions) + (c-a)*(double(j)/divisions);
                VHACD::Vect3 closest;
                if (!source.GetClosestPointWithinDistance(sample, error * 0.5, closest)) return {};
            }
        }
        return hull;
    }

    struct Frame
    {
        Vector3 Center, HalfExtent;
        Quaternion Rotation;
        std::vector<Vector3> Points;
    };

    Frame Project(const Geometry& geometry, const Quaternion& rotation)
    {
        Frame frame;
        frame.Rotation = rotation;
        Quaternion inverse; rotation.Conjugate(inverse);
        for (const auto& point : geometry.Points) frame.Points.push_back(Vector3::Transform(point, inverse));
        BoundingBox bounds;
        BoundingBox::CreateFromPoints(bounds, frame.Points.size(), frame.Points.data(), sizeof(Vector3));
        const Vector3 center(bounds.Center);
        frame.HalfExtent = bounds.Extents;
        for (auto& point : frame.Points) point -= center;
        frame.Center = Vector3::Transform(center, rotation) + geometry.Origin;
        return frame;
    }

    std::optional<PhysicsCompoundChild> FitPrimitiveCandidate(const Geometry& geometry, float error)
    {
        const double volume = Volume(geometry);
        if (volume <= 0) return {};
        const auto matchesVolume = [&](double candidate) { return candidate > 0 && std::abs(candidate - volume) <= candidate * 0.05; };
        const auto box = [&](const Frame& frame) -> std::optional<PhysicsCompoundChild>
        {
            const auto& h = frame.HalfExtent;
            if (!matchesVolume(8.0 * h.x * h.y * h.z)) return {};
            for (const auto& p : frame.Points)
                if ((std::min)({std::abs(std::abs(p.x) - h.x), std::abs(std::abs(p.y) - h.y), std::abs(std::abs(p.z) - h.z)}) > error) return {};
            return PhysicsCompoundChild{BoxShapeDesc{h}, frame.Center, frame.Rotation};
        };
        BoundingOrientedBox bounds;
        BoundingOrientedBox::CreateFromPoints(bounds, geometry.Points.size(), geometry.Points.data(), sizeof(Vector3));
        if (auto candidate = box(Project(geometry, Quaternion(bounds.Orientation)))) return candidate;
        Vector3 firstNormal;
        bool haveNormal = false;
        for (size_t i = 0; i < geometry.Indices.size(); i += 3)
        {
            auto normal = (geometry.Points[geometry.Indices[i + 1]] - geometry.Points[geometry.Indices[i]]).Cross(
                geometry.Points[geometry.Indices[i + 2]] - geometry.Points[geometry.Indices[i]]);
            if (normal.LengthSquared() == 0) continue;
            normal.Normalize();
            if (!haveNormal) { firstNormal = normal; haveNormal = true; }
            else if (std::abs(normal.Dot(firstNormal)) < 0.01f)
            {
                const auto y = firstNormal.Cross(normal);
                const Matrix basis(normal.x, normal.y, normal.z, 0, y.x, y.y, y.z, 0,
                    firstNormal.x, firstNormal.y, firstNormal.z, 0, 0, 0, 0, 1);
                if (auto candidate = box(Project(geometry, Quaternion::CreateFromRotationMatrix(basis)))) return candidate;
                break;
            }
        }
        const auto sphere = Project(geometry, Quaternion::Identity);
        float radius = 0;
        for (const auto& point : sphere.Points) radius = (std::max)(radius, point.Length());
        if (matchesVolume(4.0 / 3 * XM_PI * radius * radius * radius) &&
            std::all_of(sphere.Points.begin(), sphere.Points.end(), [&](const auto& p) { return std::abs(p.Length() - radius) <= error; }))
            return PhysicsCompoundChild{SphereShapeDesc{radius}, sphere.Center};
        const Vector3 h(bounds.Extents);
        const Vector3 longest = h.x > h.y && h.x > h.z ? Vector3::UnitX : h.z > h.y ? Vector3::UnitZ : Vector3::UnitY;
        const auto axis = Vector3::Transform(longest, Quaternion(bounds.Orientation));
        const auto frame = Project(geometry, Quaternion::FromToRotation(Vector3::UnitY, axis));
        radius = 0;
        for (const auto& p : frame.Points) radius = (std::max)(radius, std::hypot(p.x, p.z));
        const auto half = frame.HalfExtent.y;
        if (matchesVolume(2.0 * XM_PI * radius * radius * half) &&
            std::all_of(frame.Points.begin(), frame.Points.end(), [&](const auto& p)
            { return (std::min)(std::abs(std::hypot(p.x, p.z) - radius), std::abs(std::abs(p.y) - half)) <= error; }))
            return PhysicsCompoundChild{CylinderShapeDesc{radius, half}, frame.Center, frame.Rotation};
        const float segment = half - radius;
        if (segment > 0 && matchesVolume(XM_PI * radius * radius * (2.0 * segment + 4.0 / 3 * radius)) &&
            std::all_of(frame.Points.begin(), frame.Points.end(), [&](const auto& p)
            { return std::abs(Vector3(p.x, (std::max)(0.f, std::abs(p.y) - segment), p.z).Length() - radius) <= error; }))
            return PhysicsCompoundChild{CapsuleShapeDesc{radius, segment}, frame.Center, frame.Rotation};
        return {};
    }

    std::optional<PhysicsCompoundChild> FitPrimitive(const Geometry& geometry, float error, bool convexPart = false)
    {
        auto candidate = FitPrimitiveCandidate(geometry, error);
        if (!candidate || convexPart) return candidate;
        // A primitive may only replace a convex source; inner faces expose gaps even at 99% filled volume.
        for (size_t i = 0; i < geometry.Indices.size(); i += 3)
        {
            const auto precise = [](const Vector3& p) { return VHACD::Vect3(p.x,p.y,p.z); };
            const auto a = precise(geometry.Points[geometry.Indices[i]]);
            auto normal = (precise(geometry.Points[geometry.Indices[i+1]])-a).Cross(precise(geometry.Points[geometry.Indices[i+2]])-a);
            normal.Normalize();
            bool positive = false, negative = false;
            for (const auto& point : geometry.Points)
            {
                const double distance = normal.Dot(precise(point)-a);
                positive |= distance > error * 0.1f;
                negative |= distance < -error * 0.1f;
                if (positive && negative) return {};
            }
        }
        return candidate;
    }

    PhysicsShapeDesc DynamicShape(const std::vector<SubMesh>& parts, float error)
    {
        auto convex = std::make_shared<PhysicsConvexCompound>();
        std::vector<std::optional<PhysicsCompoundChild>> fitted;
        bool anyPrimitive = false;
        for (const auto& part : parts)
        {
            const Mesh mesh({part}, {});
            const auto geometry = Gather(mesh, {0}, error);
            fitted.push_back(FitPrimitive(geometry, error, true));
            anyPrimitive |= fitted.back().has_value();
            auto& hull = convex->Hulls.emplace_back();
            for (const auto& vertex : part.Vertices) hull.push_back(vertex.Position);
        }
        if (!anyPrimitive) return ConvexCompoundShapeDesc{convex};
        auto compound = std::make_shared<PhysicsCompound>();
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (fitted[i]) compound->Children.push_back(*fitted[i]);
            else
            {
                auto hull = std::make_shared<PhysicsConvexCompound>();
                hull->Hulls.push_back(std::move(convex->Hulls[i]));
                compound->Children.push_back({ConvexCompoundShapeDesc{hull}});
            }
        }
        return CompoundShapeDesc{compound};
    }
}

TriangleMeshShapeDesc BuildStaticCollision(const Engine::Core::Mesh& mesh,
    const std::vector<uint32_t>& subMeshes, float maxErrorWorldUnits)
{
    auto geometry = Gather(mesh, subMeshes, maxErrorWorldUnits);
    const auto path = CachePath(geometry, maxErrorWorldUnits, 0);
    auto cached = LoadMeshCache(path);
    std::vector<SubMesh> baked;
    if (cached && ValidParts(*cached, 1))
    {
        Logger::Info("Static collision cache hit: {}", path.filename().string());
    }
    else
    {
        Simplify(geometry, maxErrorWorldUnits);
        baked.push_back(Store(geometry));
        Save(path, baked);
    }
    const auto& part = baked.empty() ? cached->GetSubMesh(0) : baked.front();
    auto result = std::make_shared<PhysicsTriangleMesh>();
    result->Vertices.reserve(part.Vertices.size());
    result->Triangles.reserve(part.Indices.size() / 3);
    for (const auto& vertex : part.Vertices) result->Vertices.push_back(vertex.Position);
    for (size_t i = 0; i < part.Indices.size(); i += 3)
        result->Triangles.push_back({part.Indices[i], part.Indices[i + 1], part.Indices[i + 2]});
    return {result};
}

PhysicsShapeDesc BuildDynamicCollision(const Engine::Core::Mesh& mesh,
    const std::vector<uint32_t>& subMeshes, float maxErrorWorldUnits, unsigned maxHulls)
{
    if (maxHulls == 0 || maxHulls > 64) throw std::invalid_argument("Collision hull budget must be 1..64");
    auto geometry = Gather(mesh, subMeshes, maxErrorWorldUnits);
    if (auto primitive = FitPrimitive(geometry, maxErrorWorldUnits))
    {
        auto compound = std::make_shared<PhysicsCompound>();
        compound->Children.push_back(*primitive);
        return CompoundShapeDesc{compound};
    }
    const auto path = CachePath(geometry, maxErrorWorldUnits, maxHulls);
    if (auto cached = LoadMeshCache(path); cached && ValidParts(*cached, maxHulls, true))
    {
        Logger::Info("Convex collision cache hit: {}", path.filename().string());
        return DynamicShape(cached->GetSubMeshes(), maxErrorWorldUnits);
    }
    unsigned hullBudget = maxHulls;
    if (auto convex = NearConvex(geometry, maxErrorWorldUnits, hullBudget))
    {
        const std::vector<SubMesh> parts{Store(*convex)};
        Save(path, parts);
        return DynamicShape(parts, maxErrorWorldUnits);
    }
    Simplify(geometry, maxErrorWorldUnits * 0.5f);
    std::unique_ptr<VHACD::IVHACD, void(*)(VHACD::IVHACD*)> decomposition(VHACD::CreateVHACD(), [](VHACD::IVHACD* value) { value->Release(); });
    VHACD::IVHACD::Parameters settings;
    settings.m_maxConvexHulls = hullBudget;
    settings.m_maxNumVerticesPerCH = 32;
    settings.m_maxRecursionDepth = 8;
    settings.m_minimumVolumePercentErrorAllowed = 0.01;
    settings.m_asyncACD = false;
    BoundingBox bounds;
    BoundingBox::CreateFromPoints(bounds, geometry.Points.size(), geometry.Points.data(), sizeof(Vector3));
    const auto size = Vector3(bounds.Extents) * 2;
    const double cells = (std::max)({size.x, size.y, size.z}) / double(maxErrorWorldUnits);
    settings.m_resolution = static_cast<uint32_t>(std::clamp(cells * cells * cells, 100000.0, 1000000.0));
    if (!decomposition->Compute(&geometry.Points.front().x, static_cast<uint32_t>(geometry.Points.size()), geometry.Indices.data(),
        static_cast<uint32_t>(geometry.Indices.size() / 3), settings) || decomposition->GetNConvexHulls() == 0)
        throw std::runtime_error("Automatic convex collision decomposition failed");
    if (decomposition->GetNConvexHulls() > maxHulls) throw std::runtime_error("Automatic collision exceeded its hull budget");
    std::vector<SubMesh> parts;
    for (uint32_t i = 0; i < decomposition->GetNConvexHulls(); ++i)
    {
        VHACD::IVHACD::ConvexHull hull;
        if (!decomposition->GetConvexHull(i, hull) || hull.m_points.size() < 4 || hull.m_points.size() > 32)
            throw std::runtime_error("Invalid automatic collision hull");
        Geometry output;
        output.Origin = geometry.Origin;
        for (const auto& p : hull.m_points) output.Points.emplace_back(static_cast<float>(p.mX), static_cast<float>(p.mY), static_cast<float>(p.mZ));
        for (const auto& triangle : hull.m_triangles) output.Indices.insert(output.Indices.end(), {triangle.mI0, triangle.mI1, triangle.mI2});
        parts.push_back(Store(output));
    }
    Logger::Info("Automatic collision decomposition: {} hulls, at most 32 vertices per hull", parts.size());
    Save(path, parts);
    return DynamicShape(parts, maxErrorWorldUnits);
}
