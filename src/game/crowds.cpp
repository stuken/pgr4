// pgr4 - ReXGlue Recompiled Project
//
// pgr4_crowd_draw_distance and pgr4_crowd_detail_distance: draw the crowds
// further out. The crowd scene build (sub_82404ED8, once per view) walks the
// course's crowd cells that the camera's PVS row marks visible and measures
// each cell's box from the eye, scaled by tan(fov / 2) (the view's +108), so
// zooming in reaches further. A cell nearer than 10 * the cells' +52 (2.5, so
// 25) draws its people as skinned models with their flags; one nearer than
// +48 draws as sprites, with its flags only inside Crowds.Flags.FlagDrawDistance
// (50). +48 is the course's CrowdSettings.DrawDistance, set every frame
// (sub_82402818), or 120 when the course has none. At a 60 degree FOV that
// puts the models out to about 43 m and the sprites to about 210 m.
//
// The two mid-asm hooks (config/game.toml) scale the thresholds as the build
// loads them, so the game's own fields keep their values and the cvars apply
// live. The sprite path appends each cell to a fixed list in the crowd
// renderer with no bounds check, so CrowdRenderer_AddSpriteCell replaces that
// append with a checked one.

#include <algorithm>
#include <atomic>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/settings.h"

REXCVAR_DEFINE_DOUBLE(pgr4_crowd_draw_distance, 1.0, kCvarGroup,
                      "Multiply the distance out to which crowds are drawn "
                      "(as sprites beyond the near range) by this (1 = the "
                      "game's)")
    .range(1.0, 4.0);

REXCVAR_DEFINE_DOUBLE(pgr4_crowd_detail_distance, 1.0, kCvarGroup,
                      "Multiply the distance out to which crowds are drawn as "
                      "full models rather than sprites by this (1 = the "
                      "game's)")
    .range(1.0, 4.0);

namespace {

// The crowd cells (the scene build's r3): the cell count, and the two
// distances in the units the build compares, metres * tan(fov / 2).
constexpr u32 kCellsCountOffset = 28;
constexpr u32 kCellsFarOffset = 48;
constexpr u32 kCellsNearOffset = 52;
// The view (r4): the eye at +96, tan(fov / 2) at +108.
constexpr u32 kViewFovScaleOffset = 108;

// The crowd renderer's sprite cell list: a count, then the cells' addresses,
// up to the next field at 0x209BC.
constexpr u32 kSpriteCountOffset = 0x1E5C8;
constexpr u32 kSpriteListOffset = 0x1E5CC;
constexpr u32 kSpriteListCapacity = (0x209BC - kSpriteListOffset) / 4;
// Its ring of blocks drawn as models.
constexpr u32 kModelCountOffset = 8868;
constexpr u32 kModelRingCapacity = 880;

} // namespace

REX_EXTERN(__imp__CrowdRenderer_AddModelBlock);

// Mid-asm hook (0x82404F08): f0 holds the cells' +52, which the build then
// multiplies by 10 for the model range. r3 is the cells, r4 the view.
void Crowds_ScaleDetailDistance(PPCRegister &f0, PPCRegister &r3,
                                PPCRegister &r4) {
  static std::atomic<u32> s_last_cells{0};
  if (s_last_cells.exchange(r3.u32, std::memory_order_relaxed) != r3.u32) {
    PGR4_DEBUG("crowds: cells 0x{:08X}, {} cells, far {:.1f}, near {:.1f} x10, "
               "view tan(fov/2) {:.3f}",
               r3.u32, pgr4::mem::load<u32>(r3.u32 + kCellsCountOffset),
               pgr4::mem::load<float>(r3.u32 + kCellsFarOffset),
               pgr4::mem::load<float>(r3.u32 + kCellsNearOffset),
               pgr4::mem::load<float>(r4.u32 + kViewFovScaleOffset));
  }
  f0.f64 *= REXCVAR_GET(pgr4_crowd_detail_distance);
}

// Mid-asm hook (0x823E96A4): f1 holds the cells' +52 on its way to the crowd
// update (sub_823E3B40), which marks a cell far once every view is at least
// 10 times it away. The model range above scales the same, so no cell drawn
// as models is updated as far.
void Crowds_ScaleUpdateDistance(PPCRegister &f1) {
  f1.f64 *= REXCVAR_GET(pgr4_crowd_detail_distance);
}

// Mid-asm hook (0x82404F34): f0 holds the cells' +48, the sprite range.
void Crowds_ScaleDrawDistance(PPCRegister &f0) {
  f0.f64 *= REXCVAR_GET(pgr4_crowd_draw_distance);
}

// Mid-asm hook (0x82404F54): v12 element 0 holds Crowds.Flags.FlagDrawDistance,
// the range of the sprite cells' flags. It scales with the sprites.
void Crowds_ScaleFlagDistance(PPCVRegister &v12) {
  const float scale = static_cast<float>(REXCVAR_GET(pgr4_crowd_draw_distance));
  for (float &lane : v12.f32)
    lane *= scale;
}

// sub_823FD7D0 (renderer r3, block r4): appends a block of people drawn as
// models to a ring of 880 (count at +8868, ring at +8864). The draw takes the
// last 880, so a fuller frame loses its first blocks rather than corrupting
// anything. Logged to size the model range.
REX_HOOK_RAW(CrowdRenderer_AddModelBlock) {
  const u32 renderer = ctx.r3.u32;
  __imp__CrowdRenderer_AddModelBlock(ctx, base);
  const u32 count = pgr4::mem::load<u32>(renderer + kModelCountOffset);
  static std::atomic<u32> s_max_count{0};
  u32 seen = s_max_count.load(std::memory_order_relaxed);
  if (count > seen &&
      s_max_count.compare_exchange_strong(seen, count,
                                          std::memory_order_relaxed) &&
      (count % 100 == 0 || count == kModelRingCapacity + 1))
    PGR4_DEBUG("crowds: {} model blocks in a frame", count);
}

// sub_823FC1B0 (renderer r3, cell r4): appends the cell to the sprite list.
// The game's version stores past the list's end when it is full.
REX_HOOK_RAW(CrowdRenderer_AddSpriteCell) {
  (void)base;
  const u32 renderer = ctx.r3.u32;
  const u32 count = pgr4::mem::load<u32>(renderer + kSpriteCountOffset);
  static std::atomic<u32> s_max_count{0};
  if (count >= kSpriteListCapacity) {
    static std::atomic<bool> s_warned{false};
    if (!s_warned.exchange(true, std::memory_order_relaxed))
      PGR4_WARN("crowds: the sprite cell list is full ({} cells); dropping "
                "the rest",
                kSpriteListCapacity);
    return;
  }
  pgr4::mem::store<u32>(renderer + kSpriteListOffset + 4 * count, ctx.r4.u32);
  pgr4::mem::store<u32>(renderer + kSpriteCountOffset, count + 1);
  u32 seen = s_max_count.load(std::memory_order_relaxed);
  if (count + 1 > seen &&
      s_max_count.compare_exchange_strong(seen, count + 1,
                                          std::memory_order_relaxed) &&
      (count + 1) % 50 == 0)
    PGR4_DEBUG("crowds: {} sprite cells in a frame", count + 1);
}
