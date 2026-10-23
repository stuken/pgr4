// pgr4 - ReXGlue Recompiled Project
//
// GPU queries for the native renderer. RR6 never creates one; PGR3 creates
// occlusion queries (type 9) and event queries (type 8) through the XDK 2099
// API (config/d3d.toml). The guest's D3DDevice_CreateQuery and
// D3DQuery_Release still run, so the query objects and their result slots
// stay the guest's (the device's slot pool is seeded in Direct3D_CreateDevice).
// Nothing ever writes the slots, though: the guest's ring is discarded. So
// Issue does nothing, and GetData answers at once: an event has completed,
// and an occlusion query saw 2^20 samples in each of its four
// counts. That disables the game's occlusion culling and treats every flare
// or probe as visible, which errs towards drawing too much; real host
// occlusion queries are a later step.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/settings.h" // kCvarGroup

REXCVAR_DEFINE_INT32(pgr4_debug_occlusion_samples, 1 << 20, kCvarGroup,
                     "Bring-up experiment: the sample count every occlusion "
                     "query reports");

namespace {

constexpr u32 kQueryTypeEvent = 8;
constexpr u32 kQueryTypeOcclusion = 9;

} // namespace

// (query, flags). The guest body only writes ZPass and fence packets.
REX_HOOK_RAW(D3DQuery_Issue) {
  (void)base;
  (void)ctx;
}

// (query, data, size, flags) -> S_OK (0) with the data, or S_FALSE (1). The
// query object keeps its type at +4.
REX_HOOK_RAW(D3DQuery_GetData) {
  (void)base;
  const u32 query = ctx.r3.u32;
  const u32 data = ctx.r4.u32;
  const u32 size = ctx.r5.u32;
  const u32 type = pgr4::mem::try_load<u32>(query + 4);
  if (auto *out = data ? pgr4::mem::at<be_u32>(data) : nullptr) {
    u32 value = 0;
    if (type == kQueryTypeEvent)
      value = 1;
    else if (type == kQueryTypeOcclusion)
      value = u32(REXCVAR_GET(pgr4_debug_occlusion_samples));
    else
      PGR4_WARN_FIRST(4, "D3DQuery_GetData: query type {} answered with zeros", type);
    for (u32 i = 0; i < size / 4 && i < 4; ++i)
      out[i] = value;
  }
  ctx.r3.u64 = 0;
}
