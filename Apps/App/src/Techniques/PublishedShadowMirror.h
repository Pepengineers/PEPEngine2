#pragma once
#include "SceneLocalShadows.h"
#include "ShadowDepthTransfer.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"

namespace BistroRender
{
struct PublishedShadows
{
	PublishedShadows(SceneLocalShadows& pass, UINT slot, UINT lightCount)
	    : Fallback(pass.Output(slot)), Atlases(pass.Outputs(slot)), Lights(pass.LightSource(slot)),
	      Source(pass), Slot(slot), LightCount(lightCount) {}
	GDX12Texture* Fallback;
	std::vector<GDX12Texture*> Atlases;
	GDX12UploadBuffer<GDX12LocalLightConstants>& Lights;
	const SceneLocalShadows& Source;
	UINT Slot, LightCount;
};

// The caller retires the consumer's previous submission before preparing another capture or frame.
class PublishedShadowMirror
{
	struct Atlas
	{
		ShadowMemory::Charge Memory;
		std::unique_ptr<GDX12Texture> Depth;
		std::unique_ptr<ShadowDepthTransfer> Transfer;
		std::vector<uint64_t> Versions;
		std::vector<UINT> Slices;
	};
	std::map<UINT, Atlas> _atlases;
	std::map<UINT, UINT> _descriptorSizes;
	bool _indexed = false;

	static UINT Descriptor(const GDX12LocalLightConstants& light, const PublishedShadows& source)
	{
		return light.ShadowFlags & LocalShadowAtlasIndex ? light.ShadowFlags >> 4 : source.Fallback->GetSRV()->HeapIndex;
	}

	template<class Visitor>
	static void VisitReadySlices(const PublishedShadows& source, Visitor visit)
	{
		for (UINT i = 0; i < source.LightCount; ++i)
		{
			const auto& light = source.Lights.GetCPUData()[i];
			if (light.ShadowFirstSlice == UINT_MAX) continue;
			const UINT descriptor = Descriptor(light, source);
			for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
				if (light._padding[2] & (1u << face)) visit(descriptor, light.ShadowFirstSlice + face);
		}
	}

public:
	static bool CanRecreate(const QueueSubmission& submit, Id owner,
	                        std::initializer_list<const PublishedShadows*> sources)
	{
		auto& primary = submit.Device(0);
		auto& device = submit.Device(owner);
		const bool indexed = device.GetDeviceFeatures().Features.HighestShaderModel() >= D3D_SHADER_MODEL_6_6;
		UINT64 primaryBytes = 0, ownerBytes = 0;
		for (const auto* source : sources)
		{
			if (!source) continue;
			std::map<UINT, UINT> ready;
			VisitReadySlices(*source, [&](UINT descriptor, UINT) { ++ready[descriptor]; });
			if (!indexed && ready.size() > 1) return false;
			for (auto* atlas : source->Atlases)
			{
				const auto found = ready.find(atlas->GetSRV()->HeapIndex);
				if (found == ready.end()) continue;
				const UINT side = atlas->GetWidth();
				const UINT layers = atlas->GetResource()->D3DResource->GetDesc().DepthOrArraySize;
				const auto transfer = ShadowMemory::TransferBytes(primary, device, side, found->second);
				primaryBytes += transfer;
				ownerBytes += transfer + ShadowMemory::Size(device, ShadowMemory::Depth(side, layers, D3D12_RESOURCE_FLAG_NONE));
			}
		}
		return !ownerBytes || (ShadowMemory::Fits(primary, primaryBytes) && ShadowMemory::Fits(device, ownerBytes));
	}

	void Clear()
	{
		_atlases.clear();
		_descriptorSizes.clear();
	}

	bool Prepare(const QueueSubmission& submit, Id owner, GDX12DescriptorHeap& heap, const PublishedShadows& source)
	{
		auto& primary = submit.Device(0);
		auto& device = submit.Device(owner);
		_indexed = device.GetDeviceFeatures().Features.HighestShaderModel() >= D3D_SHADER_MODEL_6_6;
		_descriptorSizes.clear();
		for (auto* atlas : source.Atlases)
			_descriptorSizes.emplace(atlas->GetSRV()->HeapIndex, atlas->GetWidth());
		for (auto& [side, local] : _atlases) local.Slices.clear();
		VisitReadySlices(source, [&](UINT descriptor, UINT slice) {
			_atlases[_descriptorSizes.at(descriptor)].Slices.push_back(slice);
		});
		for (auto it = _atlases.begin(); it != _atlases.end();)
			if (it->second.Slices.empty()) it = _atlases.erase(it);
			else ++it;
		if (!_indexed && _atlases.size() > 1) { Clear(); return false; }

		// Old slot resources are fence-safe now. Release replaced allocations before pricing their successors.
		UINT64 primaryBytes = 0, ownerBytes = 0;
		for (auto* atlas : source.Atlases)
		{
			const UINT side = atlas->GetWidth();
			const auto found = _atlases.find(side);
			if (found == _atlases.end()) continue;
			auto& local = found->second;
			const UINT layers = atlas->GetResource()->D3DResource->GetDesc().DepthOrArraySize;
			if (local.Versions.size() != layers)
			{
				local.Depth.reset();
				local.Memory.Reset();
				local.Versions.assign(layers, 0);
			}
			if (!local.Depth) ownerBytes += ShadowMemory::Size(device, ShadowMemory::Depth(side, layers, D3D12_RESOURCE_FLAG_NONE));
			const UINT count = UINT(local.Slices.size());
			if (local.Transfer && local.Transfer->Capacity() < count) local.Transfer.reset();
			if (!local.Transfer)
			{
				const UINT64 bytes = ShadowMemory::TransferBytes(primary, device, side, count);
				primaryBytes += bytes;
				ownerBytes += bytes;
			}
		}
		if (!_atlases.empty() && (!ShadowMemory::Fits(primary, primaryBytes) || !ShadowMemory::Fits(device, ownerBytes)))
		{
			Clear();
			return false;
		}
		for (auto& [side, local] : _atlases)
		{
			const UINT layers = UINT(local.Versions.size());
			if (!local.Depth)
			{
				const auto shape = ShadowMemory::Depth(side, layers, D3D12_RESOURCE_FLAG_NONE);
				GDX12TextureDesc texture{};
				texture.Width = texture.Height = side;
				texture.Format = DXGI_FORMAT_R32_FLOAT;
				texture.SRVDesc = ShadowMemory::View(layers);
				texture.SRV_UAV_Heap = &heap;
				texture.ExternalResource = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, shape, D3D12_RESOURCE_STATE_COMMON);
				texture.SRVHeapIndex = heap.GetAvailableIndex(SceneDescriptorStart);
				local.Depth = std::make_unique<GDX12Texture>();
				local.Depth->Initialize(texture);
				local.Memory.Set(device, ShadowMemory::Size(device, shape));
			}
			if (!local.Transfer) local.Transfer = std::make_unique<ShadowDepthTransfer>(submit, owner, side, UINT(local.Slices.size()));
		}
		return true;
	}

	UINT64 Append(Graph& primary, std::vector<ID3D12Resource*>& primaryBindings, NativeWork& primaryWork,
	              Graph& remote, std::vector<ID3D12Resource*>& remoteBindings, NativeWork& remoteWork,
	              const PublishedShadows& source, std::vector<Id>& exports, std::vector<Id>& imports, std::vector<Access>& reads)
	{
		UINT64 bytes = 0;
		for (auto* atlas : source.Atlases)
		{
			const UINT side = atlas->GetWidth();
			const auto found = _atlases.find(side);
			if (found == _atlases.end()) continue;
			auto& local = found->second;
			const auto& versions = source.Source.ContentVersions(source.Slot, side);
			std::vector<UINT> dirty;
			for (UINT slice : local.Slices)
			{
				const auto version = versions.at(slice);
				if (version && version == local.Versions[slice]) continue;
				dirty.push_back(slice);
				local.Versions[slice] = version;
			}
			auto* target = local.Depth->GetResource()->D3DResource.Get();
			if (!dirty.empty())
			{
				bytes += UINT64(dirty.size()) * side * side * sizeof(float);
				exports.push_back(local.Transfer->Append(primary, primaryBindings, primaryWork,
				    atlas->GetResource()->D3DResource.Get(), dirty, true));
				imports.push_back(local.Transfer->Append(remote, remoteBindings, remoteWork, target, std::move(dirty), false));
			}
			const auto id = NativeWork::ResourceFor(remote, remoteBindings, target, "published local shadow array");
			reads.push_back({id, Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE});
		}
		return bytes;
	}

	GDX12LocalLightConstants Remap(GDX12LocalLightConstants light, const PublishedShadows& source) const
	{
		if (light.ShadowFirstSlice != UINT_MAX)
		{
			const auto& local = _atlases.at(_descriptorSizes.at(Descriptor(light, source)));
			if (_indexed) SetLocalShadowAtlas(light, local.Depth->GetSRV()->HeapIndex);
			else light.ShadowFlags &= 7u;
		}
		return light;
	}

	GDX12Texture* Fallback(GDX12Texture& empty)
	{
		return _atlases.empty() ? &empty : _atlases.begin()->second.Depth.get();
	}
	D3D12_GPU_DESCRIPTOR_HANDLE Fallback(D3D12_GPU_DESCRIPTOR_HANDLE empty)
	{
		return _atlases.empty() ? empty : _atlases.begin()->second.Depth->GetSRV()->GPUHandle;
	}
};
}
