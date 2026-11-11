# Water spatial-index alternatives: primary-source research

Date: 2026-11-12. Scope: source research and static algorithm comparison only. No production changes, compilation, GPU execution, or measured speedups. Dispatch counts below are derived command counts, not timing predictions.

## Recommendation

For the smallest production change, first evaluate a group-shared bitonic hybrid preserving today's `(bucket, particle index)` comparator and existing buffers: 21 sorting dispatches at 2048 records/block. The already-vendored FidelityFX Parallel Sort is the strongest reuse alternative, sorting only meaningful hash bits and only `Count` live records, but its 20 dispatches alone do not justify extra buffers and graph ownership compared with 21. For a more ambitious alternative, benchmark direct histogram/scan/scatter bucket construction: water needs contiguous bucket membership, not a general comparison sort. Prefer conventional multi-dispatch radix over OneSweep for an initial portable D3D12 implementation.

This is an engineering recommendation based on reuse and correctness risk, not a benchmark winner. At approximately 52K particles, dispatch overhead, occupancy, barriers and neighbor traversal can matter more than throughput measured on hundreds of millions of keys.

## Local contract inspected

`Apps/App/Shaders/FluidSolver.hlsl` generates `(Key(Cell(predicted position)), particle index)`. `Key` masks to `Capacity-1`. Its comparison sort orders by key, then index. Density and integration start from `Offsets[key]`, stop when the key changes, and check the actual cell to reject hash collisions. Thus contiguous buckets suffice for membership correctness; ascending particle-index ties additionally preserve accumulation order.

`Apps/App/src/Rendering/Water/GDX12Fluid.cpp` rounds capacity to a power of two and dispatches every bitonic stage separately, each followed by a UAV barrier. For capacity 65536, `log2(Capacity)=16`, giving `16*17/2=136` sorting dispatches per substep, or 408 over three substeps. Hash, offsets, density and integrate are additional dispatches.

The repository already has `External/Particles/ffx_parallelsort.h`, documented in `External/Particles/README.md` as an unmodified import at FidelityFX SDK commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`. `Engine/Engine.RendererDX12/Shaders/ParticleSort.hlsl` supplies callbacks and Count/Reduce/Scan/ScanAdd/Scatter entry points. It uses separate key/payload buffers, whereas water currently uses `uint2`; adapt callbacks or water storage deliberately instead of assuming the bindings are interchangeable.

## Dispatch comparison at 65536 capacity, 51975 live particles

| Candidate | Sorting/index construction dispatches per substep | Three substeps | Count basis and caveat |
|---|---:|---:|---|
| Current global bitonic | 136 | 408 | Sort only; source loop |
| Shared-memory bitonic, 512 records/block | 36 | 108 | Sort only; derived schedule below |
| Shared-memory bitonic, 2048 records/block | 21 | 63 | Sort only; Microsoft sample adds one indirect-argument dispatch |
| FidelityFX, full 32 bits | 40 | 120 | Eight digits, five dispatches each |
| FidelityFX, water's 16 bits | 20 | 60 | Four digits; hash and offsets remain separate |
| GPUSorting DeviceRadixSort, full 32 bits | 13 | 39 | Init plus four sets of upsweep/scan/downsweep |
| DeviceRadixSort adapted to 16 bits | 7 | 21 | Init plus two digits; an adaptation, not the stock loop |
| GPUSorting OneSweep reference, full 32 bits | 7 | 21 | Unity host reference: init, histogram, scan, four binning dispatches |
| OneSweep adapted to 16 bits | 5 | 15 | Same setup plus two binning dispatches; requires a validated adaptation |
| Direct bucket histogram + hierarchical scan + scatter | 6 | 18 | Proposed full index construction, including clear/hash; not implemented |

Do not compare the last row as if it still needs the old hash and BuildOffsets passes. Conversely, do not call a radix digit a single dispatch when its implementation contains several kernels. Resource transitions and UAV barriers remain necessary at producer/consumer boundaries.

## Shared-memory bitonic hybrid

Microsoft's MiniEngine implements pre-sort up to 2048 records, global outer passes, then an inner merge in shared memory. Its host source carries an MIT notice. [Microsoft implementation](https://raw.githubusercontent.com/microsoft/DirectX-Graphics-Samples/master/MiniEngine/Core/BitonicSort.cpp).

For `N=2^m` and block size `B=2^b`, one block pre-sort followed by each global merge with its local tail gives `1 + (m-b)(m-b+3)/2` dispatches. Hence 36 at B=512 and 21 at B=2048. These numbers assume local tails are collapsed at every merge level, not just an initial local sort. Shared storage for 2048 `uint2` records is 16 KiB before any other scratch. Larger blocks reduce launch count but can reduce occupancy; the sample's block size is not a measured optimum for this shader.

Keep the existing lexicographic comparator and sentinels to preserve exact integer output. A network is not generally stable by key alone; unique index tie-breaking supplies the deterministic total order here.

Microsoft's shader specifically uses 1024 threads, each comparing one pair per stage, and two 2048-element shared uint arrays (16 KiB total). A 512-thread/two-comparisons variant is possible but is a separate adaptation. Its sample compares keys only and stores `(index,key)`, opposite water's layout: copy the scheduling idea, not those semantics. [Pre-sort shader](https://raw.githubusercontent.com/microsoft/DirectX-Graphics-Samples/master/MiniEngine/Core/Shaders/Bitonic32PreSortCS.hlsl). The occupancy comment in that sample is not a guarantee for this application's target hardware.

## FidelityFX radix: reuse first

AMD documents four bits per digit and five stages: count, reduce, reduced scan, scan-add and scatter. Full uint32 sorting therefore needs eight digits. The implementation supports key/payload sorting, D3D12, HLSL SM6 wave operations, and MIT licensing. [AMD overview](https://gpuopen.com/fidelityfx-parallel-sort/), [technique documentation](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/parallel-sort/).

For water, `Key < Capacity=65536` means only low 16 bits carry information. Four digits suffice. More generally use `ceil(log2(Capacity)/4)`; keep the header's four-bit digit size unchanged, since AMD explicitly warns that changing it requires internal LDS/count/scan/scatter changes. [GPU reference](https://gpuopen.com/manuals/fidelityfx_sdk/reference_documentation/sdk/gpu/fidelityfx_gpu_references/fidelityfx_parallel_sort/ffx_g_p_u_parallel_sort/).

Sort only live `Count` entries. Stable LSD radix applied to input ordered by particle index preserves today's tie order without sorting the payload as another 32-bit key. Revalidate stability in the selected implementation and callbacks. If sorting padded entries while discarding sentinel upper bits, the sentinel's low 16 bits alias bucket 65535; do not rely on its full-uint maximum ordering after a partial-bit sort. Avoid that contract entirely by passing live count.

Stability is source-derived here: the pinned `ffx_parallelsort.h` scatter uses exclusive prefix ranks for its local two-bit rearrangements, preserves payload with each key, adds ordered group offsets, and advances cached bin offsets between successive input chunks. Equal digits retain input order at each digit. This inspection supports stable sorting for the unchanged algorithm; it does not validate a new callback/buffer integration on GPU. [Exact pinned AMD source](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/include/FidelityFX/gpu/parallelsort/ffx_parallelsort.h).

Odd/even digit count determines which ping-pong allocation contains the final data. Keep offsets and consumers bound to the actual final buffer for every capacity, not only the four-digit example.

## Modern radix and OneSweep

GPUSorting's D3D12 `DeviceRadixSort::PrepareSortCmdList` explicitly dispatches an initialization followed by upsweep, scan and downsweep for each eight-bit digit. Its stock loop processes 32 bits. Reading that source yields 13 dispatches; restricting the loop to 16 meaningful bits yields seven, subject to corresponding integration and tail checks. [D3D12 source](https://raw.githubusercontent.com/b0nes164/GPUSorting/main/GPUSortingD3D12/DeviceRadixSort.cpp).

The OneSweep paper reduces digit-binning global traffic from approximately 3N to 2N operations. Its headline benchmark is on 256M keys on an NVIDIA A100; it does not establish performance for this 52K key/payload water workload. [Adinets and Merrill, 2022](https://arxiv.org/abs/2206.01784).

The readable Unity reference performs InitSweep, GlobalHistogram and Scan once, then four eight-bit DigitBinningPass dispatches. This proves seven for that reference, not a fully inspected D3D12 wrapper. The D3D12 OneSweep host file could not be retrieved during this investigation. [OneSweep Unity source](https://raw.githubusercontent.com/b0nes164/GPUSorting/main/GPUSortingUnity/Runtime/OneSweep.cs).

The author explicitly recommends DeviceRadixSort when portability matters: OneSweep's chained look-back depends on forward progress. Runtime wave-size adaptation is claimed, with testing on wave sizes 4, 16, 32 and 64; the D3D12 implementation is described as a demo. This is a useful source implementation, not a plug-in production dependency. [Author README](https://github.com/b0nes164/GPUSorting).

Wave32/wave64 support and forward progress are separate concerns. Correct wave arithmetic does not prove that a waiting group permits a predecessor group to run. An interlocked operation also supplies no implicit memory fence. Use proper publication/barrier semantics and do not infer cross-group scheduling guarantees from a successful run on one adapter. HLSL wave size is implementation-dependent, not universally 32. [Microsoft WaveGetLaneCount](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/wavegetlanecount), [Microsoft InterlockedAdd](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/interlockedadd).

AMD's newer OneSweep extension uses circular buffers to reduce scratch growth and supplies an Orochi implementation. That improves the research options, but adds porting work to a D3D12 HLSL engine; it does not remove the need to validate synchronization. [AMD circular-buffer extension](https://gpuopen.com/learn/boosting_gpu_radix_sort/).

GPUSorting's own code is MIT, but its combined LICENSE also contains third-party notices including CUB and a segmented-sort LGPL component. Import only the selected implementation and retain relevant notices; do not assume the entire benchmark dependency tree has one license. [Repository license inventory](https://raw.githubusercontent.com/b0nes164/GPUSorting/main/LICENSE).

## Construct buckets without comparison sorting

Proposed six-dispatch schedule for 65536 buckets, with a 512-bucket scan tile:

1. Clear bucket counts/cursors.
2. Compute each particle's key; atomically increment its bucket count.
3. Exclusive-scan each 512-bucket tile and write 128 tile totals.
4. Scan the 128 tile totals in one group.
5. Add tile prefixes to bucket offsets; initialize separate scatter cursors. Preserve empty-bucket behavior expected by consumers.
6. Scatter `(bucket, particle index)` to `bucketStart + atomicIncrement(cursor[bucket])`.

This is a local design derived from histogram/exclusive-scan primitives, not a claim about a shipped library. The hierarchical scan pattern and radix scatter rationale are documented by NVIDIA. [GPU Gems scan](https://developer.nvidia.com/gpugems/gpugems3/part-vi-gpu-computing/chapter-39-parallel-prefix-sum-scan-cuda), [NVIDIA spatial subdivision and radix sort](https://developer.nvidia.com/gpugems/gpugems3/part-v-physics-simulation/chapter-32-broad-phase-collision-detection-cuda).

An optional variant stores the old atomic count as each particle's local rank during histogramming, then uses that rank during scatter; this avoids the scatter atomic but requires rank storage and keeps nondeterministic tie order. Both variants keep exact allocation `Count` and avoid fixed per-bucket capacities or overflow/drop behavior. Work is O(Count+Capacity); each additional uint bucket array costs 256 KiB at capacity 65536. Hot or colliding buckets can serialize atomics. Clearing/scanning a large mostly empty table also costs real bandwidth.

Atomic reservation order is not particle-index order. Membership stays correct, but floating-point sums and consequently simulation trajectories can change. If preserving the existing accumulation sequence is required, use stable radix or pay for deterministic per-bucket ordering. Continue actual-cell rejection in both neighbor kernels; changing the index construction does not make hash buckets unique spatial cells.

## Validation gates before selecting a winner

Compare exact sorted `(key,index)` output for deterministic candidates, and exact per-bucket membership for atomic construction. Include empty/one-element input, partial groups, counts around powers of two, all-equal keys, collisions between distinct cells, maximum bucket key, capacity changes and maximum supported particle count. Verify every particle occurs exactly once, offsets identify the intended bucket, and padded entries never enter live traversal.

Benchmark full substep cost, not only the sorting shader: record GPU time for index construction, density and integration, plus CPU command recording and total frame time. Test representative dense water and dispersed/collision-heavy distributions on intended wave32/wave64 adapters. Check repeated runs for deterministic variants and numerical drift for atomic variants. No such validation was performed for this note.
