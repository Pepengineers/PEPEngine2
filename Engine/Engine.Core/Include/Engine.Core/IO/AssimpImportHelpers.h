// AssimpImportHelpers.h

#pragma once

#include <Engine.Core/IO/ImportPolicies.h>

#include <assimp/postprocess.h>

namespace Engine::Core
{
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