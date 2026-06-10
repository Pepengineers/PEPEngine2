// MeshImporter.cpp

#include <Engine.Core/IO/MeshImporter.h>
#include <Engine.Core/IO/AssimpImportHelpers.h>

#include <assimp/Importer.hpp>
#include <assimp/GltfMaterial.h>
#include <assimp/ObjMaterial.h>
#include <assimp/material.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/matrix3x3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace SimpleMath = DirectX::SimpleMath;

namespace 
{
	struct ObjMtlTransparencyInfo
	{
		bool HasOpacity = false;
		float Opacity = 1.0f;
		bool HasTransparentIlluminationModel = false;
		std::filesystem::path OpacityTexturePath;
	};

	/// Constructs a BoundingBox from explicit min/max corner points.
	/// Center and Extents are computed as the midpoint and half-size of the AABB.
	DirectX::BoundingBox CreateBoundsFromMinMax(const SimpleMath::Vector3& minPoint, const SimpleMath::Vector3& maxPoint)
	{
		DirectX::BoundingBox bounds = {};
		bounds.Center =
		{
			(minPoint.x + maxPoint.x) * 0.5f,
			(minPoint.y + maxPoint.y) * 0.5f,
			(minPoint.z + maxPoint.z) * 0.5f
		};
		bounds.Extents =
		{
			(maxPoint.x - minPoint.x) * 0.5f,
			(maxPoint.y - minPoint.y) * 0.5f,
			(maxPoint.z - minPoint.z) * 0.5f
		};

		return bounds;
	}

	/// Computes a tight axis-aligned bounding box enclosing all given vertices.
	/// Returns a default-constructed BoundingBox if the vertex list is empty.
	DirectX::BoundingBox CalculateVertexBounds(const std::vector<Engine::Core::Vertex>& vertices)
	{
		if (vertices.empty())
		{
			return {};
		}

		SimpleMath::Vector3 minPoint = vertices.front().Position;
		SimpleMath::Vector3 maxPoint = vertices.front().Position;

		for (const Engine::Core::Vertex& vertex : vertices)
		{
			minPoint.x = (std::min)(minPoint.x, vertex.Position.x);
			minPoint.y = (std::min)(minPoint.y, vertex.Position.y);
			minPoint.z = (std::min)(minPoint.z, vertex.Position.z);

			maxPoint.x = (std::max)(maxPoint.x, vertex.Position.x);
			maxPoint.y = (std::max)(maxPoint.y, vertex.Position.y);
			maxPoint.z = (std::max)(maxPoint.z, vertex.Position.z);
		}

		return CreateBoundsFromMinMax(minPoint, maxPoint);
	}

	/// Computes a tight axis-aligned bounding box enclosing all given submeshes.
	/// Each submesh's Bounds is unpacked into min/max corners and merged into a single AABB.
	/// Returns a default-constructed BoundingBox if the submesh list is empty.
	DirectX::BoundingBox CalculateMeshBounds(const std::vector<Engine::Core::SubMesh>& subMeshes)
	{
		if (subMeshes.empty())
		{
			return {};
		}

		const DirectX::BoundingBox& firstBounds = subMeshes.front().Bounds;

		SimpleMath::Vector3 minPoint =
		{
			firstBounds.Center.x - firstBounds.Extents.x,
			firstBounds.Center.y - firstBounds.Extents.y,
			firstBounds.Center.z - firstBounds.Extents.z
		};

		SimpleMath::Vector3 maxPoint =
		{
			firstBounds.Center.x + firstBounds.Extents.x,
			firstBounds.Center.y + firstBounds.Extents.y,
			firstBounds.Center.z + firstBounds.Extents.z
		};

		for (const Engine::Core::SubMesh& subMesh : subMeshes)
		{
			const DirectX::BoundingBox& subMeshBounds = subMesh.Bounds;

			const SimpleMath::Vector3 subMeshMinPoint =
			{
				subMeshBounds.Center.x - subMeshBounds.Extents.x,
				subMeshBounds.Center.y - subMeshBounds.Extents.y,
				subMeshBounds.Center.z - subMeshBounds.Extents.z
			};

			const SimpleMath::Vector3 subMeshMaxPoint =
			{
				subMeshBounds.Center.x + subMeshBounds.Extents.x,
				subMeshBounds.Center.y + subMeshBounds.Extents.y,
				subMeshBounds.Center.z + subMeshBounds.Extents.z
			};

			minPoint.x = (std::min)(minPoint.x, subMeshMinPoint.x);
			minPoint.y = (std::min)(minPoint.y, subMeshMinPoint.y);
			minPoint.z = (std::min)(minPoint.z, subMeshMinPoint.z);

			maxPoint.x = (std::max)(maxPoint.x, subMeshMaxPoint.x);
			maxPoint.y = (std::max)(maxPoint.y, subMeshMaxPoint.y);
			maxPoint.z = (std::max)(maxPoint.z, subMeshMaxPoint.z);
		}

		return CreateBoundsFromMinMax(minPoint, maxPoint);
	}

	std::filesystem::path ResolveAssimpTexturePath(const std::filesystem::path& sourcePath, const aiString& assimpTexturePath)
	{
		const std::string texturePathText = assimpTexturePath.C_Str();
		if (texturePathText.empty() || texturePathText.front() == '*' /* '*' means embedded texture */)
		{
			return {};
		}

		const std::filesystem::path texturePath = std::filesystem::path(texturePathText);
		if (texturePath.is_absolute())
		{
			return texturePath.lexically_normal();
		}

		return (sourcePath.parent_path() / texturePath).lexically_normal();
	}

	std::string BuildTexturePathKey(const std::filesystem::path& texturePath)
	{
		std::string key = texturePath.lexically_normal().generic_string();
		std::transform(key.begin(), key.end(), key.begin(), [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
		return key;
	}

	bool AreSameTexturePath(const std::filesystem::path& left, const std::filesystem::path& right)
	{
		if (left.empty() || right.empty())
		{
			return false;
		}

		return BuildTexturePathKey(left) == BuildTexturePathKey(right);
	}

	float ConvertShininessToRoughness(const float shininess)
	{
		if (shininess <= 0.0f)
		{
			return 1.0f;
		}

		const float roughness = std::sqrt(2.0f / (shininess + 2.0f));
		return (std::max)(0.04f, (std::min)(roughness, 1.0f));
	}

	float Clamp01(const float value)
	{
		return (std::max)(0.0f, (std::min)(value, 1.0f));
	}

	bool IsTransparentObjIlluminationModel(const int illuminationModel)
	{
		return illuminationModel == 4 || illuminationModel == 6 || illuminationModel == 7 || illuminationModel == 9;
	}

	/// Returns true for OBJ illumination models that describe glass/refraction transparency.
	bool HasTransparentObjIlluminationModel(const aiMaterial& assimpMaterial)
	{
		int illuminationModel = 0;
		if (assimpMaterial.Get(AI_MATKEY_OBJ_ILLUM, illuminationModel) != AI_SUCCESS)
		{
			return false;
		}

		return IsTransparentObjIlluminationModel(illuminationModel);
	}

	/// Reads OBJ/Assimp transparency factor and converts it to opacity.
	/// OBJ Tr is transparency, while the renderer expects opacity.
	bool TryReadTransparencyFactorOpacity(const aiMaterial& assimpMaterial, float& outOpacity)
	{
		float transparencyFactor = 0.0f;
		if (assimpMaterial.Get(AI_MATKEY_TRANSPARENCYFACTOR, transparencyFactor) != AI_SUCCESS)
		{
			return false;
		}

		outOpacity = Clamp01(1.0f - transparencyFactor);
		return true;
	}

	/// Converts an ASCII string to lowercase.
	std::string ToLowerAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
		return value;
	}

	std::string TrimWhitespace(const std::string& value)
	{
		const std::size_t first = value.find_first_not_of(" \t\r\n");
		if (first == std::string::npos)
		{
			return {};
		}

		const std::size_t last = value.find_last_not_of(" \t\r\n");
		return value.substr(first, last - first + 1);
	}

	/// Skips common OBJ texture map options so the remaining tokens can be read as the texture path.
	void SkipObjTextureOptionArguments(std::istringstream& stream, const std::string& option)
	{
		int argumentCount = 1;
		if (option == "-mm")
		{
			argumentCount = 2;
		}
		else if (option == "-o" || option == "-s" || option == "-t")
		{
			argumentCount = 3;
		}

		std::string ignoredToken;
		for (int argumentIndex = 0; argumentIndex < argumentCount && stream >> ignoredToken; ++argumentIndex)
		{
		}
	}

	/// Extracts a texture path from an OBJ MTL map_* statement.
	std::string ReadObjTexturePathFromMapStatement(const std::string& line, const std::string& keyword)
	{
		std::istringstream stream(line.substr(keyword.size()));
		std::string texturePathText;
		std::string token;
		while (stream >> token)
		{
			if (!token.empty() && token.front() == '-')
			{
				SkipObjTextureOptionArguments(stream, ToLowerAscii(token));
				continue;
			}

			if (!texturePathText.empty())
			{
				texturePathText += ' ';
			}
			texturePathText += token;
		}

		return TrimWhitespace(texturePathText);
	}

	/// Resolves an OBJ MTL texture reference relative to the .mtl file that declared it.
	std::filesystem::path ResolveObjMtlTexturePath(const std::filesystem::path& mtlPath, const std::string& texturePathText)
	{
		if (texturePathText.empty() || texturePathText.front() == '*')
		{
			return {};
		}

		const std::filesystem::path texturePath(texturePathText);
		if (texturePath.is_absolute())
		{
			return texturePath.lexically_normal();
		}

		return (mtlPath.parent_path() / texturePath).lexically_normal();
	}

	/// Reads transparency data that Assimp does not always expose consistently for OBJ materials.
	void ParseObjMtlTransparencyFile(
		const std::filesystem::path& mtlPath,
		std::unordered_map<std::string, ObjMtlTransparencyInfo>& outTransparencyInfos)
	{
		std::ifstream file(mtlPath);
		if (!file.is_open())
		{
			return;
		}

		std::string currentMaterialName;
		std::string line;
		while (std::getline(file, line))
		{
			const std::size_t commentPosition = line.find('#');
			if (commentPosition != std::string::npos)
			{
				line.erase(commentPosition);
			}

			line = TrimWhitespace(line);
			if (line.empty())
			{
				continue;
			}

			std::istringstream stream(line);
			std::string keyword;
			stream >> keyword;
			keyword = ToLowerAscii(keyword);

			if (keyword == "newmtl")
			{
				currentMaterialName = TrimWhitespace(line.substr(std::string("newmtl").size()));
				if (!currentMaterialName.empty())
				{
					outTransparencyInfos.try_emplace(ToLowerAscii(currentMaterialName));
				}
				continue;
			}

			if (currentMaterialName.empty())
			{
				continue;
			}

			ObjMtlTransparencyInfo& info = outTransparencyInfos[ToLowerAscii(currentMaterialName)];
			if (keyword == "d")
			{
				float opacity = 1.0f;
				if (stream >> opacity)
				{
					info.Opacity = Clamp01(opacity);
					info.HasOpacity = true;
				}
			}
			else if (keyword == "tr")
			{
				float transparency = 0.0f;
				if (stream >> transparency)
				{
					info.Opacity = Clamp01(1.0f - transparency);
					info.HasOpacity = true;
				}
			}
			else if (keyword == "illum")
			{
				int illuminationModel = 0;
				if (stream >> illuminationModel)
				{
					info.HasTransparentIlluminationModel = IsTransparentObjIlluminationModel(illuminationModel);
				}
			}
			else if (keyword == "map_d")
			{
				info.OpacityTexturePath = ResolveObjMtlTexturePath(mtlPath, ReadObjTexturePathFromMapStatement(line, keyword));
			}
		}
	}

	/// Loads transparency metadata from every .mtl file referenced by the given OBJ file.
	std::unordered_map<std::string, ObjMtlTransparencyInfo> LoadObjMtlTransparencyInfos(const std::filesystem::path& sourcePath)
	{
		std::unordered_map<std::string, ObjMtlTransparencyInfo> transparencyInfos;
		if (ToLowerAscii(sourcePath.extension().string()) != ".obj")
		{
			return transparencyInfos;
		}

		std::ifstream file(sourcePath);
		if (!file.is_open())
		{
			return transparencyInfos;
		}

		std::string line;
		while (std::getline(file, line))
		{
			const std::size_t commentPosition = line.find('#');
			if (commentPosition != std::string::npos)
			{
				line.erase(commentPosition);
			}

			line = TrimWhitespace(line);
			if (line.empty())
			{
				continue;
			}

			std::istringstream stream(line);
			std::string keyword;
			stream >> keyword;
			if (ToLowerAscii(keyword) != "mtllib")
			{
				continue;
			}

			const std::string mtlName = TrimWhitespace(line.substr(std::string("mtllib").size()));
			if (mtlName.empty())
			{
				continue;
			}

			ParseObjMtlTransparencyFile((sourcePath.parent_path() / std::filesystem::path(mtlName)).lexically_normal(), transparencyInfos);
		}

		return transparencyInfos;
	}

	/// Returns true if the material declares a glTF alpha mode that requires non-opaque rendering.
	bool HasTransparentAlphaMode(const aiMaterial& assimpMaterial)
	{
		aiString alphaMode;
		if (assimpMaterial.Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) != AI_SUCCESS)
		{
			return false;
		}

		const std::string alphaModeText = ToLowerAscii(alphaMode.C_Str());
		return alphaModeText == "blend" || alphaModeText == "mask";
	}

	/// Determines the renderer-facing material type from imported transparency metadata.
	Engine::Core::EMaterialType DetermineMaterialType(
		const aiMaterial& assimpMaterial,
		const float opacity,
		const std::filesystem::path& opacityTexturePath)
	{
		if (opacity < 0.999f || !opacityTexturePath.empty())
		{
			return Engine::Core::EMaterialType::Transparent;
		}

		int blendMode = aiBlendMode_Default;
		if (assimpMaterial.Get(AI_MATKEY_BLEND_FUNC, blendMode) == AI_SUCCESS && blendMode == aiBlendMode_Additive)
		{
			return Engine::Core::EMaterialType::Transparent;
		}

		if (HasTransparentAlphaMode(assimpMaterial))
		{
			return Engine::Core::EMaterialType::Transparent;
		}

		if (HasTransparentObjIlluminationModel(assimpMaterial))
		{
			return Engine::Core::EMaterialType::Transparent;
		}

		return Engine::Core::EMaterialType::Opaque;
	}

	void TryImportTexturePath(
		const aiMaterial& assimpMaterial,
		const aiTextureType textureType,
		const std::filesystem::path& sourcePath,
		std::filesystem::path& outTexturePath)
	{
		aiString texturePath;
		if (assimpMaterial.GetTexture(textureType, 0, &texturePath) == AI_SUCCESS)
		{
			outTexturePath = ResolveAssimpTexturePath(sourcePath, texturePath);
		}
	}

	std::vector<Engine::Core::MeshMaterial> ImportMaterials(const aiScene& assimpScene, const std::filesystem::path& sourcePath)
	{
		const std::unordered_map<std::string, ObjMtlTransparencyInfo> objMtlTransparencyInfos = LoadObjMtlTransparencyInfos(sourcePath);
		std::vector<Engine::Core::MeshMaterial> importedMaterials;
		importedMaterials.reserve(assimpScene.mNumMaterials);
		
		for (unsigned int materialIndex = 0; materialIndex < assimpScene.mNumMaterials; ++materialIndex)
		{
			const aiMaterial* assimpMaterial = assimpScene.mMaterials[materialIndex];
			if (assimpMaterial == nullptr)
			{
				importedMaterials.emplace_back();
				continue;
			}
			
			Engine::Core::MeshMaterial importedMaterial = {};
			
			aiString materialName;
			if (assimpMaterial->Get(AI_MATKEY_NAME, materialName) == AI_SUCCESS)
			{
				importedMaterial.Name = materialName.C_Str();
			}

			const ObjMtlTransparencyInfo* objMtlTransparencyInfo = nullptr;
			if (!importedMaterial.Name.empty())
			{
				const auto transparencyInfoIt = objMtlTransparencyInfos.find(ToLowerAscii(importedMaterial.Name));
				if (transparencyInfoIt != objMtlTransparencyInfos.end())
				{
					objMtlTransparencyInfo = &transparencyInfoIt->second;
				}
			}
			
			aiColor3D diffuseColor(1.0f, 1.0f, 1.0f);
			if (assimpMaterial->Get(AI_MATKEY_COLOR_DIFFUSE, diffuseColor) == AI_SUCCESS)
			{
				importedMaterial.DiffuseColor = SimpleMath::Vector3(diffuseColor.r, diffuseColor.g, diffuseColor.b);
			}
			
			aiColor3D specularColor(0.0f, 0.0f, 0.0f);
			if (assimpMaterial->Get(AI_MATKEY_COLOR_SPECULAR, specularColor) == AI_SUCCESS)
			{
				importedMaterial.SpecularColor = SimpleMath::Vector3(specularColor.r, specularColor.g, specularColor.b);
			}
			
			aiColor3D emissiveColor(0.0f, 0.0f, 0.0f);
			if (assimpMaterial->Get(AI_MATKEY_COLOR_EMISSIVE, emissiveColor) == AI_SUCCESS)
			{
				importedMaterial.EmissiveColor = SimpleMath::Vector3(emissiveColor.r, emissiveColor.g, emissiveColor.b);
			}
			
			float shininess = 0.0f;
			if (assimpMaterial->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS)
			{
				importedMaterial.Roughness = ConvertShininessToRoughness(shininess);
			}
			
			float roughness = importedMaterial.Roughness;
			if (assimpMaterial->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS)
			{
				importedMaterial.Roughness = (std::max)(0.04f, (std::min)(roughness, 1.0f));
			}

			float opacity = importedMaterial.Opacity;
			if (assimpMaterial->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS)
			{
				importedMaterial.Opacity = Clamp01(opacity);
			}

			if (TryReadTransparencyFactorOpacity(*assimpMaterial, opacity))
			{
				importedMaterial.Opacity = opacity;
			}

			if (objMtlTransparencyInfo != nullptr && objMtlTransparencyInfo->HasOpacity)
			{
				importedMaterial.Opacity = objMtlTransparencyInfo->Opacity;
			}
			
			TryImportTexturePath(*assimpMaterial, aiTextureType_DIFFUSE, sourcePath, importedMaterial.DiffuseTexturePath);
			TryImportTexturePath(*assimpMaterial, aiTextureType_SPECULAR, sourcePath, importedMaterial.SpecularTexturePath);
			TryImportTexturePath(*assimpMaterial, aiTextureType_NORMALS, sourcePath, importedMaterial.NormalTexturePath);
			TryImportTexturePath(*assimpMaterial, aiTextureType_DIFFUSE_ROUGHNESS, sourcePath, importedMaterial.RoughnessTexturePath);
			TryImportTexturePath(*assimpMaterial, aiTextureType_EMISSIVE, sourcePath, importedMaterial.EmissiveTexturePath);
			TryImportTexturePath(*assimpMaterial, aiTextureType_OPACITY, sourcePath, importedMaterial.OpacityTexturePath);
			if (importedMaterial.OpacityTexturePath.empty() && objMtlTransparencyInfo != nullptr)
			{
				importedMaterial.OpacityTexturePath = objMtlTransparencyInfo->OpacityTexturePath;
			}
			importedMaterial.Type = DetermineMaterialType(*assimpMaterial, importedMaterial.Opacity, importedMaterial.OpacityTexturePath);
			if (objMtlTransparencyInfo != nullptr && objMtlTransparencyInfo->HasTransparentIlluminationModel)
			{
				importedMaterial.Type = Engine::Core::EMaterialType::Transparent;
			}
			
			importedMaterial.UseBakedLighting = AreSameTexturePath(importedMaterial.DiffuseTexturePath, importedMaterial.EmissiveTexturePath);
				
			importedMaterials.push_back(std::move(importedMaterial));
		}
		
		return importedMaterials;
	}

	/// Transforms a position vector by a 4x4 matrix, including the translation component (w=1).
	aiVector3D TransformPosition(const aiMatrix4x4& transform, const aiVector3D& position)
	{
		return transform * position;
	}

	/// Builds the inverse-transpose 3x3 matrix used to transform direction vectors
	/// (normals, tangents) correctly under non-uniform scaling.
	aiMatrix3x3 BuildNormalTransform(const aiMatrix4x4& transform)
	{
		aiMatrix3x3 normalTransform(transform);
		normalTransform.Inverse().Transpose();
		return normalTransform;
	}

	/// Transforms a direction vector (normal, tangent) by a precomputed inverse-transpose matrix.
	/// The result is re-normalized to unit length.
	aiVector3D TransformDirection(const aiMatrix3x3& normalTransform, const aiVector3D& direction)
	{
		aiVector3D transformedDirection = normalTransform * direction;

		if (transformedDirection.SquareLength() > 0.0f)
		{
			transformedDirection.Normalize();
		}

		return transformedDirection;
	}
	
	/// Converts a single Assimp mesh into an engine SubMesh.
	/// Vertex positions, normals, UVs and tangents are read from the Assimp mesh and
	/// baked into world space using nodeTransform.
	/// startVertexLocation and startIndexLocation are recorded for use in merged GPU buffers.
	Engine::Core::SubMesh ImportSubMesh(
		const aiMesh& assimpMesh,
		const aiMatrix4x4& nodeTransform,
		const std::uint32_t startVertexLocation,
		const std::uint32_t startIndexLocation)
	{
		Engine::Core::SubMesh importedSubMesh = {};
		importedSubMesh.Vertices.reserve(assimpMesh.mNumVertices);
		importedSubMesh.Indices.reserve(assimpMesh.mNumFaces * 3u);
		importedSubMesh.MaterialIndex = assimpMesh.mMaterialIndex;

		const aiMatrix3x3 normalTransform = BuildNormalTransform(nodeTransform);

		for (unsigned int vertexIndex = 0; vertexIndex < assimpMesh.mNumVertices; ++vertexIndex)
		{
			const aiVector3D transformedPosition = TransformPosition(nodeTransform, assimpMesh.mVertices[vertexIndex]);

			Engine::Core::Vertex importedVertex = {};
			importedVertex.Position = SimpleMath::Vector3(transformedPosition.x, transformedPosition.y, transformedPosition.z);

			if (assimpMesh.HasNormals())
			{
				const aiVector3D transformedNormal = TransformDirection(normalTransform, assimpMesh.mNormals[vertexIndex]);
				importedVertex.Normal = SimpleMath::Vector3(transformedNormal.x, transformedNormal.y, transformedNormal.z);
			}

			if (assimpMesh.HasTextureCoords(0))
			{
				const aiVector3D& assimpTexCoord = assimpMesh.mTextureCoords[0][vertexIndex];
				importedVertex.TexCoord = SimpleMath::Vector2(assimpTexCoord.x, assimpTexCoord.y);
			}

			if (assimpMesh.HasTangentsAndBitangents())
			{
				const aiVector3D transformedTangent = TransformDirection(normalTransform, assimpMesh.mTangents[vertexIndex]);
				importedVertex.Tangent = SimpleMath::Vector3(transformedTangent.x, transformedTangent.y, transformedTangent.z);
			}

			importedSubMesh.Vertices.push_back(importedVertex);
		}

		for (unsigned int faceIndex = 0; faceIndex < assimpMesh.mNumFaces; ++faceIndex)
		{
			const aiFace& assimpFace = assimpMesh.mFaces[faceIndex];

			assert(assimpFace.mNumIndices == 3);
			if (assimpFace.mNumIndices != 3)
			{
				continue;
			}

			for (unsigned int indexIndex = 0; indexIndex < assimpFace.mNumIndices; ++indexIndex)
			{
				importedSubMesh.Indices.push_back(assimpFace.mIndices[indexIndex]);
			}
		}

		importedSubMesh.Bounds = CalculateVertexBounds(importedSubMesh.Vertices);
		return importedSubMesh;
	}

	/// Recursively walks the Assimp node tree and imports all meshes into importedSubMeshes.
	/// nodeTransform accumulates the full chain of transforms from the root to the current node,
	/// so that each mesh is baked into a common world space.
	/// startVertexLocation and startIndexLocation are updated after each imported submesh
	/// to maintain correct offsets for a merged GPU vertex/index buffer.
	void ImportNodeMeshes(
		const aiScene& assimpScene,
		const aiNode& assimpNode,
		const aiMatrix4x4& parentTransform,
		std::vector<Engine::Core::SubMesh>& importedSubMeshes,
		std::uint32_t& startVertexLocation,
		std::uint32_t& startIndexLocation)
	{
		const aiMatrix4x4 nodeTransform = parentTransform * assimpNode.mTransformation;

		for (unsigned int nodeMeshIndex = 0; nodeMeshIndex < assimpNode.mNumMeshes; ++nodeMeshIndex)
		{
			const unsigned int sceneMeshIndex = assimpNode.mMeshes[nodeMeshIndex];
			if (sceneMeshIndex >= assimpScene.mNumMeshes)
			{
				continue;
			}

			const aiMesh* assimpMesh = assimpScene.mMeshes[sceneMeshIndex];
			if (assimpMesh == nullptr)
			{
				continue;
			}

			Engine::Core::SubMesh importedSubMesh = ImportSubMesh(*assimpMesh, nodeTransform, startVertexLocation, startIndexLocation);
			if (importedSubMesh.GetVertexCount() == 0 || importedSubMesh.GetIndexCount() == 0)
			{
				continue;
			}

			startVertexLocation += static_cast<std::uint32_t>(importedSubMesh.GetVertexCount());
			startIndexLocation += static_cast<std::uint32_t>(importedSubMesh.GetIndexCount());

			importedSubMeshes.push_back(std::move(importedSubMesh));
		}

		for (unsigned int childIndex = 0; childIndex < assimpNode.mNumChildren; ++childIndex)
		{
			const aiNode* childNode = assimpNode.mChildren[childIndex];
			if (childNode == nullptr)
			{
				continue;
			}

			ImportNodeMeshes(assimpScene, *childNode, nodeTransform, importedSubMeshes, startVertexLocation, startIndexLocation);
		}
	}
}

namespace Engine::Core
{
	std::unique_ptr<Mesh> MeshImporter::ImportSingleMeshAsset(const std::filesystem::path& sourcePath, const MeshImportOptions& options)
	{
		if (sourcePath.empty())
		{
			return nullptr;
		}

		Assimp::Importer importer;
		const std::string sourcePathUtf8 = sourcePath.u8string();

		const std::uint32_t assimpFlags = BuildAssimpPostProcessFlags(options);
		const aiScene* assimpScene = importer.ReadFile(sourcePathUtf8.c_str(), assimpFlags);
		
		if (assimpScene == nullptr || assimpScene->mRootNode == nullptr || (assimpScene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0)
		{
			return nullptr;
		}

		std::vector<SubMesh> importedSubMeshes;
		std::uint32_t startVertexLocation = 0;
		std::uint32_t startIndexLocation = 0;

		const aiMatrix4x4 identityTransform;
		ImportNodeMeshes(*assimpScene, *assimpScene->mRootNode, identityTransform, importedSubMeshes, startVertexLocation, startIndexLocation);

		if (importedSubMeshes.empty())
		{
			return nullptr;
		}

		const DirectX::BoundingBox meshBounds = CalculateMeshBounds(importedSubMeshes);
		std::vector<MeshMaterial> importedMaterials = ImportMaterials(*assimpScene, sourcePath);
		std::unique_ptr<Mesh> mesh = std::make_unique<Mesh>(std::move(importedSubMeshes), meshBounds, std::move(importedMaterials));
		return mesh;
	}

	std::unique_ptr<Mesh> MeshImporter::ImportMeshSubAsset(const std::filesystem::path& sourcePath, const std::uint32_t subAssetIndex, const MeshImportOptions& options)
	{
		if (sourcePath.empty())
		{
			return nullptr;
		}

		Assimp::Importer importer;
		const std::string sourcePathUtf8 = sourcePath.u8string();

		const std::uint32_t assimpFlags = BuildAssimpPostProcessFlags(options);
		const aiScene* assimpScene = importer.ReadFile(sourcePathUtf8.c_str(), assimpFlags);

		if (assimpScene == nullptr || assimpScene->mRootNode == nullptr || (assimpScene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0)
		{
			return nullptr;
		}

		if (subAssetIndex >= assimpScene->mNumMeshes)
		{
			return nullptr;
		}

		const aiMesh* assimpMesh = assimpScene->mMeshes[subAssetIndex];
		if (assimpMesh == nullptr)
		{
			return nullptr;
		}

		const aiMatrix4x4 identityTransform;
		SubMesh importedSubMesh = ImportSubMesh(*assimpMesh, identityTransform, 0u, 0u);
		if (importedSubMesh.GetVertexCount() == 0 || importedSubMesh.GetIndexCount() == 0)
		{
			return nullptr;
		}

		std::vector<SubMesh> importedSubMeshes;
		importedSubMeshes.push_back(std::move(importedSubMesh));

		const DirectX::BoundingBox meshBounds = CalculateMeshBounds(importedSubMeshes);
		return std::make_unique<Mesh>(std::move(importedSubMeshes), meshBounds, ImportMaterials(*assimpScene, sourcePath));
	}
}