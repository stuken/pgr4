// pgr4 - ReXGlue Recompiled Project
//
// pgr4_bumper_camera_height: raises the bumper camera. The game places that
// view's eye at the car's bumper mount, plus a height along the car's up axis
// that it leaves at 0 for the bumper mode (Camera_PlaceBumper in
// config/game.toml), so it sits low on most cars. The other camera modes that
// reuse the placement pass 0.5 and are left alone.

#include <atomic>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/settings.h"

REXCVAR_DEFINE_DOUBLE(pgr4_bumper_camera_height, 0.0, kCvarGroup,
                      "Raise the bumper camera by this many metres (0 = the "
                      "game's height, up to 1)")
    .range(0.0, 1.0);

REX_EXTERN(__imp__Camera_PlaceBumper);

namespace {

// Cam::CameraImp's mode field, and the bumper view's mode.
constexpr u32 kCameraModeOffset = 496;
constexpr u32 kCameraModeBumper = 257;

} // namespace

REX_HOOK_RAW(Camera_PlaceBumper) {
  const u32 mode = pgr4::mem::load<u32>(ctx.r3.u32 + kCameraModeOffset);
  static std::atomic<u32> s_last_mode{0};
  if (s_last_mode.exchange(mode, std::memory_order_relaxed) != mode)
    PGR4_DEBUG("bumper placement for camera mode {} (camera 0x{:08X})", mode,
               ctx.r3.u32);
  if (mode == kCameraModeBumper)
    ctx.f1.f64 += REXCVAR_GET(pgr4_bumper_camera_height);
  __imp__Camera_PlaceBumper(ctx, base);
}
