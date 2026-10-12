# Foliage model and export performance regression (local)

`foliage_model_tests` exercises the real JSON model parser and deduplicator:

- 22.5 degree element rescale uses `1 / cos(angle)` on perpendicular axes.
- Zero-thickness elements retain their explicitly defined front/back faces.
- UV locking handles geometry with no `cullface` (all 96 cardinal-face X/Y variants).
- UV locking preserves extended UVs and animation frame-local coordinates.
- Face dedup canonicalizes cyclic starts but preserves winding.

Build with `BUILD_TESTING=ON` and run `ctest -R foliage_model --output-on-failure`.
All leaf cases failed on the pre-fix implementation; they pass after the fixes.
No specific third-party foliage pack was supplied, so visual equivalence with the
user's problematic resource pack remains unverified.

## Performance observations

Local single-run comparisons, MSVC Release static, 4 model threads, full model
output, greedy mesh enabled, 20 section tasks per batch, partition size 1.
World: Minimized Townscape 2024, Y=20..95. Measurements include initialization
and final export; they are not statistically controlled multi-run benchmarks.

| Resources / selected region | Before | After |
| --- | ---: | ---: |
| Mods only (mcmeta grid), X/Z=-32..31, 4 batches | 17.941 s | 5.462 s |
| Yuushya Modpack Inside.zip, same region | 6.415 s | 6.156 s |
| Same pack, X=-96..31 Z=-64..63, 16 batches | 11.988 s | 9.970 s |

The mcmeta grid parser previously copied JSON and decoded the atlas PNG for each
face. Positive and negative thread-local metadata caching eliminates this work.
Other changes avoid rebuilding blockstate/name strings for the entire growing
palette each batch, reuse per-group material lookups, avoid throwaway mesh
reserves, and ignore empty section meshes during append.

The full export still accumulates the final mesh in memory; at very large scale
memory pressure/page faults can cause later stages to slow down. Logs now show
accumulated face count and backing-array capacity (not total process memory).
The 16-batch test did not reproduce a monotonic slowdown: individual batch times
also depend on model density and first-time model/texture initialization.

More geometry is expected after preserving reverse leaf faces. OBJ byte counts
are consequently not a valid equivalence test for this change.
