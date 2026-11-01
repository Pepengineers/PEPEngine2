// AssimpImportHelpers.h

#pragma once

#include <Engine.Core/IO/ImportPolicies.h>

#include <assimp/postprocess.h>
#include <assimp/matrix4x4.h>
#include <directxtk/SimpleMath.h>
#include <cmath>
#include <stdexcept>

namespace Engine::Core
{
	inline DirectX::SimpleMath::Matrix ImportAssimpMatrix(const aiMatrix4x4& matrix)
	{
		DirectX::SimpleMath::Matrix result(
			matrix.a1, matrix.b1, matrix.c1, matrix.d1,
			matrix.a2, matrix.b2, matrix.c2, matrix.d2,
			matrix.a3, matrix.b3, matrix.c3, matrix.d3,
			matrix.a4, matrix.b4, matrix.c4, matrix.d4);
		const auto* values = reinterpret_cast<const float*>(&result);
		for (size_t i = 0; i < 16; ++i)
			if (!std::isfinite(values[i])) throw std::runtime_error("Non-finite imported matrix");
		return result;
	}

	/// Builds an Assimp post-process flags bitmask from mesh import options.
	/// This helper is shared by mesh and scene importers so they stay consistent
	/// when using the same baseline import policy.
	[[nodiscard]] inline std::uint32_t BuildAssimpPostProcessFlags(const MeshImportOptions& options)
	{
		std::uint32_t assimpFlags = 0;

		if (options.bTriangulate)
		{
			assimpFlags |= aiProcess_Triangulate;
		}

		if (options.bFlipUVs)
		{
			assimpFlags |= aiProcess_FlipUVs;
		}

		if (options.bGenerateNormals)
		{
			assimpFlags |= aiProcess_GenNormals;
		}

		if (options.bGenerateTangents)
		{
			assimpFlags |= aiProcess_CalcTangentSpace;
		}

		return assimpFlags;
	}

	/// Returns the default Assimp post-process flags used for scene import.
	/// At the moment scenes use the same baseline geometry-processing policy
	/// as default mesh import.
	[[nodiscard]] inline std::uint32_t BuildDefaultSceneAssimpPostProcessFlags()
	{
		return BuildAssimpPostProcessFlags(MeshImportOptions{});
	}
}