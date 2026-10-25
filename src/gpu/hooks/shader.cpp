// pgr4 - ReXGlue Recompiled Project
//
// Shader hooks for the native renderer. The guest still builds its own shader
// objects: its unhooked D3D code (the GPR allocation, the literal constant
// loads) reads them through device+0x3190 / +0x318C as real XDK objects.
// Alongside each one the host builds a GuestShader from the XenosRecomp cache,
// keyed by the guest object's address, and Set*Shader tells Video which host
// shader is bound.
//
// PGR4 creates shaders two ways (docs/native-renderer-plan.md, Phases 1 and
// 4). The game's own creators (sub_822B34A0 VS, sub_822B35F0 PS) and, by
// default, mercury's loaders go through XG: XGGetMicrocodeShaderParts fills a
// parts record whose first word is the container, XGSet*ShaderHeader(header,
// size, parts) builds the object, and XGRegister*Shader(header, physical)
// finishes it; the header is what Set*Shader later receives. The header hook
// notes the container, and the register hook builds the host shader. With
// bit 2 of their flags set, mercury's loaders call
// D3DDevice_Create{Vertex,Pixel}Shader instead, which return the object.
// 6132 shaders are D3DResources, so D3DResource_Destroy (hooks/resource.cpp)
// forgets a freed one.

#include <atomic>
#include <mutex>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device.h"
#include "gpu/resources.h"

REX_EXTERN(__imp__D3DDevice_CreateVertexShader);
REX_EXTERN(__imp__D3DDevice_CreatePixelShader);
REX_EXTERN(__imp__XGSetVertexShaderHeader);
REX_EXTERN(__imp__XGSetPixelShaderHeader);
REX_EXTERN(__imp__XGRegisterVertexShader);
REX_EXTERN(__imp__XGRegisterPixelShader);
REX_EXTERN(__imp__D3DDevice_SetVertexShader);
REX_EXTERN(__imp__D3DDevice_SetPixelShader);

namespace {

using pgr4::gpu::GuestShader;
using pgr4::gpu::ResourceType;

std::mutex g_mutex;
std::unordered_map<u32, GuestShader *> g_by_guest; // guest object VA -> host
// XG headers built but not yet registered: header VA -> container VA.
std::unordered_map<u32, u32> g_pending_headers;

struct Stats {
  std::atomic<u32> created{0};
  std::atomic<u32> cache_hits{0};
  std::atomic<u32> host_built{0};
};
Stats g_vs, g_ps;

// PGR4 creates almost all of its 990 shaders in the front end, and the
// stack-built ones as their screens need them (docs/native-renderer-plan.md,
// Phase 1), so the summary comes every few dozen creations; CreateShader warns
// on any cache miss itself.
constexpr u32 kSummaryInterval = 50;

void LogStageSummary(const char *stage, const Stats &st) {
  PGR4_DEBUG("{} shaders: {} created, {} recompiled cache hits, {} host shaders "
           "built",
           stage, st.created.load(), st.cache_hits.load(),
           st.host_built.load());
}

void TrackCreated(u32 guest_obj, u32 container_va, ResourceType type) {
  auto &st = type == ResourceType::VertexShader ? g_vs : g_ps;
  const auto *container = pgr4::mem::at<const be_u32>(container_va);
  GuestShader *gs = nullptr;
  if (container)
    gs = pgr4::gpu::CreateShader(container, type);
  if (gs) {
    if (gs->shaderCacheEntry)
      st.cache_hits.fetch_add(1, std::memory_order_relaxed);
    // Building the host shader here proves the cached blob loads on this
    // device at boot rather than at the first draw that needs it.
    if (pgr4::gpu::GetOrLinkShader(gs, 0))
      st.host_built.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(g_mutex);
    g_by_guest[guest_obj] = gs;
  }
  if ((st.created.fetch_add(1, std::memory_order_relaxed) + 1) % kSummaryInterval == 0)
    LogStageSummary(type == ResourceType::VertexShader ? "VS" : "PS", st);
}

GuestShader *Lookup(u32 guest_obj) {
  if (!guest_obj)
    return nullptr;
  std::lock_guard lock(g_mutex);
  auto it = g_by_guest.find(guest_obj);
  return it != g_by_guest.end() ? it->second : nullptr;
}

// XGSet*ShaderHeader(header, size, parts): parts[0] is the container.
void NoteHeader(u32 header, u32 parts) {
  const auto *record = pgr4::mem::at<const be_u32>(parts);
  if (!header || !record || !u32(record[0]))
    return;
  std::lock_guard lock(g_mutex);
  g_pending_headers[header] = u32(record[0]);
}

// XGRegister*Shader(header, physical): the object is complete.
void Registered(u32 header, ResourceType type) {
  u32 container = 0;
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_pending_headers.find(header); it != g_pending_headers.end()) {
      container = it->second;
      g_pending_headers.erase(it);
    }
  }
  if (!container) {
    PGR4_WARN_FIRST(8, "XGRegister{}Shader 0x{:08X}: no header seen for it",
                    type == ResourceType::VertexShader ? "Vertex" : "Pixel", header);
    return;
  }
  TrackCreated(header, container, type);
}

} // namespace

namespace pgr4::gpu {

// D3DResource_Destroy on a vertex or pixel shader (hooks/resource.cpp).
bool ForgetGuestShader(u32 shader_va) {
  std::lock_guard lock(g_mutex);
  g_pending_headers.erase(shader_va);
  return g_by_guest.erase(shader_va) != 0;
}

} // namespace pgr4::gpu

REX_HOOK_RAW(XGSetVertexShaderHeader) {
  NoteHeader(ctx.r3.u32, ctx.r5.u32);
  __imp__XGSetVertexShaderHeader(ctx, base);
}

REX_HOOK_RAW(XGSetPixelShaderHeader) {
  NoteHeader(ctx.r3.u32, ctx.r5.u32);
  __imp__XGSetPixelShaderHeader(ctx, base);
}

REX_HOOK_RAW(XGRegisterVertexShader) {
  const u32 header = ctx.r3.u32;
  __imp__XGRegisterVertexShader(ctx, base);
  Registered(header, ResourceType::VertexShader);
}

REX_HOOK_RAW(XGRegisterPixelShader) {
  const u32 header = ctx.r3.u32;
  __imp__XGRegisterPixelShader(ctx, base);
  Registered(header, ResourceType::PixelShader);
}

// (pFunction) -> D3DVertexShader*. Only mercury's loaders call these.
REX_HOOK_RAW(D3DDevice_CreateVertexShader) {
  const u32 container = ctx.r3.u32;
  __imp__D3DDevice_CreateVertexShader(ctx, base);
  if (ctx.r3.u32)
    TrackCreated(ctx.r3.u32, container, ResourceType::VertexShader);
}

REX_HOOK_RAW(D3DDevice_CreatePixelShader) {
  const u32 container = ctx.r3.u32;
  __imp__D3DDevice_CreatePixelShader(ctx, base);
  if (ctx.r3.u32)
    TrackCreated(ctx.r3.u32, container, ResourceType::PixelShader);
}

// (device, shader). The guest body still runs so the device keeps its own
// view (and its literal constant copies) consistent.
REX_HOOK_RAW(D3DDevice_SetVertexShader) {
  pgr4::gpu::Video::SetVertexShader(Lookup(ctx.r4.u32));
  __imp__D3DDevice_SetVertexShader(ctx, base);
}

REX_HOOK_RAW(D3DDevice_SetPixelShader) {
  pgr4::gpu::Video::SetPixelShader(Lookup(ctx.r4.u32));
  __imp__D3DDevice_SetPixelShader(ctx, base);
}
