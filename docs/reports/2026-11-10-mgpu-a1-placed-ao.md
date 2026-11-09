# Placed AO recording — 2026-11-10

The AO pass now shares its preparation, algorithm evaluation and upsample
recorders between the legacy local path and a placed slot. Pack writes typed
R32_FLOAT depth and R16G16B16A16_FLOAT normals at the working resolution.
Evaluate accepts the pair and writes resolved R32_FLOAT visibility. Finalize
copies Full visibility or upsamples Half visibility into the existing
full-resolution lighting texture. The placed callbacks use a frozen frame
snapshot and do not submit or wait on queues.

A slot publishes all its external texture accesses and the corresponding
native handles. P4 can check those handles against the executor bindings.
Engine scratch and upload pages belong exclusively to their invocation
slot. Its engine scratch transitions to COMMON before the callback returns.
CACAO keeps its native private states and gets a single-page context per
evaluation slot. The only vendor addition is same-layout rebind of external
views after retirement. The recorder rejects preparation while another
owner retains the slot. The shader build and legacy CACAO initialization
are unchanged.

## Verification

Base `8fa01af240733db82a060c6e5c7ae9d1bb10d5c9`. The final runner states and
their stdout/stderr logs are in the A1 worktree's `Build/A1`.

| Check | Runner state |
| --- | --- |
| AOTests Debug/x64 with the local P0 /Z7 override | `build-ao-tests-debug-final.json` |
| CPU CVar defaults and validation | `ao-cvar-cpu-final.json` |
| Debug Layer with `--ao-only` | `ao-placed-debug-layer-final.json` |
| GPU-Based Validation with `--gpu-validation` | `ao-placed-gbv-final.json` |

All four returned exit 0 with empty stderr. Both GPU runs reported
`maxError=0` for all eight algorithm/resolution pairs and both CACAO rebind
cases. Each placed result was compared per pixel with a local reference on
the same adapter after changing live AO CVars. The Half grid was 33×25 for
a 65×49 input. The fixture recorded and discarded Pack, Evaluate and Finalize
without advancing either submission timeline or changing a boundary tracker.
It also held the Compute receipt in flight, confirmed that a retained CACAO
slot rejected reprepare, and rebound it to a distinct native depth/normal
pair after retirement. The A0 local Full/Half, queue parity, neutral output,
history, jitter, and resize fixtures also passed.

The first build rejected the fixture's abbreviated function template under
the project's C++17 mode. The first Debug Layer run caught a missing call
to apply the frozen settings to native CACAO after extraction. Both were
corrected before the final runs. The checks reused the local A0 dependencies
and immutable INI seed. The validated executable SHA-256 was
`33744942cb027690a5819a90d05a571605aae2b79a5583993d67855a9e4b2a40`.
`processes-final.json` records the source fingerprints and zero remaining
packet workloads.

The focused fixture emulates P2b boundary transitions on one physical
adapter. The ExactVersion transport, two physical adapters and production
Bistro early/late bindings remain part of integrated A1 acceptance.
Performance and preset tuning were outside this verification.
