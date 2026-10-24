// pgr4 - ReXGlue Recompiled Project
//
// GPU queries for the native renderer. PGR4 creates occlusion queries (type
// 9) for its light glows and a few view probes, and event queries (type 8).
// The guest's D3DDevice_CreateQuery and D3DQuery_Release still run, so the
// query objects stay the guest's, but the guest's ring is discarded and
// nothing writes their result slots. So Issue and GetData are answered here:
// an event has completed at once, and an occlusion query is counted by host
// occlusion queries (gpu/occlusion.h), "not yet" until its frame's fence.
//
// The 6132 GetData of an occlusion query fills up to four dwords: the
// samples that passed the depth test, those that failed it, those that
// failed the stencil test, and the total. The host counts only passes, so
// the total reads the same: a test the game reads as a ratio (sub_826A12A8,
// 1 - passed / total) sees all or nothing.

#include <algorithm>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/settings.h" // kCvarGroup
#include "gpu/occlusion.h"

REXCVAR_DEFINE_INT32(pgr4_debug_occlusion_samples, 1 << 20, kCvarGroup,
                     "Bring-up experiment: with pgr4_occlusion_queries off, "
                     "the sample count every occlusion query reports");

namespace {

constexpr u32 kQueryTypeEvent = 8;
constexpr u32 kQueryTypeOcclusion = 9;
constexpr u32 kIssueEnd = 1;
constexpr u32 kIssueBegin = 2;

} // namespace

// (query, flags). The guest body only writes ZPass and fence packets.
REX_HOOK_RAW(D3DQuery_Issue) {
  (void)base;
  const u32 query = ctx.r3.u32;
  const u32 flags = ctx.r4.u32;
  if (pgr4::mem::try_load<u32>(query + 4) == kQueryTypeOcclusion &&
      pgr4::gpu::occlusion::Enabled())
    pgr4::gpu::occlusion::Issue(query, flags & kIssueBegin, flags & kIssueEnd);
}

// (query, data, size, flags) -> S_OK (0) with the data, or S_FALSE (1). The
// query object keeps its type at +4.
REX_HOOK_RAW(D3DQuery_GetData) {
  (void)base;
  const u32 query = ctx.r3.u32;
  const u32 data = ctx.r4.u32;
  const u32 size = ctx.r5.u32;
  const u32 type = pgr4::mem::try_load<u32>(query + 4);
  auto *out = data ? pgr4::mem::at<be_u32>(data) : nullptr;
  const u32 dwords = std::min(size / 4, 4u);
  if (type == kQueryTypeOcclusion && pgr4::gpu::occlusion::Enabled()) {
    u32 samples = 0;
    const bool ready = pgr4::gpu::occlusion::Result(query, samples);
    // Not ready reads as the guest's own answer then: 1 passed of 1.
    const u32 passed = ready ? samples : 1;
    const u32 counts[4] = {passed, 0, 0, passed};
    for (u32 i = 0; out && i < dwords; ++i)
      out[i] = counts[i];
    ctx.r3.u64 = ready ? 0 : 1;
    return;
  }
  if (out) {
    u32 value = 0;
    if (type == kQueryTypeEvent)
      value = 1;
    else if (type == kQueryTypeOcclusion)
      value = u32(REXCVAR_GET(pgr4_debug_occlusion_samples));
    else
      PGR4_WARN_FIRST(4, "D3DQuery_GetData: query type {} answered with zeros", type);
    for (u32 i = 0; i < dwords; ++i)
      out[i] = value;
  }
  ctx.r3.u64 = 0;
}
