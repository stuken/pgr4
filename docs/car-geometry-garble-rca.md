# Garbled car geometry after a course change: RCA and fix plan

Status (2026-10-26): fixes 1-3 and the confirm logging are in; the garble has
not been reproduced in a scripted run, and the user is verifying manually. The state audited is the one after the first pass at car
geometry in `native-renderer-plan.md` (Phase 5 findings, "Vertex textures").

## Progress

- **Fix 1 done.** `FindResolveTargetForHeader` returns a header's own target
  only if the header's base is the target's latest resolved base and its
  dimension, guest size and texel layout fit (`ResolveTargetFitsHeader`). It
  returns a by-base alias only if that fits too. The layout check compares the
  block size and bits per pixel rather than the exact format, because the
  bonnet's `k_16_16_16_16_EXPAND` (29) header aliases the
  `k_16_16_16_16_FLOAT` (32) scene. When `GetOrCreateResolveTarget` reuses a
  target's host texture, it now refreshes `guestFormat` and `hostScale` from
  the header.
- **Logging.** `pgr4_log_resolve_match` logs each distinct header the lookup
  turns down or aliases. `pgr4_log_vertex_textures` logs each distinct texture
  bound to slots 16-19 and whether a resolve target, a mirror or nothing backs
  it.
- **First run** (`scratch/runs/h1_fix`, a native solo Tokyo street race with
  both on):
  - The aliases still hold: `0x40E5D08C` to the scene `0x400CD27C` (EXPAND to
    FLOAT, 1280x720), `0x40E5CE84` to `0x400CD248` (424x96), and the depth
    alias `0x40D74CA4` to `0x40E5D08C`.
  - The only rejection was `0x40E5D08C`'s stale front-end depth target, and
    the alias then served that header correctly.
  - No vertex slot was given a wrong resolve target. The one resolve bound to
    a vertex slot is the depth alias on slot 17 (fmt 23, 1280x720), which is
    correct.
- **New facts for H1 and H2.** Every car draw in the run used **one** control
  mesh header (`0x40E5C700`, 8x6 linear `k_8_8_8_8`, base `0x0CA01000`) and
  one 128x128 scale map next to it (`0x40E5C734`). Both headers sit in the
  same pool as the resolve headers (`0x40E5Cxxx`/`0x40E5Dxxx`), so pool reuse
  can plausibly put a resolve header's old address on them (H1). With a single
  shared lattice, H2 now depends on whether the game rewrites it between car
  draws within a frame. Note that `BindGuestTextures`' per-slot skip is frame
  keyed as well, not just the mirror hash.
- **Correction.** Before `cf2188b` the lookup matched by header address
  alone, so `cf2188b` tightened the match rather than loosening it. The gap
  was that neither match checked size.
- **Repro path.** The bug shows mostly on the second course load after a race.
  Scripted with `custom_race.sh`'s extra-script argument (times from the
  content check):
  `start@300s,down@303s,down@305s,down@307s,down@309s,down@311s,a@314s,a@318s,`
  `y@335s,a@341s,up@347s,up@351s,up@355s,a@359s,right@367s,a@381s,down@387s,`
  `down@391s,down@397s,a@405s,start@415s,rt@432s+3000`.
  - The pause menu lists Continue Event, Restart, Photo Mode, Options, Sound
    and Abandon Event. Abandon asks "Are you sure?", where A means yes.
  - Abandon returns to the Custom Match lobby. Y there opens the race-type list
    (Street Race / Eliminator), then the options list (City, Route, Weather,
    Number of Laps, Advanced Options, Lobby), with the cursor on Lobby.
- **Reload run** (`scratch/runs/reload2`, solo, Clear then Light Rain, with
  fix 1): no garble on the Enzo. The run still confirms H1's precondition.
  - After the second load, ordinary course textures take over the old resolve
    headers `0x40E5D08C`, `0x40D74CA4` and `0x400CD248` (DXT1/DXN, various
    sizes). The stale targets stay registered, and race 2 resolves into new
    headers (depth at `0x41B8E878`).
  - All 28 rejections had a format different from the stale target's, so the
    old rule would also have turned them down. The garble needs a
    `k_8_8_8_8` texture (a control mesh) landing on a `k_8_8_8_8` resolve
    header (minimap, Perlin or bloom). Each load is a coin toss, and opponents
    add more tosses.
  - The control mesh header moves between loads and runs: `0x40867EE0` in race
    1, `0x41B8E2FC` in race 2, and `0x40E5C700` in another run.
- **Stale targets leak.** Each reload strands the previous course's resolve
  targets, about 66 MB each at 3x for the FP16 scene. Evicting on a shape
  mismatch is unsafe: the game re-points live resolve headers within a frame.
  `0x40E5D08C` takes the depth resolve and is also used as the EXPAND header
  over the scene, and `0x40D74CA4` samples its depth through the alias. An
  eviction rule needs both "header no longer fits" and "not resolved into for
  N frames".
- **Stale-mirror log.** `pgr4_log_vertex_textures` now records each mirror's
  `HashGuestTexels` at build time (only while the cvar is on). It flags a
  mirror bound after its guest texels changed (`STALE MIRROR`, or `TEXELS
  CHANGED IN FRAME` within its build frame). Comparing by pointer flagged false
  positives on the 156x1 bone palettes, which alternate between two headers
  each frame and get rebuilt into reused pointers.
- **Fix 2 done** (`EvictStaleResolveTargets`, run at most every 60 frames
  from `GetOrCreateResolveTarget`). A resolve target is evicted when nothing
  has resolved into it for 600 frames **and** its own header no longer
  describes it (base, size and layout). The pair avoids evicting the live
  targets whose headers are re-pointed within a frame. With
  `pgr4_log_resolve_match` on, each eviction is logged.
- **Fix 3 done.** A texture bound to a vertex sampler is marked in
  `s_vertex_textures`. Its tiled texels are then hashed in full
  (`HashGuestTexels`, up to 1 MB) instead of by 64 samples. The per-thread
  lookup cache doesn't hand a vertex lookup an entry a pixel lookup cached.
  H2 (rehashing linear textures on every draw) is not done: hardware cannot
  see a texture rewritten between draws either, and no log has shown it.
- **Sanity run** (`scratch/runs/reload3`, all three fixes, reload path): ten
  stale targets were evicted, including the front-end and race-1 targets at
  the reused header addresses. There were no errors and no `STALE MIRROR` or
  `TEXELS CHANGED IN FRAME` flags. Race 2's chase, cockpit, bumper and bonnet
  views render correctly, with the bonnet reflection intact.
- **Next.** The user is verifying the fix manually on the reload repro.

## Symptom

After changing course, some car models load with garbled geometry (spikes,
dents). Other cars on the same course are fine.

## Background

The damage-variant car shaders (`vsCarPaint7` and similar) displace every
vertex using vertex-sampled textures:

- crumple map: vertex sampler 0
- control mesh: vertex sampler 1 (8x6 linear `k_8_8_8_8`, data in the mip tail)
- scale map: vertex sampler 2
- palette variants: `g_VertexColourPalette` on sampler 3

Vertex sampler n is device slot 16+n. The first pass made those slots bind and
fixed the control mesh's tail offset. A car is only right if each of those
textures resolves to the right mirror. If one resolves to the wrong texture,
the vertices move by arbitrary amounts, on that car only.

## Root-cause candidates, ranked

### H1: a stale resolve-target match hijacks a vertex texture (most likely)

`FindResolveTargetForHeader` (`src/gpu/native_texture_mirror.cpp:715`) runs
first in `ResolveOrMirrorGuestTextureSlow`, before any mirror is built. It has
two matches that never check the header still describes a render target:

- **By header address and format.** A header address that was once a resolve
  destination matches on `guestFormat == header.format` alone, ignoring size
  and base. The control mesh is `k_8_8_8_8`, as are the Perlin (128x128),
  minimap (200x200) and bloom (320x180) resolve destinations. A control-mesh
  header landing at one of those old header addresses samples the Perlin or
  bloom image, so the displacement is noise: spikes.
- **By base address.** `g_resolve_by_base.find(base)` matches any header whose
  data base equals an old resolve base.

A course change is when this happens: pools are torn down and reallocated, and
the guest builds its own headers inside pools, so many frees never reach
`D3DResource_Destroy`. `XPhysicalFree` (`src/gpu/hooks/resource.cpp:141`) only
evicts allocations whose exact base was tracked, so nothing evicts a
sub-allocation of a larger pool block. `g_resolve_targets`,
`g_resolve_base_of` and `g_resolve_by_base` then outlive the memory they
describe.

The permissive match is recent (`cf2188b`, the bonnet-camera alias fix, then
`eb008e6`).

### H2: control mesh hashed once per frame

Linear textures are hashed on the first lookup of each frame (lines 958-978);
later draws that frame reuse the hash. If the game rewrites one shared 8x6
mesh per car draw, every later car in the frame draws with the first car's
lattice. A course change brings a grid of opponents rather than one car, which
would expose it. Medium-low likelihood: how the game allocates the texture is
not known.

### H3: tiled textures signed by sparse samples

Tiled textures are signed by 64 sparse 16-byte samples (lines 986-1020). The
crumple and scale maps are exposed: a late streaming write between sample
points is never seen, and the mirror stays as built from the half-streamed
texture. Medium-low likelihood.

### H4: cold vertex and index buffer mirrors

`MirrorGuestBuffer` (`src/gpu/guest_buffers.cpp:146`) checks buffers stable for
60 frames with a 16-sample hash on 7 of every 8 frames. A reloaded buffer at
the same address and size that differs only between samples shows for up to 8
frames, then heals. Unlikely to be a persistent garble.

### Ruled out

- `GpuAddress` / `StreamOffset` handle the 0xE0000000 range (static asserts).
- `Video::SetTexture` ignoring a null bind matches hardware.
- The vertex slot indices (16-19) in `SharedConstants` look consistent.

## How to confirm (no code change to the fix)

1. In a course-change run where a car garbles, log slots 16-19 only: header
   address, fetch format and size, and whether `FindResolveTargetForHeader`
   returned a target. H1 predicts a vertex slot reported as `resolve` with a
   size that does not match the header (for example 128x128 against an 8x6
   control mesh).
2. Run with `pgr4_log_destroy` on. H1 predicts a resolve header address reused
   with no `D3DResource_Destroy` line before the course change.
3. A/B with `pgr4_debug_null_slots` on slots 16-19. If the garble becomes a
   clean uniform dent, the cause is in the vertex texture bindings, not the
   buffers.
4. For H2, trace one multi-car frame (`pgr4_trace_frame`) and check whether
   slot 17's header address and base repeat across cars.

## Fix plan

1. **Tighten the resolve match (H1).** Return a resolve target for a header
   only if format, width and height match the target's guest size and the
   header's base equals the target's last resolved base. Apply the same rule to
   the by-base alias. Drop the stale entry otherwise. The bonnet-camera alias
   still passes, since its base and format agree.
2. **Invalidate resolve entries when memory is reused.** Evict
   `g_resolve_targets`, `g_resolve_base_of` and `g_resolve_by_base` on course
   unload (a large `XPhysicalFree`, or the loading-state transition), or when
   a mirror build or `LockRect` lands on a resolve target's base.
3. **Mirror staleness (H2, H3).** For vertex slots 16-19, rehash linear
   textures on every lookup (they are tiny). Add a whole-texture hash to the
   signature for small tiled vertex textures, or force it for the first N
   frames after a build.
4. **Regression check.** Run the course-change script both ways (garage to
   race, race to race; plan Phase 5 step 6) with `pgr4_log_destroy` on.
   Compare each opponent car with the xenos reference at 1x, plus a 3x pass.
   Confirm the bonnet camera, the Quebec cube reflections and the water normal
   map chain still resolve correctly, since they depend on the alias logic.

Do fix 1 and the logging in "How to confirm" first; they are small and
low-risk. If the log shows H1 is wrong, it also shows which of H2-H4 is next.
