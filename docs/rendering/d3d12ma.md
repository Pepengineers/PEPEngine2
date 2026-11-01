# D3D12MA integration

The renderer vendors AMD D3D12 Memory Allocator v3.2.1 (MIT), with one allocator per native `GDX12Device`. Source provenance and hashes are in `External/External.Libraries/D3D12MemoryAllocator/README.md`.

Engine-owned textures, pass-specific arrays/cubemaps, geometry, upload/default buffers and GPU timer readbacks use `GDX12Device::CreateResource`. Swapchain images and cross-adapter shared heaps remain native imports. Vendor SDK internal allocations are outside this allocator.

## Ownership and synchronization

`GDX12ResourceHandle` retains the allocator and allocation together. D3D12MA's allocation owns the native resource. Copies of texture descriptions, resource wrappers and texture upload batches retain this ownership; `.Get()` only borrows the native resource. Do not replace an owning handle with a `ComPtr<ID3D12Resource>`: that would allow the memory region to be recycled while the native resource still exists.

Callers must keep the handle until the last GPU fence completes. Existing upload-batch drains, geometry upload flushes, frame-slot waits and resize flushes remain responsible for synchronization. This is not a deferred GPU deletion queue. A handle may safely outlive the `GDX12Device` wrapper after GPU work has completed.

Fresh pooled allocations do not promise zero contents. Upload buffers are explicitly zeroed, including newly grown capacity. Textures must be initialized before sampling. Disabled/empty shadow maps now receive a first clear; the fullscreen display output uses discard before its full overwrite. This also meets [D3D12 placed render-target/depth initialization requirements](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createplacedresource).

Default D3D12MA allocation heuristics are preserved, including dedicated allocations for large resources and, on applicable devices, small buffers. No forced pooling, custom pools, defragmentation, residency manager or overlapping resource aliasing is enabled. Resources can reuse released heap space. The [render graph](render-graph.md) additionally reuses compatible physical textures across non-overlapping logical lifetimes; overlapping heap aliasing remains separate.

## Validation and measurements

Build `Apps/App/MemoryAllocatorTests.vcxproj` using MSBuild with `/p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m:1 /nr:false`, then run `Build/bin/x64/Release/MemoryAllocatorTests/MemoryAllocatorTests.exe` from the repository root. Debug is supported too.

The test enables the D3D12 debug layer and checks suballocation, GPU upload/default/readback copies, upload initialization and growth, texture resize/descriptor stability, copied ownership, zero live allocations after teardown and resource handles outliving the device wrapper. Existing renderer suites cover pass output correctness and runtime resize/switches.

The loading benchmark JSON now includes `gpu_allocation_count`, `gpu_block_count`, `gpu_allocation_bytes` and `gpu_block_bytes` for the primary device's allocator. These exclude swapchain, shared heaps and SDK internals. Block bytes include pool spare capacity; they are not a measurement of total resident VRAM.

### Local validation, 2026-11-02

* App and MemoryAllocatorTests: Debug/Release x64 builds passed; allocator GPU tests passed in both configurations. The 32-buffer pooling test used one block and released all allocations.
* Release GPU suites passed: ReflectionProbeTests (`--debug`), AOTests, SSRTests, DepthOfFieldTests, MotionBlurTests, SecondGroupTests and ReconstructionTests.
* Bistro completed 121 frames (120 measured), exit 0. Mean frame time: baseline 20.1359 ms, final 20.2672 ms. Single runs do not establish a performance improvement.
* Final allocator counters: 227 allocations in 80 blocks, 2,179,665,920 allocated bytes and 2,331,639,808 block bytes (about 145 MiB spare capacity).
* Process private bytes increased from 4,833,308,672 to 4,983,099,392, while working set decreased from 2,810,720,256 to 2,558,427,136. These are process counters, not resident VRAM measurements; this integration does not demonstrate an overall memory reduction.

Logs and JSON: `Build/d3d12ma-*-build.log`, `Build/d3d12ma-*-test.log`, `Build/d3d12ma-tests-{debug,release}.log`, `Build/d3d12ma-before.json`, `Build/d3d12ma-final.json`. Existing project conversion warnings and third-party PDB warnings remain. Multi-GPU runtime, 32-bit builds, extended stability runs and actual PIX capture inspection were not performed.
