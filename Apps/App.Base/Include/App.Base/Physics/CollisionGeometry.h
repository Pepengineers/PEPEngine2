#pragma once

#include "App.Base/Physics/PhysicsTypes.h"
#include "Engine.Core/Types/MeshTypes.h"

// Error is in source-mesh units. Static QEM simplification preserves boundaries.
TriangleMeshShapeDesc BuildStaticCollision(const Engine::Core::Mesh& mesh,
    const std::vector<uint32_t>& subMeshes, float maxErrorWorldUnits);

// Primitive fitting checks convexity, source vertices and volume. Single hulls
// require surface coverage; decomposition is limited by voxel resolution and hull budget.
PhysicsShapeDesc BuildDynamicCollision(const Engine::Core::Mesh& mesh,
    const std::vector<uint32_t>& subMeshes, float maxErrorWorldUnits, unsigned maxHulls = 12);
