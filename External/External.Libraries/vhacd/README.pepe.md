# V-HACD collision baking

Upstream: https://github.com/kmammou/v-hacd

Pinned commit: `f900e42361491f525262d4825e758845e4969897`.
Unmodified standalone V-HACD 4 header. Used to prepare a bounded number of convex
parts from source geometry; results are cached and not recomputed on physics ticks.
License: `LICENSE` (BSD-3-Clause).

Upstream is archived. This self-contained implementation avoids the additional
build dependencies of CoACD; upgrading it requires revalidating collision output
and invalidating the derived geometry cache.
