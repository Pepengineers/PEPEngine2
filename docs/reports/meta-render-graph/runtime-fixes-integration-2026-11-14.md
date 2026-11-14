# MetaRenderGraph runtime fixes integration — 2026-11-14

Base: `aabb2744`. Source fixes: `791db96c` and `a287c9a5` from the MisSpec validation branch; its latest fetched head is `175e4189`.

- Drain the owned executor before swapchain resize/provider recreation, then release UI composite backbuffer owners. Preserve U9/U10 producer Stop and retirement paths.
- Allocate Meta transient texture descriptors after the fixed texture/cubemap domains; the production App retains its existing allocation policy.
- Declare the XeGTAO normal input, omit unused probe-shadow SRVs and omit conflicting SSR preparation SRVs.
- Freeze the selected reflection radiance handle for transparent geometry and water resolve. Legacy water callers retain the default handle.
- Use the reconstruction render extent for deferred lighting and WBOIT; raster diagnostic paired posters only once per displayed frame.

The existing startup/include fixes from `aabb2744` remain intact. No source files were replaced wholesale. Test targets, expanded numerical/edge-case fixtures, validation matrices, readback tooling, benchmark instrumentation and historical evidence CSVs were not imported. Existing diagnostic functions were not expanded.

Source review covered declarations/callers, default legacy behavior and retained U9/U10 hooks. `git diff --check` passed. Builds, tests, application runs and captures: **NOT RUN**, per the source-only instruction. This report does not claim runtime acceptance of the combined branch or Bistro integration.
