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
2. **Simplex noise, batched SIMD noise**: done in this item's pull
   request (generator 8).
   - Simplex replaces the lattice noise: 3 corners in 2D and 4 in 3D,
     gradients picked with selects, cells counted modulo 2^32 with an
     add instead of a conversion. Each sample goes through an odd
     polynomial fitted so its distribution matches the old noise's
     quantile for quantile; without it a simplex sum never passes 0.56,
     and lakes and cave chambers vanished. Input frequencies are set for
     the old mean slope, and `eroded2d`'s damping for the old share of
     high ground. Biomes, slopes, lakes, rivers, trees and plants stay
     within a few per cent of generator 7 over scattered samples; cave
     air is up about 8 %.
   - Batches (`noise2d_n` and the rest) run the same kernels as plain
     loops, which clang vectorises for SSE4.2 or NEON, with an AVX2 copy
     picked at run time from cpuid. Batches equal single points to the
     bit on either path. Worldgen batches its fields per column, and
     the cave grid skips the blocks between grid heights that cannot
     carve.
   - The climate already sits on the slow fields' 8-block grid with a
     memo (#105), so no coarser grid was needed.
   - Per sample: 2D 20.8 → 15.8 ns, 3D 53.8 → 20.2 ns (AVX2 batch on
     Gracemont). `--bench` worldgen 0.763 → 0.515 ms per column, and
     0.742 → 0.602 ms over scattered columns. `--bench` now generates
     sixteen patches spread over 60 km instead of one 256 m square, so
     one forest no longer decides its number.
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
