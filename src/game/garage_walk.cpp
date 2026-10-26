// pgr4 - ReXGlue Recompiled Project
//
// Walking in the garage at more than about 45 fps. The walk camera's
// collision callback (sub_824BB618) splits each frame's step into
// trunc(length * 20) sub-steps and skips the move when that is 0, so a step
// under 5 cm never moves. At 30 fps a full-stick step is 7.5 cm; at 60 fps it
// is 3.7 cm, and the camera stands still. A step that reaches this point is
// not zero (the callback returns earlier for those), so one sub-step moves it
// the whole way, never more than 5 cm, well inside the 0.3 m collision radius.
// That also lets the stick walk below two thirds at 30 fps, which the console
// ignored.

#include <atomic>

#include <rex/ppc/context.h>

#include "core/logging.h"

// Mid-asm hook (config/game.toml, 0x824BB70C): r29 holds the sub-step count.
void GarageWalk_SubstepCount(PPCRegister &r29) {
  if (r29.s32 > 0)
    return;
  static std::atomic<bool> s_logged{false};
  if (!s_logged.exchange(true, std::memory_order_relaxed))
    PGR4_DEBUG("garage walk: a step under 5 cm takes one sub-step");
  r29.u64 = 1;
}
