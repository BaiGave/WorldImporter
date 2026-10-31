# First-wins vertex deduplication

`DeduplicateVertices` visits the original vertex array in order and inserts each
quantized `MakeVertexKey` into a `std::pmr::unordered_map<VertexKey, int>`. The
first original float xyz for each key is retained. A monotonic arena batches
hash-node allocations and releases their memory together. The hash mixes all
three integer coordinates using a 64-bit avalanche function.

Face order, winding, and attributes are preserved during index remapping. The
existing thread-budget contract and explicit face-remapping threads remain.
After GreedyMesh, the second vertex pass is skipped only when the vertex array
size is identical: GreedyMesh only appends vertices and the input vertices are
already unique. GreedyMesh merge, UV, adjacency, palette, and face-filtering
rules are unchanged.

## Reproduction

Worktree: `/workspace/crafter-pipeline/import-opt/wi`, baseline `36a0bde`.
Linux GCC 14, Release/LTO, 8 hardware threads reported.

```sh
cmake -S. -Bbuild-linux --preset release -G Ninja \
  -DCMAKE_CXX_FLAGS="-I/workspace/crafter-pipeline/linux-compat -Wno-deprecated-declarations" \
  -Dlibzip_DIR=/usr/lib/x86_64-linux-gnu/cmake/libzip
cmake --build build-linux -j$(nproc)
ctest --test-dir build-linux --output-on-failure
cp build-linux/WorldImporter ../runs/dense/WorldImporter
cd ../runs/dense
./WorldImporter
```

The existing dense `config/config.json` was preserved byte-for-byte: AABB
(-64, -64, -96)..(63, 200, 31), `cullCave=false`, `modelThreads=1`,
`exportFullModel=true`, `useGreedyMesh=true`. The executable was copied into the
run directory and invoked with that directory as cwd, preserving ExeDir behavior.

## Measurements

Times in milliseconds except wall seconds. `DeduplicateVertices` includes local
container destruction; its internal total is logged before that cleanup.
The fresh baseline below is the second of two runs of the saved original binary.

| Run | Wall (s) | Export | 顶点去重 stage | DeduplicateVertices | Key + hash build | Face remap | Internal total |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Fresh baseline | 2.994 | 2866 | 573 | 173.967 | — | 1.215 | 173.396 |
| Hash run 1 | 2.857 | 2730 | 475 | 84.922 | 78.058 | 1.076 | 79.135 |
| Hash run 2 | 2.823 | 2678 | 485 | 97.893 | 89.601 | 0.976 | 90.577 |

Fresh baseline key calculation / sort / compact scan: 17.428 / 140.176 /
14.577 ms. The sort is eliminated in the hash runs.

Both changed runs skip the second vertex pass; the fresh baseline spends
19.620 ms there and reduces 567315 vertices to 567315. The first pass reduces
1699336 vertices to 567315 in all runs. GreedyMesh keeps **317261 → 147373** faces.
The changed vertex order makes later UV deduplication and GreedyMesh's existing
vertex-key sort somewhat slower; the full 顶点去重 stage still improves.

## Output equivalence

The two changed runs produce byte-identical OBJ output to each other. Compared
with the baseline, the vertex array order and the corresponding vertex references
in `f` lines differ. Restoring the baseline vertex-line order and remapping only
face vertex indices makes the entire OBJ byte-identical to the baseline; every
other byte is unchanged.

A parser resolves each face corner to its original float coordinates, UVs, and
normals, retaining corner order and active material. The entire **ordered** face
sequence matches the baseline exactly, including winding, UVs, and material
assignment. The vertex-coordinate multiset, UV array, and normals also match.
The MTL files are byte-identical. Both OBJs have 570788 exported vertices
(including the later coplanar-face separation) and 147373 faces.

SHA-256 of concatenated `f ` lines, with original newlines:

- Baseline: `203648f56fdfe606bfa8df21d696f7c8a350db4558a85bd6ab6768920780dfd2`
- Changed: `7f18a8176c197885ba0ed598827cf5d50f829ba4312c80794edd96d45762a390`

Raw logs, timing JSON, preserved OBJs/MTLs, the baseline binary, config snapshot,
and measurement/comparison scripts are in
`/workspace/crafter-pipeline/import-opt/runs/dense/dedup-hash-evidence/`.
The final executable and OBJ/MTL remain in the dense run directory.

## Regression checks

CTest passes `model_deduplicator` and the existing `biome_colors` suite. The new
checks exercise first-wins coordinates and order, positive/negative half-step
rounding, distinct keys that collided under the old hash, exact face remapping,
face metadata/UV/overlay preservation, idempotence, empty meshes, and meshes
without faces. Both the serial remap (budget 2) and hardware-thread remap
(budget 1) run through the same fixtures. Checks remain active in Release builds.
