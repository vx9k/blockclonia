# Rendering performance

The next three renderer optimisations, in order (planned 2026-11-18).
None of them has started. The weak GPU they are measured on is the
Intel ADL-N iGPU; each lands as its own pull request with a before and
after table of GPU frame times there.

What is done and what is next lives in [TODO.md](../../TODO.md); this page
holds the reasoning and the detail. What the renderer already does is in
the README's "Built for low-end devices" table.

## Where it stands

`collect_visible` in `renderer.c` culls only against the view frustum,
per column and per 16³ section, and per section skips the face groups
that point away from the camera. Nothing checks whether a section is
hidden behind others, so caves under the player and valleys behind a
ridge are drawn in full. Every column is meshed at full detail out to
the render distance. Since the vegetation series, ground plants are two
crossed quads drawn from both sides, four quads a plant, to the edge of
the view; on the iGPU they roughly doubled the terrain's GPU time in a
meadow (1.32 ms to 3.10 ms).

## 1. Occlusion culling

A visibility graph between sections, walked on the CPU each frame. It
needs nothing from the GPU, so it works the same on bare Vulkan 1.2,
MoltenVK and lavapipe.

- **What the mesher records:** for each section, which of its six faces
  can see each other through cells that are not opaque: a flood fill
  over the section's open cells, noting the faces each open region
  touches. Fifteen face pairs, 15 bits a section. A section with no
  open cells connects nothing; an all-air section connects everything
  and needs no fill.
- **The walk:** a breadth-first search from the camera's section. A
  step goes from a section out through a face to its neighbour only if
  the section connects the face it was entered by to the face it leaves
  by, the step leads away from the camera (never back toward it), and
  the neighbour is in the frustum. Sections reached are drawn; the rest
  are not. The walk replaces the spiral order and keeps the list front
  to back for early-Z.
- **What counts as open:** the cells light passes through and that are
  not full cubes: air, water, glass, leaves and plants. A leaf canopy
  must not hide the ground under it.
- **Edges:** sections not yet meshed count as open, so terrain never
  vanishes while it streams in. The camera's own section starts the walk
  from every face. In spectator mode inside rock, culling is off.
- **Shadows** keep their own culling: the shadow map sees from the sun,
  so it still draws every section within its reach.
- **Cost:** the fill is part of meshing, on the workers. The walk visits
  only the sections it draws and their neighbours. A setting is not
  needed, but `MC_NO_OCCLUSION=1` turns it off to compare.
- **Tests:** the face-pair bits for an empty, a solid, a tunnel and a
  room section; a walk through a stack of sections where a solid floor
  hides the cave under it and a hole in it doesn't.

## 2. Cheaper plants

- **Two quads, not four:** a second cut-out pipeline with culling off
  draws each crossed plane once from both sides. The normal for lighting
  flips in the fragment shader with `gl_FrontFacing`.
- **A plant draw distance:** plants beyond it are left out of the draw,
  with the fog hiding the edge. The mesher already puts plants in their
  own range after the leaves, so the range can end early per section.
  A setting with Auto, Near, Far and Off; Auto picks from the GPU
  timestamps, as bloom and shadows do.
- **Tests:** the plant quad count per plant; the setting's parse and
  write.

## 3. Distant terrain detail

Far columns get coarser meshes: 2 x 2 x 2 blocks merged into one beyond
one distance, 4 x 4 x 4 beyond a second.

- **Building them:** a coarse cell takes the block most of its cells
  hold, weighted toward the surface so peaks and shores keep their
  outline. The workers build coarse meshes from the same snapshots.
- **Seams:** where a coarse column meets a finer one, the coarse side
  gets skirts (short faces hanging down from its edges), so no gaps show
  through to the sky.
- **Swapping levels:** a column changes level with some hysteresis, so
  walking along the boundary doesn't rebuild meshes every frame.
- **Memory:** a coarse mesh is about a quarter (2 x 2) or a sixteenth
  (4 x 4) of the full one, which also lets the render distance grow on
  machines whose budget allows it (`sys_plan`).
- **Tests:** a coarse column's cells against the full one's; skirts on
  every face that borders a finer level.
