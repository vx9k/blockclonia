# Performance series

The optimisation series chosen on 2026-11-18: the CPU, threading and
algorithm work, and the GPU items that go beyond
[rendering.md](rendering.md). Each item lands as its own pull request
with a before and after table (medians of five runs, with min–max) of
`--bench` on the CPU, or of GPU frame times on the Intel ADL-N iGPU,
the weak GPU the renderer is judged on.

## Baseline

`main` at cf10721, profile preset, `--bench`:

| Workload | Time |
|---|---|
| worldgen | 0.960 ms per column (0.953–1.016) |
| gather + mesh | ~0.100 ms per section |
| fluid flood | 0.429 ms per tick |
| `light_region` | 2.5 ms per region, up to 9 regions per edit |

Where the time went (perf): noise ~25 %, `mesh_section_counts` 14 %,
`fluid_run` 13 %, fluid `wake_one` + `queue_push` 14 %. Inside
worldgen, the tree pass was ~47 % (point queries in `tree_site_at`).

## Order

1. **Forest types, tree grid, slow-field memo**: done in #105.
   Forests grow in woods of one main kind (85 %) and one companion. Trees
   root on a jittered 4 x 4 grid, and each column's site list is
   cached and shared by the workers. A per-thread memo keeps the slow
   fields' 8-block grid points. Worldgen 0.960 → 0.633 ms per column.
2. **Simplex noise, batched SIMD noise, coarse climate grid.** Replace
   Perlin with simplex: fewer corners in 3D, no axis artefacts. Evaluate
   noise for 4 or 8 points at once, as plain loops over small arrays
   that the auto-vectoriser turns into SSE/AVX or NEON (the code is C11,
   so no vector extensions); check the assembly.
   Sample climate on a 4 x 4 grid and interpolate. Changes the terrain:
   bump `WORLDGEN_VERSION` and the golden hash. Keep `FP_CONTRACT OFF`
   so x86_64 and arm64 stay bit-identical.
3. **Hybrid incremental lighting.** Ordinary edits use a removal queue
   then an add queue (darken what the old light lit, refill from the
   borders), FIFO so each block is visited once. Big changes
   (explosions, collapses, a queue past a threshold) reflood only the
   sections within 15 blocks instead of whole regions.
4. **Fluid.**
   - Batch the wakes per tick, and skip the structural check when only
     liquid changed.
   - Replace the hash set in `queue_push` with a queued bitmap per
     section.
   - Move the tick's levels to a structure of arrays so the inner loop
     vectorises.
5. **Binary bitmask mesher.** One 64-bit word per row of a section's
   solidity. Face culling becomes shifts and ANDs, and greedy merging
   walks the bits with `ctz`. This replaces the per-block
   `mesh_section_counts` pass. The vertex format stays 4 bytes.
6. **Threads.**
   - Job queue: profile the live game while streaming first, and build
     a work-stealing queue only if the locks in `jobs.c` show up.
   - Parallel physics and fluid by fixed regions, in a fixed order, so
     replay stays exact.
   - Streaming priority: columns in view and near first, and cancel
     jobs for columns that left the view distance.
7. **GPU.**
   - The three items in [rendering.md](rendering.md): occlusion culling,
     cheaper plants, distant terrain detail.
   - GPU compute culling: a compute pass tests every section against
     the frustum and a Hi-Z pyramid of last frame's depth, then writes
     the draws for `vkCmdDrawIndexedIndirectCount`. This needs the
     optional 1.2 feature `drawIndirectCount`, so it gets a `GPUCAP_*`
     bit with today's CPU culling as the fallback. It also tightens the
     section bounds to each mesh's real extent.
   - Parallel command recording: secondary command buffers recorded by
     the workers, one per range of sections, when the draw count is
     high enough to pay for it.

## Measured and left alone

These were looked at and not worth changing: the thermo cell hash, the
structure check, the cave grid, the culling hierarchy, `gpupool` and
item merging. Moving buffers from the heap to the stack gave no
speed-up (#100).

## Later candidates

Not chosen yet, worth measuring when the series is done:

- Compressed section storage (a palette with 4–8 bits a block) for
  memory and cache use at long view distances.
- Mesh upload through a staging ring with `VK_EXT_host_image_copy` or
  host-visible device memory where the device has it (resizable BAR).
- Mesh shaders or meshlets for terrain on devices that have them,
  behind a `GPUCAP_*` bit.
- Profile-guided ordering of the `--bench` workloads, since PGO and
  BOLT are trained on it: keep it covering what the game spends time on.
- SIMD for the thermodynamics diffusion step and the health model's
  per-tissue loops.
- Recording a frame's draws once and replaying them while the camera
  doesn't move across a section boundary.
