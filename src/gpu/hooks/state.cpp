// pgr4 - ReXGlue Recompiled Project
//
// Render target, depth and vertex declaration hooks. Like the shader hooks
// these wrap the guest originals: the guest's D3D keeps its own surfaces and
// declarations, the host keeps a mirror keyed by the guest object, and the
// binding state follows reblue's gpu/hooks/state.cpp (Copyright (c) 2026 Tom
// Clay, BSD 3-Clause).
//
// A host surface is keyed by the guest surface and its header's placement
// (RB_SURFACE_INFO, the EDRAM info, size and format), not the address alone:
// PGR4 rewrites its two main-scene surfaces in place every frame, so each is
// the colour target one frame and the depth the next
// (docs/native-renderer-plan.md, Phase 4).

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/settings.h" // kCvarGroup
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/output.h"
#include "gpu/resources.h"
#include "gpu/shaders/shader_constants.h"
#include "gpu/surface_registry.h"
#include "gpu/vertex_declaration.h"

REXCVAR_DEFINE_BOOL(pgr4_msaa, true, kCvarGroup,
                    "Render the guest's multisampled targets with the sample "
                    "count it asks for (PGR4: the race scene 2x, the "
                    "environment cube and quarter-resolution view 4x)");

REX_EXTERN(__imp__D3DDevice_SetRenderTarget);
REX_EXTERN(__imp__D3DDevice_SetDepthStencilSurface);
REX_EXTERN(__imp__D3DDevice_CreateVertexDeclaration);
REX_EXTERN(__imp__D3DDevice_SetVertexDeclaration);
REX_EXTERN(__imp__D3DSurface_GetDesc);
REX_EXTERN(__imp__D3DDevice_SetFVF);

namespace pgr4::gpu {
void NoteDefaultSurfaces(u32 back_buffer, u32 depth);
bool ForgetGuestSurface(u32 surface_va);
bool TraceFrameActive();
void SetTiledExtent(PPCContext &ctx, u8 *base, u32 device_va, u32 width, u32 height);
}

namespace {

using pgr4::gpu::GuestTexture;
using pgr4::gpu::GuestVertexDeclaration;

// D3D__InitializeStateTables' own SetFVF call returns here (0x826DA6E4 + 4).
constexpr u32 kInitializeStateTablesSetFVFReturn = 0x826DA6E8;

// One host surface per guest surface and header placement it has had.
struct SurfaceEntry {
  u64 placement = 0;
  GuestTexture *host = nullptr;
};

std::mutex g_mutex;
std::unordered_map<u32, std::vector<SurfaceEntry>> g_surfaces;
std::unordered_map<u32, GuestVertexDeclaration *> g_decls;
u32 g_default_back_buffer = 0;
std::atomic<u32> g_bound_render_target{0};
// Between BeginTiling and EndTiling, the tiles' extent (width << 32 | height):
// the guest surfaces are one tile's size, the host ones the whole extent.
std::atomic<u64> g_tiled_extent{0};

using SurfaceDesc = pgr4::gpu::D3DSurfaceDesc;

// Runs the guest's own GetDesc on an isolated frame, with the descriptor on
// the guest stack below the caller's live frames.
bool GuestSurfaceDesc(PPCContext &ctx, u8 *base, u32 surface,
                      SurfaceDesc &out) {
  rex::CallFrame frame(ctx);
  const u32 desc_va = (ctx.r1.u32 - 0x100) & ~0xFu;
  frame.ctx.r1.u64 = desc_va - 0x80;
  frame.ctx.r3.u64 = surface;
  frame.ctx.r4.u64 = desc_va;
  __imp__D3DSurface_GetDesc(frame.ctx, base);
  const auto *desc = pgr4::mem::at<const SurfaceDesc>(desc_va);
  if (!desc)
    return false;
  out = *desc;
  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    PGR4_INFO("D3DSurface_GetDesc(0x{:08X}): format=0x{:08X} type={} {}x{} "
             "msaa={}",
             surface, u32(out.Format), u32(out.Type), u32(out.Width),
             u32(out.Height), u32(out.MultiSampleType));
  }
  return out.Width != 0 && out.Height != 0;
}

// D3DMULTISAMPLE_TYPE (RB_SURFACE_INFO's MSAA_SAMPLES): 0 none, 1 2x, 2 4x.
// Colour and depth bound together carry the same type, so they pair.
u32 SurfaceSampleCount(const SurfaceDesc &desc) {
  if (!REXCVAR_GET(pgr4_msaa))
    return 1;
  const u32 samples = u32(desc.MultiSampleType) == 2   ? 4
                      : u32(desc.MultiSampleType) == 1 ? 2
                                                       : 1;
  if (samples == 1)
    return 1;
  auto *device = pgr4::gpu::Video::HostDevice();
  const plume::RenderFormat format = pgr4::gpu::ConvertGuestFormat(desc.Format);
  if (!device || format == plume::RenderFormat::UNKNOWN ||
      !(device->getSampleCountsSupported(format) &
        static_cast<plume::RenderSampleCounts>(samples))) {
    PGR4_WARN_FIRST(4, "{}x MSAA unsupported for guest format 0x{:08X}, "
                   "rendering it at 1x", samples, u32(desc.Format));
    return 1;
  }
  return samples;
}

// The header fields that place a surface: RB_SURFACE_INFO, the EDRAM info,
// size and format (gpu/d3d.h).
u64 Placement(u32 surface) {
  const auto *h = pgr4::mem::at<const pgr4::gpu::D3DSurface>(surface);
  if (!h)
    return 0;
  const u64 a = u64(u32(h->SurfaceInfo)) << 32 | u32(h->EdramInfo);
  const u64 b = u64(u32(h->Size)) << 32 | u32(h->Format);
  return a ^ (b * 0x9E3779B97F4A7C15ull);
}

GuestTexture *FindSurfaceLocked(u32 surface, u64 placement) {
  if (auto it = g_surfaces.find(surface); it != g_surfaces.end())
    for (const SurfaceEntry &e : it->second)
      if (e.placement == placement)
        return e.host;
  return nullptr;
}

// The host surface the guest surface's header currently describes.
GuestTexture *CurrentHostSurfaceLocked(u32 surface) {
  return FindSurfaceLocked(surface, Placement(surface));
}

GuestTexture *HostSurface(PPCContext &ctx, u8 *base, u32 surface) {
  if (!surface)
    return nullptr;
  if (surface == g_default_back_buffer)
    return pgr4::gpu::state().back_buffer_surface;
  const u64 extent = g_tiled_extent.load(std::memory_order_relaxed);
  // A tiled surface is a host surface of its own, apart from its untiled one.
  const u64 placement =
      extent ? Placement(surface) ^ (extent * 0xC2B2AE3D27D4EB4Full + 1) : Placement(surface);
  {
    std::lock_guard lock(g_mutex);
    if (GuestTexture *host = FindSurfaceLocked(surface, placement))
      return host;
  }
  SurfaceDesc desc{};
  if (!GuestSurfaceDesc(ctx, base, surface, desc))
    return nullptr;
  if (extent) {
    desc.Width = std::max(u32(desc.Width), u32(extent >> 32));
    desc.Height = std::max(u32(desc.Height), u32(extent));
  }
  // The host surface is the guest size times its scale.
  u32 scale =
      pgr4::gpu::Output::SurfaceScale(u32(desc.Width), u32(desc.Height));
  // Car shadow surfaces render finer than the scene (pgr4_shadow_scale).
  if (pgr4::gpu::IsDepthFormat(pgr4::gpu::ConvertGuestFormat(desc.Format)) &&
      pgr4::gpu::Output::IsShadowSurface(u32(desc.Width), u32(desc.Height),
                                         u32(desc.MultiSampleType)))
    scale *= pgr4::gpu::Output::ShadowScale();
  GuestTexture *host = pgr4::gpu::SurfaceRegistry::Get(
      u32(desc.Width) * scale, u32(desc.Height) * scale, desc.Format,
      SurfaceSampleCount(desc));
  if (host)
    host->hostScale = scale;
  std::lock_guard lock(g_mutex);
  auto &entries = g_surfaces[surface];
  if (!entries.empty())
    PGR4_DEV_INFO("surface 0x{:08X} rewritten in place: host surface {} for it ({}x{} fmt=0x{:08X})",
             surface, entries.size() + 1, u32(desc.Width), u32(desc.Height),
             u32(desc.Format));
  entries.push_back({placement, host});
  return host;
}

} // namespace

namespace pgr4::gpu {

// The device hook creates the default back buffer through the guest and
// names it here, so binding it targets the host back buffer Present shows.
void NoteDefaultSurfaces(u32 back_buffer, u32 /*depth*/) {
  g_default_back_buffer = back_buffer;
}

u32 BoundRenderTargetVa() {
  return g_bound_render_target.load(std::memory_order_relaxed);
}

// The host gives each surface its own texture, but on Xenos surfaces share
// EDRAM, and a clear writes the tiles of every surface placed there. That is
// modelled only for a colour clear to zero, which reads as zero in any
// format, and only for surfaces that lie wholly inside the cleared tiles.
// PGR3's title screen needs it: the race scene clears its target to the sky
// colour, the Bink planes then render into surfaces at the same EDRAM base,
// each cleared to black first, and the menu blends the movie over what that
// leaves, which on Xenos is black.
void ClearEdramAliases(u32 cleared_va) {
  const auto *cleared = pgr4::mem::at<const D3DSurface>(cleared_va);
  if (!cleared_va || !cleared ||
      (u32(cleared->resource.Common) & kCommonLevelSurface))
    return;
  const u32 base = u32(cleared->EdramInfo) & 0xFFFu;
  const u32 end = base + u32(cleared->EdramSize) / kEdramTileBytes;
  std::vector<GuestTexture *> aliases;
  {
    std::lock_guard lock(g_mutex);
    const GuestTexture *cleared_host = CurrentHostSurfaceLocked(cleared_va);
    for (const auto &[va, entries] : g_surfaces) {
      GuestTexture *host = va == cleared_va ? nullptr : CurrentHostSurfaceLocked(va);
      if (!host || host == cleared_host || IsDepthFormat(host->format))
        continue;
      const auto *surface = pgr4::mem::at<const D3DSurface>(va);
      if (!surface || (u32(surface->resource.Common) & kCommonLevelSurface))
        continue;
      const u32 first = u32(surface->EdramInfo) & 0xFFFu;
      const u32 last = first + u32(surface->EdramSize) / kEdramTileBytes;
      if (last > first && first >= base && last <= end)
        aliases.push_back(host);
    }
  }
  for (GuestTexture *host : aliases)
    Video::ClearTexture(host, 0);
}

// D3DResource_Destroy on a surface. The host surface goes back to the
// registry, which reissues a free one to the next surface of the same size and
// format and retires it after it idles, so a surface created later at the
// same address is mapped afresh.
bool ForgetGuestSurface(u32 surface_va) {
  std::vector<SurfaceEntry> entries;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_surfaces.find(surface_va);
    if (it == g_surfaces.end())
      return false;
    entries = std::move(it->second);
    g_surfaces.erase(it);
  }
  for (const SurfaceEntry &e : entries)
    if (e.host && e.host != state().back_buffer_surface)
      e.host->held = false;
  return true;
}

// D3DResource_Destroy on a vertex declaration (a resource in 6132).
bool ForgetGuestVertexDeclaration(u32 decl_va) {
  std::lock_guard lock(g_mutex);
  return g_decls.erase(decl_va) != 0;
}

} // namespace pgr4::gpu

namespace {
// Xenos depth shares the colour surface's EDRAM pitch, so a depth surface
// smaller than its colour target still covers it: RR6 pairs its 1280x720
// depth with the 1440x800 race target. D3D12 clips rendering to the smallest
// attachment, so the host depth texture grows to cover whatever colour target
// it is bound with (once; it keeps the larger size). Called outside the state
// lock, which ResizeTexture takes.
void GrowDepthToCoverTarget() {
  auto &s = pgr4::gpu::state();
  GuestTexture *rt = s.render_target;
  GuestTexture *ds = s.depth_stencil;
  if (!rt || !ds || !ds->texture || rt->sampleCount != ds->sampleCount)
    return;
  if (ds->width >= rt->width && ds->height >= rt->height)
    return;
  const u32 w = std::max(ds->width, rt->width);
  const u32 h = std::max(ds->height, rt->height);
  PGR4_INFO("depth surface grows {}x{} -> {}x{} to cover its render target",
           ds->width, ds->height, w, h);
  pgr4::gpu::Video::ResizeTexture(ds, w, h);
  s.draw_framebuffer_bound = false;
}

void BindRenderTarget(PPCContext &ctx, u8 *base, u32 device_va, u32 surface_va);
void BindDepthStencil(PPCContext &ctx, u8 *base, u32 surface_va);
} // namespace

// (device, index, surface)
REX_HOOK_RAW(D3DDevice_SetRenderTarget) {
  const u32 device_va = ctx.r3.u32;
  const u32 index = ctx.r4.u32;
  const u32 surface_va = ctx.r5.u32;
  __imp__D3DDevice_SetRenderTarget(ctx, base);
  // MRT: slot 1 is modelled (PGR3's night bloom wrote its compressed glow
  // there; PGR4's race frames only ever set it to null), slots 2-3 are not.
  if (index == 1) {
    GuestTexture *surface = HostSurface(ctx, base, surface_va);
    if (surface && pgr4::gpu::IsDepthFormat(surface->format))
      surface = nullptr;
    auto &s = pgr4::gpu::state();
    if (s.render_target1 != surface)
      s.draw_framebuffer_bound = false;
    pgr4::gpu::Video::SetDirtyValue<GuestTexture *>(
        s.dirtyStates.renderTargetAndDepthStencil, s.render_target1, surface);
    pgr4::gpu::Video::SetDirtyValue<plume::RenderFormat>(
        s.dirtyStates.pipelineState, s.pipelineState.renderTargetFormat1,
        surface ? surface->format : plume::RenderFormat::UNKNOWN);
    return;
  }
  if (index != 0) {
    if (surface_va)
      PGR4_WARN_FIRST(4, "SetRenderTarget: MRT slot {} (0x{:08X}) not modeled",
                     index, surface_va);
    return;
  }
  BindRenderTarget(ctx, base, device_va, surface_va);
}

// (device, surface)
REX_HOOK_RAW(D3DDevice_SetDepthStencilSurface) {
  const u32 surface_va = ctx.r4.u32;
  __imp__D3DDevice_SetDepthStencilSurface(ctx, base);
  BindDepthStencil(ctx, base, surface_va);
}

namespace pgr4::gpu {
// Predicated tiling (gpu/hooks/draw.cpp): a non-zero extent rebinds the bound
// colour target and depth at that size, zero rebinds them at their own.
void SetTiledExtent(PPCContext &ctx, u8 *base, u32 device_va, u32 width, u32 height) {
  const auto *dev = pgr4::mem::at<const D3DDevice>(device_va);
  if (!device_va || !dev)
    return;
  const bool tiling = width && height;
  g_tiled_extent.store(tiling ? u64(width) << 32 | height : 0,
                       std::memory_order_relaxed);
  // The larger of the pair binds second, so GrowDepthToCoverTarget never
  // sees a tiled target over an untiled depth.
  if (tiling) {
    BindDepthStencil(ctx, base, dev->depthStencil);
    BindRenderTarget(ctx, base, device_va, dev->renderTargets[0]);
  } else {
    BindRenderTarget(ctx, base, device_va, dev->renderTargets[0]);
    BindDepthStencil(ctx, base, dev->depthStencil);
  }
}
} // namespace pgr4::gpu

namespace {
void BindRenderTarget(PPCContext &ctx, u8 *base, u32 device_va, u32 surface_va) {
  g_bound_render_target.store(surface_va, std::memory_order_relaxed);
  GuestTexture *surface = HostSurface(ctx, base, surface_va);
  if (pgr4::gpu::TraceFrameActive()) {
    SurfaceDesc desc{};
    if (surface_va)
      GuestSurfaceDesc(ctx, base, surface_va, desc);
    PGR4_INFO("[trace] SetRenderTarget 0x{:08X} -> {}x{} guest fmt=0x{:08X} "
             "host fmt={} samples={} lr=0x{:08X}",
             surface_va, surface ? surface->width : 0,
             surface ? surface->height : 0, u32(desc.Format),
             surface ? static_cast<int>(surface->format) : -1,
             surface ? u32(surface->sampleCount) : 0, u32(ctx.lr));
  }
  auto &s = pgr4::gpu::state();
  if (s.render_target != surface)
    s.draw_framebuffer_bound = false;
  pgr4::gpu::Video::SetDirtyValue<GuestTexture *>(
      s.dirtyStates.renderTargetAndDepthStencil, s.render_target, surface);
  pgr4::gpu::Video::SetDirtyValue<plume::RenderFormat>(
      s.dirtyStates.pipelineState, s.pipelineState.renderTargetFormat,
      surface ? surface->format : plume::RenderFormat::UNKNOWN);
  pgr4::gpu::Video::SetDirtyValue<plume::RenderSampleCounts>(
      s.dirtyStates.pipelineState, s.pipelineState.sampleCount,
      surface ? surface->sampleCount : plume::RenderSampleCount::COUNT_1);
  pgr4::gpu::Video::SetDefaultViewport(
      pgr4::mem::at<pgr4::gpu::D3DDevice>(device_va), surface);
  GrowDepthToCoverTarget();
}

void BindDepthStencil(PPCContext &ctx, u8 *base, u32 surface_va) {
  GuestTexture *surface = HostSurface(ctx, base, surface_va);
  if (pgr4::gpu::TraceFrameActive()) {
    // Words 6-8 carry RB_SURFACE_INFO, the EDRAM base and the HiZ info
    // (gpu/d3d.h D3DSurface).
    std::string words;
    if (const auto *w = pgr4::mem::at<const be_u32>(surface_va); w && surface_va)
      for (int i = 6; i < 9; ++i)
        words += std::format(" {:08X}", u32(w[i]));
    PGR4_INFO("[trace] SetDepthStencilSurface 0x{:08X} -> {}x{} host={}:{} lr=0x{:08X}",
             surface_va, surface ? surface->width : 0,
             surface ? surface->height : 0, static_cast<const void *>(surface),
             words, u32(ctx.lr));
  }
  if (surface && !pgr4::gpu::IsDepthFormat(surface->format))
    surface = nullptr;
  auto &s = pgr4::gpu::state();
  if (s.depth_stencil != surface)
    s.draw_framebuffer_bound = false;
  pgr4::gpu::Video::SetDirtyValue<GuestTexture *>(
      s.dirtyStates.renderTargetAndDepthStencil, s.depth_stencil, surface);
  pgr4::gpu::Video::SetDirtyValue<plume::RenderFormat>(
      s.dirtyStates.pipelineState, s.pipelineState.depthStencilFormat,
      surface ? surface->format : plume::RenderFormat::UNKNOWN);
  if (surface)
    s.scene_depth = surface;
  // No viewport reset: on Xenos D3D binding a depth surface leaves the
  // viewport the render target set (RR6's race depth was smaller than its
  // 1440x800 colour target).
  GrowDepthToCoverTarget();
}
} // namespace

// (pVertexElements) -> D3DVertexDeclaration*
REX_HOOK_RAW(D3DDevice_CreateVertexDeclaration) {
  const u32 elements_va = ctx.r3.u32;
  __imp__D3DDevice_CreateVertexDeclaration(ctx, base);
  const u32 decl_va = ctx.r3.u32;
  auto *elements = pgr4::mem::at<pgr4::gpu::GuestVertexElement>(elements_va);
  if (!decl_va || !elements)
    return;
  if (auto *decl = pgr4::gpu::CreateVertexDeclaration(elements)) {
    std::lock_guard lock(g_mutex);
    g_decls[decl_va] = decl;
  }
}

// (device, declaration)
REX_HOOK_RAW(D3DDevice_SetVertexDeclaration) {
  const u32 decl_va = ctx.r4.u32;
  __imp__D3DDevice_SetVertexDeclaration(ctx, base);
  GuestVertexDeclaration *decl = nullptr;
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_decls.find(decl_va); it != g_decls.end())
      decl = it->second;
  }
  // The recompiled VS guards its packed normal decodes on these bits.
  auto &s = pgr4::gpu::state();
  u32 spec = s.pipelineState.specConstants &
             ~(pgr4::gpu::kSpecConstantR11G11B10Normal | pgr4::gpu::kSpecConstantDec3NNormal);
  if (decl && decl->hasR11G11B10Normal)
    spec |= pgr4::gpu::kSpecConstantR11G11B10Normal;
  if (decl && decl->hasDec3NNormal)
    spec |= pgr4::gpu::kSpecConstantDec3NNormal;
  pgr4::gpu::Video::SetDirtyValue<u32>(s.dirtyStates.pipelineState,
                                      s.pipelineState.specConstants, spec);
  pgr4::gpu::Video::SetVertexDeclaration(decl);
}

// (device, FVF). It builds a declaration inside the device (+0x2F98) and
// binds it without D3DDevice_SetVertexDeclaration, so the host does not see
// it. D3D__InitializeStateTables calls it at device creation; warn if
// anything else does (mercury's sub_8285B278 is the only other caller).
REX_HOOK_RAW(D3DDevice_SetFVF) {
  if (u32(ctx.lr) != kInitializeStateTablesSetFVFReturn)
    PGR4_WARN_FIRST(4, "D3DDevice_SetFVF 0x{:X} from 0x{:08X}: FVF declarations "
                       "are not modelled",
                    ctx.r4.u32, u32(ctx.lr));
  __imp__D3DDevice_SetFVF(ctx, base);
}
