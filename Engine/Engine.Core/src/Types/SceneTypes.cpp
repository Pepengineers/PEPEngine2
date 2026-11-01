// SceneTypes.cpp

#include <Engine.Core/Types/SceneTypes.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Engine::Core
{
#pragma region SceneNode
	bool SceneNode::HasParent() const
	{
		return ParentIndex >= 0;
	}

	bool SceneNode::HasChildren() const
	{
		return !ChildrenIndices.empty();
	}

	bool SceneNode::HasMeshes() const
	{
		return !Meshes.empty();
	}
#pragma endregion SceneNode

#pragma region SceneAsset
	SceneAsset::SceneAsset(std::vector<SceneNode> nodes, std::vector<std::uint32_t> rootNodeIndices,
		std::vector<AnimationClip> clips) : _nodes(std::move(nodes)),
		_rootNodeIndices(std::move(rootNodeIndices)), _clips(std::move(clips)) {}

	bool SceneAsset::IsEmpty() const
	{
		return _nodes.empty();
	}

	size_t SceneAsset::GetNodeCount() const
	{
		return _nodes.size();
	}

	size_t SceneAsset::GetRootNodeCount() const
	{
		return _rootNodeIndices.size();
	}

	const std::vector<SceneNode>& SceneAsset::GetNodes() const
	{
		return _nodes;
	}

	const std::vector<std::uint32_t>& SceneAsset::GetRootNodeIndices() const
	{
		return _rootNodeIndices;
	}

	const SceneNode& SceneAsset::GetNode(const size_t nodeIndex) const
	{
		assert(nodeIndex < _nodes.size());
		return _nodes.at(nodeIndex);
	}
#pragma endregion SceneAsset
}
namespace
{
	template<typename T, typename Interpolate>
	T SampleKeys(const std::vector<Engine::Core::AnimationKey<T>>& keys, double time,
		const T& bind, Interpolate interpolate)
	{
		if (keys.empty()) return bind;
		if (time <= keys.front().TimeSeconds) return keys.front().Value;
		if (time >= keys.back().TimeSeconds) return keys.back().Value;
		const auto right = std::upper_bound(keys.begin(), keys.end(), time,
			[](double t, const auto& key) { return t < key.TimeSeconds; });
		const auto& left = *(right - 1);
		const float alpha = static_cast<float>((time - left.TimeSeconds) /
			(right->TimeSeconds - left.TimeSeconds));
		return interpolate(left.Value, right->Value, alpha);
	}
}

namespace Engine::Core
{
	std::vector<DirectX::SimpleMath::Matrix> SceneAsset::EvaluatePose(
		int clipIndex, double timeSeconds, bool loop,
		const std::vector<DirectX::SimpleMath::Matrix>& localPose) const
	{
		using namespace DirectX::SimpleMath;
		if (!std::isfinite(timeSeconds) || clipIndex < -1 ||
			(clipIndex >= 0 && static_cast<size_t>(clipIndex) >= _clips.size()))
			throw std::invalid_argument("Invalid animation clip/time");
		if (!localPose.empty() && localPose.size() != _nodes.size())
			throw std::invalid_argument("Local pose must contain every hierarchy node");
		std::vector<Matrix> pose;
		pose.reserve(_nodes.size());
		for (const auto& node : _nodes) pose.push_back(node.LocalBindMatrix);
		if (clipIndex >= 0 && localPose.empty())
		{
			const auto& clip = _clips[clipIndex];
			double time = (std::max)(0.0, (std::min)(timeSeconds, clip.DurationSeconds));
			if (loop && clip.DurationSeconds > 0)
			{
				time = std::fmod(timeSeconds, clip.DurationSeconds);
				if (time < 0) time += clip.DurationSeconds;
			}
			for (const auto& track : clip.Tracks)
			{
				const auto& node = _nodes.at(track.NodeIndex);
				if (track.Translation.empty() && track.Rotation.empty() && track.Scale.empty()) continue;
				const auto lerp = [](const Vector3& a, const Vector3& b, float t) { return Vector3::Lerp(a, b, t); };
				const auto slerp = [](const Quaternion& a, const Quaternion& b, float t) { return Quaternion::Slerp(a, b, t); };
				const auto translation = SampleKeys(track.Translation, time, node.LocalTranslation, lerp);
				const auto rotation = SampleKeys(track.Rotation, time, node.LocalRotation, slerp);
				const auto scale = SampleKeys(track.Scale, time, node.LocalScale, lerp);
				pose[track.NodeIndex] = Matrix::CreateScale(scale) *
					Matrix::CreateFromQuaternion(rotation) * Matrix::CreateTranslation(translation);
			}
		}
		if (!localPose.empty()) pose = localPose;
		for (size_t i = 0; i < pose.size(); ++i)
		{
			const auto* values = reinterpret_cast<const float*>(&pose[i]);
			for (size_t j = 0; j < 16; ++j)
				if (!std::isfinite(values[j])) throw std::invalid_argument("Non-finite pose matrix");
			const auto parent = _nodes[i].ParentIndex;
			if (parent >= static_cast<std::int32_t>(i) || parent < -1)
				throw std::invalid_argument("Hierarchy must be in parent-first order");
			if (parent >= 0) pose[i] *= pose[parent];
			for (size_t j = 0; j < 16; ++j)
				if (!std::isfinite(values[j])) throw std::overflow_error("Global pose matrix overflow");
			if (pose[i]._14 != 0 || pose[i]._24 != 0 || pose[i]._34 != 0 || pose[i]._44 != 1)
				throw std::invalid_argument("Skeletal transforms must be affine");
		}
		return pose;
	}
}
