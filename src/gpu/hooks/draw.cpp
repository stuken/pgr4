// pgr4 - ReXGlue Recompiled Project
//
// Draw hooks for the native renderer. The draw entry points are replaced (the
// guest bodies only build PM4 for the discard ring), while BeginVertices,
// EndVertices and DrawVerticesUP wrap the guest. XDK 2135's DrawVerticesUP
// calls BeginVertices but closes the packet inline rather than through
// EndVertices, so both ends record the pending immediate-mode draw. Vertex and index buffers are still guest-owned, so each draw
// binds host mirrors of whatever the device has set (gpu/guest_buffers.h).
// The recording sequence follows reblue's gpu/hooks/draw.cpp (Copyright (c)
// 2026 Tom Clay, BSD 3-Clause).

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/hook.h>
#include <rex/system/thread_state.h>
#include <rex/types.h>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "core/settings.h"
#include "gpu/shaders/shader_cache.h"
#include "gpu/constant_buffers.h"
#include "gpu/d3d.h"
#include "gpu/format.h"
#include "gpu/frame_dump.h"
#include "gpu/device.h"
#include "gpu/guest_buffers.h"
#include "gpu/native_texture_mirror.h"
#include "gpu/output.h"
#include "gpu/resources.h"
#include "gpu/perf_stats.h"

REXCVAR_DEFINE_INT32(pgr4_trace_frame, 0, kCvarGroup,
                     "Log every draw and resolve of present frame N (0 = off)");
REXCVAR_DEFINE_INT32(pgr4_trace_frame_count, 1, kCvarGroup,
                     "How many frames pgr4_trace_frame traces, from N on");
REXCVAR_DEFINE_STRING(pgr4_trace_vs, "", kCvarGroup,
                      "In the pgr4_trace_frame frames, trace only the draws of the "
                      "VS with these hashes (hex, comma-separated), with their "
                      "constants and first stream records");
REXCVAR_DEFINE_STRING(pgr4_debug_fill_depth_resolve, "", kCvarGroup,
                      "Bring-up experiment: fill each depth resolve's guest "
                      "texture memory with this 32-bit value (hex), as if the "
                      "GPU had written it back");
REXCVAR_DEFINE_INT32(pgr4_trace_vs_records, 8, kCvarGroup,
                     "How many vertices pgr4_trace_vs dumps per draw: the first N "
                     "indices' records of streams 0 and 1");
REXCVAR_DEFINE_INT32(pgr4_trace_vs_draws, 8, kCvarGroup,
                     "How many draws per frame pgr4_trace_vs dumps inputs for");
REXCVAR_DEFINE_STRING(pgr4_debug_skip_vs, "", kCvarGroup,
                      "Bring-up experiment: skip draws whose vertex shader hash "
                      "(hex, as pgr4_trace_frame prints it) is in this comma list");

REX_EXTERN(__imp__D3DDevice_BeginVertices);
REX_EXTERN(__imp__D3DDevice_SetViewport);
REX_EXTERN(__imp__D3DDevice_EndVertices);
REX_EXTERN(__imp__D3DDevice_DrawVerticesUP);

namespace pgr4::gpu {
u32 PresentCount();
u32 BoundRenderTargetVa();
void SetTiledExtent(PPCContext &ctx, u8 *base, u32 device_va, u32 width, u32 height);
}

namespace {

namespace xe = rex::graphics::xenos;
using pgr4::gpu::D3DDevice;
using pgr4::gpu::GuestBuffer;
using pgr4::gpu::ResourceType;
using pgr4::gpu::Video;

using pgr4::gpu::D3DIndexBuffer;
using pgr4::gpu::D3DVertexBuffer;

// Draw counters, reported by LogDrawStats from the Present hook.
struct DrawStats {
  std::atomic<u32> issued{0};
  std::atomic<u32> recorded{0};
  std::atomic<u32> no_framebuffer{0};
  std::atomic<u32> no_state{0};
  std::atomic<u32> no_index_buffer{0};
  std::atomic<u32> textures_resolved{0};
  std::atomic<u32> textures_unresolved{0};
  std::atomic<u32> resolves{0};
  std::mutex rt_mutex;
  std::unordered_map<u64, u32> draws_per_rt; // (w << 32 | h) -> draws
};
DrawStats g_stats;

struct DrawArgs {
  bool indexed = false;
  u32 vertexOrIndexCount = 0;
  i32 baseVertexIndex = 0;
  u32 startIndex = 0;
  u32 startVertex = 0;
};

plume::RenderPrimitiveTopology MapPrimitiveType(u32 prim) {
  switch (static_cast<xe::PrimitiveType>(prim)) {
  // Points are one guest pixel: no point sprite sizes, and DispatchDraw covers
  // the host block at a resolution scale. Lines are one host pixel wide,
  // unscaled; RR6 draws none.
  case xe::PrimitiveType::kPointList:
    return plume::RenderPrimitiveTopology::POINT_LIST;
  case xe::PrimitiveType::kLineList:
    PGR4_WARN_FIRST(1, "MapPrimitiveType: line list drawn one pixel wide");
    return plume::RenderPrimitiveTopology::LINE_LIST;
  case xe::PrimitiveType::kLineStrip:
    PGR4_WARN_FIRST(1, "MapPrimitiveType: line strip drawn one pixel wide");
    return plume::RenderPrimitiveTopology::LINE_STRIP;
  case xe::PrimitiveType::kTriangleList:
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  case xe::PrimitiveType::kTriangleFan:
    // Expanded through Video::FanExpansionIBView.
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  case xe::PrimitiveType::kTriangleStrip:
    return plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
  case xe::PrimitiveType::kQuadList:
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  default: {
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 4)
      PGR4_WARN("MapPrimitiveType: unmapped primitive type {} drawn as a "
               "triangle list",
               prim);
    return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
  }
}

// Pixel samplers 0..15 and vertex samplers 0..3 (slots 16..19):
// device.textures[s] pairs with fetch constant s.
// Most draws bind what the draw before them did in most slots. A slot whose
// texture, fetch constant, frame and mirror generation all match the last
// draw's still holds the right binding, so it skips the lookup and the bind.
void BindGuestTextures(const D3DDevice &dev) {
  struct Slot {
    u32 va = 0;
    u32 fetch[6] = {};
  };
  thread_local Slot t_last[pgr4::gpu::kHostSamplerSlots];
  thread_local u64 t_generation = 0, t_frame = ~0ull;
  const u64 generation = pgr4::gpu::TextureMirrorGeneration();
  const u64 frame = pgr4::gpu::GuestMirrorFrame();
  const bool same_epoch = generation == t_generation && frame == t_frame;
  t_generation = generation;
  t_frame = frame;
  pgr4::gpu::GuestTexture *textures[pgr4::gpu::kHostSamplerSlots] = {};
  u32 resolved = 0, unresolved = 0;
  bool any = false;
  for (u32 i = 0; i < pgr4::gpu::kHostSamplerSlots; ++i) {
    const u32 tex_va = dev.textures[i];
    if (!tex_va)
      continue;
    u32 fetch[6];
    for (u32 k = 0; k < 6; ++k)
      fetch[k] = u32(dev.fetchConstants[i].dword[k]);
    Slot &last = t_last[i];
    if (same_epoch && last.va == tex_va &&
        std::memcmp(last.fetch, fetch, sizeof(fetch)) == 0) {
      ++resolved;
      continue;
    }
    auto *tex = pgr4::gpu::ResolveOrMirrorGuestTexture(tex_va);
    ++(tex ? resolved : unresolved);
    textures[i] = tex;
    any = true;
    // An unresolved texture leaves the slot as it was, so it is retried.
    last.va = tex ? tex_va : 0;
    std::memcpy(last.fetch, fetch, sizeof(fetch));
  }
  g_stats.textures_resolved.fetch_add(resolved, std::memory_order_relaxed);
  g_stats.textures_unresolved.fetch_add(unresolved, std::memory_order_relaxed);
  if (any)
    Video::SetTextures(textures);
}

void CountRenderTarget() {
  auto &s = pgr4::gpu::state();
  std::lock_guard rt_lock(g_stats.rt_mutex);
  const u64 key = s.render_target ? (u64(s.render_target->width) << 32 |
                                     s.render_target->height)
                                  : 0;
  ++g_stats.draws_per_rt[key];
}

// The guest return addresses above a hook: lr (if non-zero) first, then the
// back chain. Each frame's first word links to the caller's, and prologues
// save LR at -8 from the caller's stack pointer.
std::string GuestCallChain(u32 sp, u32 lr, u32 depth) {
  std::string chain;
  if (lr)
    chain += std::format(" {:08X}", lr);
  for (u32 i = 0; sp && i < depth; ++i) {
    // The outermost frame links past the top of the stack: checked reads.
    const auto *link = pgr4::mem::try_at<const be_u32>(sp);
    if (!link || !u32(*link))
      break;
    sp = u32(*link);
    if (const auto *saved = pgr4::mem::try_at<const be_u32>(sp - 8))
      chain += std::format(" {:08X}", u32(*saved));
  }
  return chain;
}

// A stream's offset into its vertex buffer. 6132 keeps no offset field:
// SetStreamSource adds it to the address in the stream's vertex fetch
// constant, which is physical, while the buffer header holds the virtual one.
u32 StreamOffset(const D3DDevice &dev, u32 stream, const D3DVertexBuffer &vb) {
  const u32 k = pgr4::gpu::kStreamVertexFetchBase - stream;
  const u32 fetch = u32(dev.fetchConstants[k / 3].dword[(k % 3) * 2]) & ~0x3u;
  const u32 base = pgr4::gpu::GpuAddress(u32(vb.Fetch0) & ~0x3u);
  return fetch >= base ? fetch - base : 0;
}

// pgr4_trace_frame=N logs every draw of present frame N.
bool TracingThisFrame() {
  const i32 wanted = REXCVAR_GET(pgr4_trace_frame);
  const i32 count = std::max(REXCVAR_GET(pgr4_trace_frame_count), 1);
  const i64 frame = i64(pgr4::gpu::PresentCount()) + 1;
  return wanted > 0 && frame >= wanted && frame < i64(wanted) + count;
}

void TraceDraw(const char *path, const D3DDevice &dev, u32 prim, u32 count) {
  auto &s = pgr4::gpu::state();
  auto hash = [](const pgr4::gpu::GuestShader *gs) -> u64 {
    return gs && gs->shaderCacheEntry ? gs->shaderCacheEntry->hash : 0;
  };
  std::string tex;
  for (u32 i = 0; i < pgr4::gpu::kSamplerCount; ++i) {
    const u32 va = dev.textures[i];
    if (!va)
      continue;
    const char *kind = pgr4::gpu::FindResolveTarget(va) ? "resolve" : "mirror";
    const auto &fc = dev.fetchConstants[i].dword;
    tex += std::format(" t{}=0x{:08X}({} fmt={} {}x{} swz={:03X} sign={:02X} exp={} dim={})",
                       i, va, kind, u32(fc[1]) & 0x3F,
                       (u32(fc[2]) & 0x1FFF) + 1,
                       ((u32(fc[2]) >> 13) & 0x1FFF) + 1,
                       (u32(fc[3]) >> 1) & 0xFFF, (u32(fc[0]) >> 2) & 0xFF,
                       static_cast<i32>(u32(fc[3]) << 13) >> 26,
                       (u32(fc[5]) >> 9) & 3);
  }
  // The bound declaration: stream:offset type usage/index per element.
  if (const auto *decl = s.vertex_declaration; decl && decl->vertexElements) {
    tex += " decl:";
    for (u32 i = 0; i + 1 < decl->vertexElementCount; ++i) {
      const auto &e = decl->vertexElements[i];
      tex += std::format(" {}:{}/{:06X}/{}{}", u32(e.stream), u32(e.offset), u32(e.type),
                         u32(e.usage), u32(e.usageIndex));
    }
  }
  // Stream 0's vertex buffer header against the vertex fetch constants in
  // the top texture slots (three 2-dword vertex fetches per 6-dword slot), to
  // see whether the game binds vertex data through SetStreamSource only.
  if (const u32 vb = dev.streams[0]) {
    if (const auto *h = pgr4::mem::at<const D3DVertexBuffer>(vb))
      tex += std::format(" s0=0x{:08X}{{f0={:08X} f1={:08X}}} off={}", vb, u32(h->Fetch0),
                         u32(h->Fetch1), StreamOffset(dev, 0, *h));
  }
  tex += " vfc:";
  for (u32 slot = 26; slot < 32; ++slot)
    for (u32 k = 0; k < 3; ++k) {
      const u32 w0 = u32(dev.fetchConstants[slot].dword[k * 2]);
      if ((w0 & 3u) == 3u)
        tex += std::format(" {}={:08X}/{:08X}", 95 - (slot * 3 + k) + 0, w0,
                           u32(dev.fetchConstants[slot].dword[k * 2 + 1]));
    }
  // The game code that issued the draw.
  if (auto *ts = rex::runtime::ThreadState::Get(); ts && ts->context()) {
    const PPCContext &ctx = *ts->context();
    tex += " chain:" + GuestCallChain(ctx.r1.u32, u32(ctx.lr), 6);
  }
  PGR4_INFO("[trace] {} prim={} n={} rt={}x{} vs={:016X} ps={:016X} "
           "blend={}:{:08X} depth={:08X} cull={:X} color={:X} cmask={:X} aref={:.3f} "
           "poly={:X}:{:.3f},{:.6f} vtx={:X} vte={:X} sc={:08X}:{:08X} ucp={:X}:{:.3f},{:.3f},{:.3f},{:.3f} vp={},{} {}x{} vso={:.1f},{:.1f},{:.1f},{:.1f},{:.3f},{:.3f}{}",
           path, prim, count, s.render_target ? s.render_target->width : 0,
           s.render_target ? s.render_target->height : 0,
           hash(s.vertex_shader), hash(s.pixel_shader),
           (u32(dev.blendFlags) >> 31), u32(dev.rbBlendControl0),
           u32(dev.rbDepthControl), u32(dev.paSuScModeCntl) & 7u,
           u32(dev.rbColorControl), u32(dev.rbColorMask), float(dev.rbAlphaRef),
           (u32(dev.paSuScModeCntl) >> 11) & 7u, float(dev.polyOffsetFrontScale),
           float(dev.polyOffsetFrontOffset), u32(dev.paSuVtxCntl), u32(dev.paClVteCntl),
           u32(dev.paScWindowScissorTL), u32(dev.paScWindowScissorBR),
           u32(dev.paClClipCntl) & 0x3Fu, float(dev.clipPlanes[0][0]),
           float(dev.clipPlanes[0][1]), float(dev.clipPlanes[0][2]),
           float(dev.clipPlanes[0][3]),
           float(dev.viewport.X), float(dev.viewport.Y), float(dev.viewport.Width),
           float(dev.viewport.Height), float(dev.viewportScaleOffset[0]),
           float(dev.viewportScaleOffset[1]), float(dev.viewportScaleOffset[2]),
           float(dev.viewportScaleOffset[3]), float(dev.viewportScaleOffset[4]),
           float(dev.viewportScaleOffset[5]), tex);
}

// The PS constants of a full-screen pass, where post grading and filter taps
// live.
void TracePixelConstants(const D3DDevice &dev) {
  // Every non-zero constant.
  std::string consts;
  for (u32 c = 0; c < 256; ++c) {
    const auto &v = dev.psFloatConstants[c];
    if (!float(v[0]) && !float(v[1]) && !float(v[2]) && !float(v[3]))
      continue;
    consts += std::format(" c{}=({:.6g},{:.6g},{:.6g},{:.6g})", c, float(v[0]),
                          float(v[1]), float(v[2]), float(v[3]));
  }
  PGR4_INFO("[trace]   ps consts{}", consts);
}

// Binds host mirrors of the device's vertex streams and index buffer.
void BindGuestGeometry(const D3DDevice &dev, bool indexed) {
  Video::VertexStream streams[16];
  for (u32 i = 0; i < 16; ++i) {
    const u32 vb_va = dev.streams[i];
    streams[i].stride = u32(dev.streamStrides[i]) * 4;
    const auto *vb = pgr4::mem::at<const D3DVertexBuffer>(vb_va);
    if (!vb_va || !vb)
      continue;
    const u32 base = u32(vb->Fetch0) & ~0x3u;
    const u32 size = u32(vb->Fetch1) & 0x03FFFFFCu;
    const u32 offset = StreamOffset(dev, i, *vb);
    GuestBuffer *mirror =
        pgr4::gpu::MirrorGuestBuffer(base, size, ResourceType::VertexBuffer, 4,
                                    plume::RenderFormat::UNKNOWN);
    if (!mirror || !mirror->hasBuffer() || offset >= size)
      continue;
    streams[i].buffer = mirror->bufferRef(offset);
    streams[i].size = size - offset;
  }
  Video::SetVertexStreams(streams);
  BindGuestTextures(dev);
  if (!indexed)
    return;
  const auto *ib = pgr4::mem::at<const D3DIndexBuffer>(dev.indexBuffer);
  GuestBuffer *mirror = nullptr;
  if (dev.indexBuffer && ib) {
    const bool index32 =
        (u32(ib->resource.Common) & pgr4::gpu::kCommonIndex32) != 0;
    mirror = pgr4::gpu::MirrorGuestBuffer(
        ib->Address, ib->Size, ResourceType::IndexBuffer, index32 ? 4 : 2,
        index32 ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
  }
  Video::SetIndices(mirror);
}

// Expands the guest's indexed quad list into a triangle list (each quad
// i0 i1 i2 i3 as i0 i1 i2, i0 i2 i3, the order QuadlistExpansionIBView uses)
// in the upload heap and draws it. Caller holds the state mutex.
bool DrawIndexedQuadList(const D3DDevice &dev, const DrawArgs &args, u32 quads) {
  const auto *ibh = pgr4::mem::at<const D3DIndexBuffer>(dev.indexBuffer);
  if (!quads || !dev.indexBuffer || !ibh)
    return false;
  const bool index32 =
      (u32(ibh->resource.Common) & pgr4::gpu::kCommonIndex32) != 0;
  const u32 index_size = index32 ? 4u : 2u;
  const u32 ib_base = u32(ibh->Address);
  const u32 ib_size = u32(ibh->Size);
  if (u64(args.startIndex + quads * 4) * index_size > ib_size)
    return false;
  const auto *src =
      pgr4::mem::at<const u8>(ib_base + args.startIndex * index_size);
  if (!src)
    return false;
  std::vector<u32> tris(size_t(quads) * 6);
  auto read = [&](u32 i) -> u32 {
    return index32 ? u32(reinterpret_cast<const be_u32 *>(src)[i])
                   : u32(u16(reinterpret_cast<const be_u16 *>(src)[i]));
  };
  for (u32 q = 0; q < quads; ++q) {
    const u32 i0 = read(q * 4), i1 = read(q * 4 + 1), i2 = read(q * 4 + 2),
              i3 = read(q * 4 + 3);
    u32 *o = tris.data() + size_t(q) * 6;
    o[0] = i0;
    o[1] = i1;
    o[2] = i2;
    o[3] = i0;
    o[4] = i2;
    o[5] = i3;
  }
  const u32 bytes = static_cast<u32>(tris.size() * sizeof(u32));
  auto alloc = pgr4::gpu::UploadHostBytes(tris.data(), bytes, 4);
  if (!alloc.memory)
    return false;
  auto &s = pgr4::gpu::state();
  const plume::RenderIndexBufferView view(alloc.ref, bytes,
                                          plume::RenderFormat::R32_UINT);
  s.command_list->setIndexBuffer(&view);
  s.command_list->drawIndexedInstanced(quads * 6, 1, 0, args.baseVertexIndex,
                                       0);
  return true;
}

// pgr4_debug_skip_vs names this draw's vertex shader.
bool SkippedVertexShader(const pgr4::gpu::GuestShader *vs) {
  const std::string &list = REXCVAR_GET(pgr4_debug_skip_vs);
  if (list.empty() || !vs || !vs->shaderCacheEntry)
    return false;
  const std::string hash = std::format("{:016X}", vs->shaderCacheEntry->hash);
  for (size_t pos = 0; pos < list.size();) {
    size_t end = list.find(',', pos);
    if (end == std::string::npos)
      end = list.size();
    if (list.compare(pos, end - pos, hash) == 0)
      return true;
    pos = end + 1;
  }
  return false;
}

// With pgr4_trace_vs set, the traced frames log only that shader's draws.
bool TracedVertexShader() {
  const std::string &wanted = REXCVAR_GET(pgr4_trace_vs);
  if (wanted.empty())
    return true;
  auto &s = pgr4::gpu::state();
  std::lock_guard lock(s.mutex);
  const auto *vs = s.vertex_shader;
  if (!vs || !vs->shaderCacheEntry)
    return false;
  // A comma-separated list of hashes.
  const std::string hash = std::format("{:016X}", vs->shaderCacheEntry->hash);
  for (size_t pos = 0; pos <= wanted.size();) {
    const size_t end = std::min(wanted.find(',', pos), wanted.size());
    if (wanted.compare(pos, end - pos, hash) == 0)
      return true;
    pos = end + 1;
  }
  return false;
}

// pgr4_trace_vs: every non-zero VS constant of the draw, and the records of
// streams 0 and 1 that its first indices reach, as raw dwords keyed by vertex
// index (index + base vertex, the r0.x the shader sees), to evaluate a shader
// by hand.
void TraceVertexShaderInputs(const D3DDevice &dev, const DrawArgs &args) {
  const std::string &wanted = REXCVAR_GET(pgr4_trace_vs);
  if (wanted.empty())
    return;
  if (!TracedVertexShader())
    return;
  static u32 s_frame = 0, s_draws = 0;
  if (s_frame != pgr4::gpu::PresentCount()) {
    s_frame = pgr4::gpu::PresentCount();
    s_draws = 0;
  }
  if (s_draws++ >= u32(std::max(REXCVAR_GET(pgr4_trace_vs_draws), 0)))
    return;
  std::string consts;
  for (u32 c = 0; c < 256; ++c) {
    const auto &v = dev.vsFloatConstants[c];
    const float f[4] = {float(v[0]), float(v[1]), float(v[2]), float(v[3])};
    if (f[0] == 0 && f[1] == 0 && f[2] == 0 && f[3] == 0)
      continue;
    consts += std::format(" c{}=({:.6g},{:.6g},{:.6g},{:.6g})", c, f[0], f[1],
                          f[2], f[3]);
  }
  PGR4_INFO("[trace]   vs-input indexed={} base={} start={} start_index={} "
            "consts{}",
            args.indexed, args.baseVertexIndex, args.startVertex,
            args.startIndex, consts);
  std::string ps_consts;
  for (u32 c = 0; c < 256; ++c) {
    const auto &v = dev.psFloatConstants[c];
    const float f[4] = {float(v[0]), float(v[1]), float(v[2]), float(v[3])};
    if (f[0] == 0 && f[1] == 0 && f[2] == 0 && f[3] == 0)
      continue;
    ps_consts += std::format(" c{}=({:.6g},{:.6g},{:.6g},{:.6g})", c, f[0], f[1],
                             f[2], f[3]);
  }
  std::string bools;
  for (u32 i = 0; i < 8; ++i)
    bools += std::format(" {:08X}", u32(dev.boolConstants[i]));
  PGR4_INFO("[trace]   ps-input bools{} consts{}", bools, ps_consts);
  // The vertices the first indices reach.
  const u32 count = std::min(u32(std::max(REXCVAR_GET(pgr4_trace_vs_records), 1)),
                             args.vertexOrIndexCount);
  std::vector<u32> vertices;
  if (const auto *ib = pgr4::mem::at<const D3DIndexBuffer>(dev.indexBuffer);
      args.indexed && dev.indexBuffer && ib) {
    const bool index32 =
        (u32(ib->resource.Common) & pgr4::gpu::kCommonIndex32) != 0;
    const u32 size = index32 ? 4u : 2u;
    if (u64(args.startIndex + count) * size <= u32(ib->Size)) {
      const auto *src = pgr4::mem::at<const u8>(u32(ib->Address) + args.startIndex * size);
      for (u32 i = 0; src && i < count; ++i)
        vertices.push_back(
            (index32 ? u32(reinterpret_cast<const be_u32 *>(src)[i])
                     : u32(u16(reinterpret_cast<const be_u16 *>(src)[i]))) +
            args.baseVertexIndex);
    }
  } else if (!args.indexed) {
    for (u32 i = 0; i < count; ++i)
      vertices.push_back(args.startVertex + i);
  }
  std::string list;
  for (u32 v : vertices)
    list += std::format(" {}", v);
  PGR4_INFO("[trace]   vs-input vertices{}", list);
  std::sort(vertices.begin(), vertices.end());
  vertices.erase(std::unique(vertices.begin(), vertices.end()), vertices.end());
  for (u32 stream = 0; stream < 2; ++stream) {
    const u32 stride = u32(dev.streamStrides[stream]) * 4;
    const auto *vb = pgr4::mem::at<const D3DVertexBuffer>(dev.streams[stream]);
    if (!vb || !dev.streams[stream] || !stride)
      continue;
    const u32 base = (u32(vb->Fetch0) & ~0x3u) + StreamOffset(dev, stream, *vb);
    std::string records;
    for (u32 v : vertices) {
      const auto *w = pgr4::mem::try_at<const be_u32>(base + v * stride);
      if (!w)
        continue;
      records += std::format(" | @{}", v);
      for (u32 i = 0; i < stride / 4; ++i)
        records += std::format(" {:08X}", u32(w[i]));
    }
    PGR4_INFO("[trace]   vs-input stream{} stride={} at 0x{:08X}{}", stream,
              stride, base, records);
  }
}

void DispatchDraw(u32 device_guest, u32 primitive_type, const DrawArgs &args) {
  pgr4::gpu::perf::Scope timer(pgr4::gpu::perf::Counter::Draw);
  PGR4_CPU_ZONE("Draw");
  const auto *dev = pgr4::mem::at<const D3DDevice>(device_guest);
  if (!dev || !args.vertexOrIndexCount)
    return;
  g_stats.issued.fetch_add(1, std::memory_order_relaxed);
  if (TracingThisFrame() && TracedVertexShader()) {
    TraceDraw(args.indexed ? "DrawIndexed" : "Draw", *dev, primitive_type,
              args.vertexOrIndexCount);
    // Full-screen passes: their PS constants (post grading lives there).
    if (!args.indexed && args.vertexOrIndexCount <= 6)
      TracePixelConstants(*dev);
    // The first few scene draws: every non-zero VS/PS float constant below
    // c64, enough to sanity-check lighting and fog parameters.
    static u32 s_dumped_frame = 0, s_dumped = 0;
    if (s_dumped_frame != pgr4::gpu::PresentCount()) {
      s_dumped_frame = pgr4::gpu::PresentCount();
      s_dumped = 0;
    }
    if (args.indexed && s_dumped++ < 3) {
      for (int stage = 0; stage < 2; ++stage) {
        std::string consts;
        for (u32 c = 0; c < 64; ++c) {
          const auto &v = stage ? dev->psFloatConstants[c] : dev->vsFloatConstants[c];
          const float f[4] = {float(v[0]), float(v[1]), float(v[2]), float(v[3])};
          if (f[0] == 0 && f[1] == 0 && f[2] == 0 && f[3] == 0)
            continue;
          consts += std::format(" c{}=({:.4g},{:.4g},{:.4g},{:.4g})", c, f[0],
                                f[1], f[2], f[3]);
        }
        PGR4_INFO("[trace]   {} consts{}", stage ? "ps" : "vs", consts);
      }
    }
    TraceVertexShaderInputs(*dev, args);
  }
  {
    pgr4::gpu::perf::Scope t(pgr4::gpu::perf::Counter::DrawGeometry);
    BindGuestGeometry(*dev, args.indexed);
  }

  // One lock across the whole recording sequence, as in reblue: the
  // per-frame command list is single-producer.
  auto &s = pgr4::gpu::state();
  std::unique_lock<std::mutex> lock(s.mutex);
  if (SkippedVertexShader(s.vertex_shader))
    return;
  Video::OpenCommandListLocked();
  Video::SetDirtyValue<plume::RenderPrimitiveTopology>(
      s.dirtyStates.pipelineState, s.pipelineState.primitiveTopology,
      MapPrimitiveType(primitive_type));
  bool bound;
  {
    pgr4::gpu::perf::Scope t(pgr4::gpu::perf::Counter::DrawFramebuffer);
    bound = Video::BindDrawFramebufferLocked();
  }
  if (!bound) {
    g_stats.no_framebuffer.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // Every host draw below passes this as its base or first vertex.
  pgr4::gpu::SetVertexIndexBase(args.indexed ? args.baseVertexIndex
                                             : static_cast<i32>(args.startVertex));
  bool flushed;
  {
    pgr4::gpu::perf::Scope t(pgr4::gpu::perf::Counter::DrawFlush);
    flushed = Video::FlushRenderStateLocked(device_guest);
  }
  if (!flushed) {
    g_stats.no_state.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  auto *cmd_list = s.command_list;
  if (!cmd_list)
    return;

  if (static_cast<xe::PrimitiveType>(primitive_type) ==
      xe::PrimitiveType::kQuadList) {
    u32 quads = args.vertexOrIndexCount / 4;
    quads = std::min(quads, Video::QuadlistMaxQuads());
    const auto *ib = Video::QuadlistExpansionIBView();
    if (args.indexed) {
      // An indexed quad list (the nitrous radial blur's grid) names its own
      // corners, so split the guest's quads rather than consecutive vertices.
      if (DrawIndexedQuadList(*dev, args, quads))
        s.dirtyStates.indices = true;
    } else if (quads && ib) {
      const i32 first = args.indexed ? args.baseVertexIndex
                                     : static_cast<i32>(args.startVertex);
      cmd_list->setIndexBuffer(ib);
      cmd_list->drawIndexedInstanced(quads * 6, 1, 0, first, 0);
      s.dirtyStates.indices = true;
    }
  } else if (static_cast<xe::PrimitiveType>(primitive_type) ==
                 xe::PrimitiveType::kTriangleFan &&
             !args.indexed) {
    const u32 tris = std::min(args.vertexOrIndexCount >= 3
                                  ? args.vertexOrIndexCount - 2
                                  : 0u,
                              Video::FanMaxTriangles());
    if (const auto *ib = Video::FanExpansionIBView(); tris && ib) {
      cmd_list->setIndexBuffer(ib);
      cmd_list->drawIndexedInstanced(tris * 3, 1, 0,
                                     static_cast<i32>(args.startVertex), 0);
      s.dirtyStates.indices = true;
    }
  } else if (args.indexed) {
    if (static_cast<xe::PrimitiveType>(primitive_type) ==
        xe::PrimitiveType::kTriangleFan) {
      static std::atomic<u32> s_warn{0};
      if (s_warn.fetch_add(1, std::memory_order_relaxed) < 4)
        PGR4_WARN("indexed triangle fan drawn as a triangle list");
    }
    if (s.index_view.buffer.ref == nullptr) {
      g_stats.no_index_buffer.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    cmd_list->drawIndexedInstanced(args.vertexOrIndexCount, 1, args.startIndex,
                                   args.baseVertexIndex, 0);
  } else if (static_cast<xe::PrimitiveType>(primitive_type) ==
                 xe::PrimitiveType::kPointList &&
             pgr4::gpu::TargetScaleLocked(s) > 1) {
    // A one-pixel guest point covers scale x scale host pixels. Host points
    // are one pixel, so draw the list once per host pixel of the block, each
    // pass moved onto that pixel's centre. RR6's only point draw is the sun
    // visibility count: 64 additive points summing the probe into one pixel
    // that the glow reads back bilinearly, so every host pixel of the block
    // needs the full sum.
    const u32 scale = pgr4::gpu::TargetScaleLocked(s);
    const float centre = 0.5f * float(scale) - 0.5f;
    for (u32 y = 0; y < scale; ++y) {
      for (u32 x = 0; x < scale; ++x) {
        pgr4::gpu::SetHostPixelNudge(float(x) - centre, float(y) - centre);
        if (!Video::FlushRenderStateLocked(device_guest))
          break;
        cmd_list->drawInstanced(args.vertexOrIndexCount, 1, args.startVertex,
                                0);
      }
    }
    pgr4::gpu::SetHostPixelNudge(0.0f, 0.0f);
  } else {
    cmd_list->drawInstanced(args.vertexOrIndexCount, 1, args.startVertex, 0);
  }
  g_stats.recorded.fetch_add(1, std::memory_order_relaxed);
  CountRenderTarget();
  pgr4::gpu::FxaaAfterCompositeLocked(s);
}

u32 D3DDevice_DrawVertices_hook(u32 device, u32 primitive_type,
                                u32 start_vertex, u32 vertex_count) {
  DrawArgs args;
  args.vertexOrIndexCount = vertex_count;
  args.startVertex = start_vertex;
  DispatchDraw(device, primitive_type, args);
  return 0;
}

// All five arguments are marshaled: IndexCount is in r7.
u32 D3DDevice_DrawIndexedVertices_hook(u32 device, u32 primitive_type,
                                       u32 base_vertex, u32 start_index,
                                       u32 index_count) {
  DrawArgs args;
  args.indexed = true;
  args.vertexOrIndexCount = index_count;
  args.baseVertexIndex = static_cast<i32>(base_vertex);
  args.startIndex = start_index;
  DispatchDraw(device, primitive_type, args);
  return 0;
}

// BeginVertices hands the game a pointer it fills, EndVertices draws it. The
// guest allocator returns discard command memory that stays valid until the
// next wrap, which is far longer than one Begin/End pair.
struct PendingUP {
  u32 device = 0;
  u32 primitive_type = 0;
  u32 vertex_count = 0;
  u32 stride = 0;
  u32 data = 0;
};
thread_local PendingUP g_pending;

} // namespace

namespace pgr4::gpu {

// For the other hooks' pgr4_trace_frame lines.
bool TraceFrameActive() { return TracingThisFrame(); }

// Draw totals so far, logged by the Present hook under pgr4_devmode.
void LogDrawStats(u32 frame) {
  PGR4_DEV_INFO("draws after frame {}: {} issued, {} recorded, skipped {} no "
           "framebuffer, {} state, {} no index buffer",
           frame, g_stats.issued.load(), g_stats.recorded.load(),
           g_stats.no_framebuffer.load(), g_stats.no_state.load(),
           g_stats.no_index_buffer.load());
  std::string rts;
  {
    std::lock_guard lock(g_stats.rt_mutex);
    for (const auto &[size, n] : g_stats.draws_per_rt)
      rts += std::format(" {}x{}:{}", size >> 32, size & 0xFFFFFFFFu, n);
  }
  PGR4_DEV_INFO("  textures {} resolved, {} unresolved, {} resolves; draws per "
               "RT:{}",
               g_stats.textures_resolved.load(),
               g_stats.textures_unresolved.load(), g_stats.resolves.load(), rts);
}

} // namespace pgr4::gpu

// (device, primitive type, vertex count, stride) -> vertex pointer
REX_HOOK_RAW(D3DDevice_BeginVertices) {
  PendingUP p{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 0};
  __imp__D3DDevice_BeginVertices(ctx, base);
  p.data = ctx.r3.u32;
  if (TracingThisFrame())
    PGR4_INFO("[trace] BeginVertices prim={} n={} stride={} data=0x{:08X}",
             p.primitive_type, p.vertex_count, p.stride, p.data);
  g_pending = p;
}

namespace {
// Records the draw BeginVertices left pending, once its vertices are written.
// Xenos completes each rect list triple into a rectangle: the fourth corner
// is opposite the right-angle vertex (the one off the longest edge). Builds
// the four corners in perimeter order, byte-swapped, for the quad-list path,
// computing the fourth in input space, which is exact for the pass-through
// vertex shaders of PGR3's full-screen passes. A dword the three vertices
// share (a colour) is copied, not summed. position_offset is the byte offset
// of the screen-space x, y floats.
// The quad winds as the guest's triple does, since Xenos culls a rect list
// like any triangle: PGR4's motion blur composite draws a clockwise triple
// under D3DCULL_CCW. Wound the other way, the host culled it, and the
// after-scene pass piled up frame over frame on the target it never redrew.
bool ExpandRectList(u32 data, u32 vertex_count, u32 stride,
                    u32 position_offset, std::vector<u32> &out) {
  const u32 rects = vertex_count / 3;
  const u32 dwords = stride / 4;
  const auto *src = pgr4::mem::at<const be_u32>(data);
  if (!rects || !dwords || stride % 4 || position_offset + 8 > stride || !src)
    return false;
  out.resize(size_t(rects) * 4 * dwords);
  auto as_float = [](u32 v) {
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
  };
  const u32 px = position_offset / 4;
  for (u32 r = 0; r < rects; ++r) {
    u32 v[3][64];
    if (dwords > 64)
      return false;
    for (u32 i = 0; i < 3; ++i)
      for (u32 d = 0; d < dwords; ++d)
        v[i][d] = u32(src[(r * 3 + i) * dwords + d]);
    auto edge = [&](u32 a, u32 b) {
      const float dx = as_float(v[a][px]) - as_float(v[b][px]);
      const float dy = as_float(v[a][px + 1]) - as_float(v[b][px + 1]);
      return dx * dx + dy * dy;
    };
    // The longest edge is the diagonal; the vertex off it is the corner.
    const float e01 = edge(0, 1), e12 = edge(1, 2), e20 = edge(2, 0);
    u32 corner = 2;
    if (e12 >= e01 && e12 >= e20)
      corner = 0;
    else if (e20 >= e01 && e20 >= e12)
      corner = 1;
    const u32 a = (corner + 1) % 3, b = (corner + 2) % 3;
    u32 *o = out.data() + size_t(r) * 4 * dwords;
    for (u32 d = 0; d < dwords; ++d) {
      const u32 va = v[a][d], vb = v[b][d], vc = v[corner][d];
      u32 fourth = va;
      if (!(va == vb && vb == vc)) {
        const float f = as_float(va) + as_float(vb) - as_float(vc);
        std::memcpy(&fourth, &f, sizeof(fourth));
      }
      // corner, a, fourth, b: (corner, a, b) is a rotation of the guest's
      // (v0, v1, v2), and both triangles of the quad wind as it does.
      o[d] = vc;
      o[dwords + d] = va;
      o[2 * dwords + d] = fourth;
      o[3 * dwords + d] = vb;
    }
  }
  return true;
}

// The byte offset of POSITION0 in stream 0 of the bound declaration (0 if
// there is none).
u32 PositionOffset() {
  auto &s = pgr4::gpu::state();
  std::lock_guard lock(s.mutex);
  const auto *decl = s.vertex_declaration;
  if (!decl || !decl->vertexElements)
    return 0;
  for (u32 i = 0; i + 1 < decl->vertexElementCount; ++i) {
    const auto &e = decl->vertexElements[i];
    if (u32(e.stream) == 0 && e.usage == 0 && e.usageIndex == 0)
      return u32(e.offset);
  }
  return 0;
}

void FlushPendingUP() {
  PendingUP p = g_pending;
  g_pending = {};
  if (!p.data || !p.vertex_count || !p.stride)
    return;
  const bool rect_list = static_cast<xe::PrimitiveType>(p.primitive_type) ==
                         xe::PrimitiveType::kRectangleList;
  const u32 traced_prim = p.primitive_type, traced_count = p.vertex_count;
  pgr4::gpu::ConstantAllocation alloc;
  if (rect_list) {
    std::vector<u32> quads;
    if (!ExpandRectList(p.data, p.vertex_count, p.stride, PositionOffset(),
                        quads)) {
      PGR4_WARN_FIRST(4, "rect list of {} vertices, stride {} not expanded",
                      p.vertex_count, p.stride);
      return;
    }
    alloc = pgr4::gpu::UploadHostBytes(
        quads.data(), static_cast<u32>(quads.size() * sizeof(u32)), 4);
    p.vertex_count = p.vertex_count / 3 * 4;
    p.primitive_type = static_cast<u32>(xe::PrimitiveType::kQuadList);
  } else {
    alloc = pgr4::gpu::UploadGuestBytesByteSwap32(
        p.data, p.vertex_count * p.stride, /*alignment=*/4);
  }
  if (!alloc.memory)
    return;
  if (const auto *dev = pgr4::mem::at<const D3DDevice>(p.device)) {
    if (TracingThisFrame()) {
      TraceDraw("DrawUP", *dev, traced_prim, traced_count);
      if (traced_count <= 6) {
        std::string verts;
        const auto *f = pgr4::mem::at<const be_f32>(p.data);
        for (u32 i = 0; f && i < traced_count * p.stride / 4; ++i)
          verts += std::format("{}{:.4f}", i % (p.stride / 4) ? "," : " | ",
                               float(f[i]));
        PGR4_INFO("[trace]   verts{}", verts);
        TracePixelConstants(*dev);
      }
    }
    BindGuestTextures(*dev);
  }
  Video::SetVertexStream(0, alloc.ref, alloc.size, p.stride);
  // Streams 1..15 of the device must not leak into an immediate-mode draw.
  for (u32 i = 1; i < 16; ++i)
    Video::SetVertexStream(i, {}, 0, 0);

  auto &s = pgr4::gpu::state();
  std::unique_lock<std::mutex> lock(s.mutex);
  Video::OpenCommandListLocked();
  Video::SetDirtyValue<plume::RenderPrimitiveTopology>(
      s.dirtyStates.pipelineState, s.pipelineState.primitiveTopology,
      MapPrimitiveType(p.primitive_type));
  g_stats.issued.fetch_add(1, std::memory_order_relaxed);
  bool bound;
  {
    pgr4::gpu::perf::Scope t(pgr4::gpu::perf::Counter::DrawFramebuffer);
    bound = Video::BindDrawFramebufferLocked();
  }
  if (!bound) {
    g_stats.no_framebuffer.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  pgr4::gpu::SetVertexIndexBase(0);
  if (!Video::FlushRenderStateLocked(p.device) || !s.command_list) {
    g_stats.no_state.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_stats.recorded.fetch_add(1, std::memory_order_relaxed);
  CountRenderTarget();
  if (static_cast<xe::PrimitiveType>(p.primitive_type) ==
      xe::PrimitiveType::kQuadList) {
    const u32 quads = std::min(p.vertex_count / 4, Video::QuadlistMaxQuads());
    if (const auto *ib = Video::QuadlistExpansionIBView(); quads && ib) {
      s.command_list->setIndexBuffer(ib);
      s.command_list->drawIndexedInstanced(quads * 6, 1, 0, 0, 0);
      s.dirtyStates.indices = true;
    }
  } else if (static_cast<xe::PrimitiveType>(p.primitive_type) ==
             xe::PrimitiveType::kTriangleFan) {
    const u32 tris = std::min(p.vertex_count >= 3 ? p.vertex_count - 2 : 0u,
                              Video::FanMaxTriangles());
    if (const auto *ib = Video::FanExpansionIBView(); tris && ib) {
      s.command_list->setIndexBuffer(ib);
      s.command_list->drawIndexedInstanced(tris * 3, 1, 0, 0, 0);
      s.dirtyStates.indices = true;
    }
  } else {
    s.command_list->drawInstanced(p.vertex_count, 1, 0, 0);
  }
  pgr4::gpu::FxaaAfterCompositeLocked(s);
}

} // namespace

// (device)
REX_HOOK_RAW(D3DDevice_EndVertices) {
  __imp__D3DDevice_EndVertices(ctx, base);
  FlushPendingUP();
}

// (device, primitive type, vertex count, data, stride)
REX_HOOK_RAW(D3DDevice_DrawVerticesUP) {
  g_pending = {};
  __imp__D3DDevice_DrawVerticesUP(ctx, base);
  FlushPendingUP();
}

// (device, flags, src rect, dest texture, dest point, level, face, clear
// color, ClearZ in f1, stencil, params). The guest body only builds PM4.
// A resolve into a texture copies the bound render target into that texture's
// host resolve target, created at the source's format. Afterwards the guest
// clears through ClearF when asked: D3DRESOLVE_CLEARRENDERTARGET (0x100)
// clears the resolved target's color from pClearColor (four floats),
// D3DRESOLVE_CLEARDEPTHSTENCIL (0x200) depth from f1 and stencil from the
// caller's stack at r1+92 (0x826CBEE0 reads 476(r1) under a 384-byte frame;
// 2099 read the same slot as 508(r1) under 416).
// A depth resolve (D3DRESOLVE_DEPTHSTENCIL) copies the depth surface into an
// R32_FLOAT texture: PGR3 samples the scene depth and its shadow maps as
// k_24_8_FLOAT, which fetches the depth into .x.
constexpr u32 kResolveClearRenderTarget = 0x100;
constexpr u32 kResolveClearDepthStencil = 0x200;

u32 FloatColorToArgb(const be_f32 *c) {
  auto byte = [](float v) {
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<u32>(v * 255.0f + 0.5f);
  };
  return byte(c[3]) << 24 | byte(c[0]) << 16 | byte(c[1]) << 8 | byte(c[2]);
}

// The work of D3DDevice_Resolve, shared with EndTiling. rect_va and point_va
// are guest D3DRECT/D3DPOINT pointers, 0 for the whole surface and the origin.
// pgr4_debug_fill_depth_resolve: the texture's whole tiled extent (32bpp,
// 32x32-aligned) at its fetch constant's physical base, big-endian.
void FillGuestTexture(u32 texture, u32 value) {
  const auto *fc = pgr4::mem::at<const be_u32>(
      texture + static_cast<u32>(offsetof(pgr4::gpu::D3DTexture, Format)));
  auto *memory = REX_KERNEL_MEMORY();
  if (!fc || !memory)
    return;
  const u32 base = u32(fc[1]) & 0xFFFFF000u;
  const u32 pitch = ((u32(fc[0]) >> 22) & 0x1FFu) * 32;
  const u32 height = ((u32(fc[2]) >> 13) & 0x1FFFu) + 1;
  const u32 dwords = pitch * ((height + 31) & ~31u);
  auto *dst = reinterpret_cast<be_u32 *>(memory->TranslatePhysical(base));
  if (!dst || !base)
    return;
  for (u32 i = 0; i < dwords; ++i)
    dst[i] = value;
  PGR4_WARN_FIRST(8, "filled depth resolve 0x{:08X} ({}x{}, base 0x{:08X}) with 0x{:08X}",
                  texture, pitch, height, base, value);
}

// A 7e3 surface's alpha comes back as 1. D3D packs the clear's alpha as
// given (PGR3's frame-end resolve clears to 0,0,0,0), but on Xenos the
// scene's EDRAM is then shared: the shadow and environment passes put
// their depth and colour at bases 0 and 224, inside the scene colour's
// tiles, and D24FS8 depth near 1.0 has the top two bits that 7e3 reads as
// alpha. xenia's EDRAM-exact path (render_target_path_d3d12=rov) resolves
// the scene with alpha 1 at every pixel. The host does not alias EDRAM,
// so every host clear of a 7e3 target (the resolve's and BeginTiling's)
// clears alpha to that. The scene's opaque geometry masks alpha off and
// keeps it, and the motion blur blurs only where alpha is 1 (PGR3's
// ps_8200F068, PGR4's ps_821AE5A0). PGR4's tone map writes RGB only into
// its 7e3 final target, and the loading screen before a race blends the
// last menu frame resolved from it (0x400CD380) by that alpha.
u32 SceneClearColor(const pgr4::gpu::GuestTexture *target, u32 color) {
  if (target && (target->guestFormat ==
                     static_cast<u32>(pgr4::gpu::D3DFormat::kA2B10G10R10FEdram) ||
                 target->guestFormat ==
                     static_cast<u32>(pgr4::gpu::D3DFormat::kA2B10G10R10FEdramAlt)))
    color |= 0xFF000000u;
  return color;
}

struct ResolveArgs {
  u32 flags;
  u32 rect_va;
  u32 dest;
  u32 point_va;
  u32 level;
  u32 face;
  u32 clear_color_va;
  double clear_z;
  u32 clear_stencil;
  u32 sp; // guest r1, for the trace's call chain
  u32 lr;
};

void ResolveNative(const ResolveArgs &a) {
  const u32 flags = a.flags;
  const u32 dest = a.dest;
  const u32 level = a.level;
  const u32 face = a.face;
  const u32 clear_color_va = a.clear_color_va;
  const double clear_z = a.clear_z;
  const u32 clear_stencil = a.clear_stencil;

  const bool depth = (flags & pgr4::gpu::kResolveDepthStencil) != 0;
  // D3DRESOLVE_RENDERTARGET0-3 in bits 0-1; slot 1 is modelled.
  pgr4::gpu::GuestTexture *src = depth ? pgr4::gpu::state().depth_stencil
                                 : (flags & 3) == 1
                                     ? pgr4::gpu::state().render_target1
                                     : pgr4::gpu::state().render_target;
  if (!depth && (flags & 3) > 1)
    PGR4_WARN_FIRST(4, "Resolve: render target {} is not modelled", flags & 3);
  if (TracingThisFrame()) {
    // D3DRECT {x1,y1,x2,y2}, D3DPOINT {x,y}; the surface words carry the
    // EDRAM base and pitch the guest resolves from.
    std::string rect = "full", point = "0,0", words;
    if (const auto *r = pgr4::mem::at<const be_u32>(a.rect_va); r && a.rect_va)
      rect = std::format("{},{}-{},{}", u32(r[0]), u32(r[1]), u32(r[2]), u32(r[3]));
    if (const auto *p = pgr4::mem::at<const be_u32>(a.point_va); p && a.point_va)
      point = std::format("{},{}", u32(p[0]), u32(p[1]));
    point += " chain:" + GuestCallChain(a.sp, 0, 6);
    const u32 surface_va = pgr4::gpu::BoundRenderTargetVa();
    if (const auto *w = pgr4::mem::at<const be_u32>(surface_va); w && surface_va)
      for (int i = 12; i < 15; ++i)
        words += std::format(" {:08X}", u32(w[i]));
    if (const auto *c = pgr4::mem::at<const be_f32>(a.clear_color_va);
        c && a.clear_color_va && (flags & kResolveClearRenderTarget))
      words += std::format(" clear=({:.3g},{:.3g},{:.3g},{:.3g})", float(c[0]),
                           float(c[1]), float(c[2]), float(c[3]));
    PGR4_INFO("[trace] Resolve flags=0x{:X} dest=0x{:08X} src={}x{} rect={} "
             "point={} surface=0x{:08X}:{} lr=0x{:08X}",
             flags, dest, src ? src->width : 0, src ? src->height : 0, rect,
             point, surface_va, words, a.lr);
  }
  // Each destination's first resolve, to find where runtime textures (like
  // the material crop sheet) come from without tracing their frame.
  {
    static std::mutex s_mutex;
    static std::unordered_set<u32> s_seen;
    std::lock_guard lock(s_mutex);
    if (dest && s_seen.insert(dest).second) {
      std::string rect = "full";
      if (const auto *r = pgr4::mem::at<const be_u32>(a.rect_va); r && a.rect_va)
        rect = std::format("{},{}-{},{}", u32(r[0]), u32(r[1]), u32(r[2]), u32(r[3]));
      PGR4_INFO("first resolve into 0x{:08X}: flags=0x{:X} src={}x{} fmt={} "
                "rect={} frame={} chain:{}",
                dest, flags, src ? src->width : 0, src ? src->height : 0,
                src ? static_cast<int>(src->format) : -1, rect,
                pgr4::gpu::PresentCount(), GuestCallChain(a.sp, a.lr, 6));
    }
  }
  // The guest clears only the resolved rect; the host clears the whole target.
  if (a.rect_va &&
      (flags & (kResolveClearRenderTarget | kResolveClearDepthStencil)))
    PGR4_WARN_FIRST(4, "Resolve: clear limited to a source rect clears the "
                   "whole target (flags=0x{:X})", flags);
  if (depth && dest) {
    const std::string &fill = REXCVAR_GET(pgr4_debug_fill_depth_resolve);
    if (!fill.empty())
      FillGuestTexture(dest, u32(std::stoul(fill, nullptr, 16)));
  }
  if (dest && src)
    pgr4::gpu::SnapshotForFrameDump(src, std::format("{:08X}", dest));
  if (dest && src && src->texture) {
    const plume::RenderFormat format =
        depth ? plume::RenderFormat::R32_FLOAT
              : pgr4::gpu::ResolveTargetHostFormat(dest, src->format);
    // A depth resolve keeps its source's scale: the car shadow maps are
    // corners of shadow surfaces that render at pgr4_shadow_scale.
    if (auto *dst = pgr4::gpu::GetOrCreateResolveTarget(
            dest, format, depth ? src->hostScale : 0u)) {
      Video::TrackResolveSource(flags, dst, level, face);
      // D3DRECT {x1,y1,x2,y2} in surface pixels and D3DPOINT {x,y}.
      plume::RenderRect rect;
      const plume::RenderRect *rect_ptr = nullptr;
      if (const auto *r = pgr4::mem::at<const be_i32>(a.rect_va); r && a.rect_va) {
        rect = plume::RenderRect(i32(r[0]), i32(r[1]), i32(r[2]), i32(r[3]));
        rect_ptr = &rect;
      }
      u32 point_x = 0, point_y = 0;
      if (const auto *p = pgr4::mem::at<const be_u32>(a.point_va); p && a.point_va) {
        point_x = p[0];
        point_y = p[1];
      }
      Video::ResolveRtToTexture(dst, rect_ptr, point_x, point_y);
      g_stats.resolves.fetch_add(1, std::memory_order_relaxed);
    }
  }

  u32 clear_flags = 0;
  u32 color = 0;
  if (flags & kResolveClearRenderTarget) {
    clear_flags |= pgr4::gpu::kClearTarget;
    if (const auto *c = pgr4::mem::at<const be_f32>(clear_color_va))
      color = FloatColorToArgb(c);
    color = SceneClearColor(src, color);
  }
  if (flags & kResolveClearDepthStencil)
    clear_flags |= pgr4::gpu::kClearZBuffer | pgr4::gpu::kClearStencil;
  if (clear_flags)
    Video::Clear(clear_flags, color, static_cast<float>(clear_z), clear_stencil);
}

REX_HOOK_RAW(D3DDevice_Resolve) {
  (void)base;
  const u32 *stencil_slot = pgr4::mem::at<const u32>(ctx.r1.u32 + 92);
  ResolveNative({.flags = ctx.r4.u32,
                 .rect_va = ctx.r5.u32,
                 .dest = ctx.r6.u32,
                 .point_va = ctx.r7.u32,
                 .level = ctx.r8.u32,
                 .face = ctx.r9.u32,
                 .clear_color_va = ctx.r10.u32,
                 .clear_z = ctx.f1.f64,
                 .clear_stencil = stencil_slot
                                      ? u32(*reinterpret_cast<const be_u32 *>(stencil_slot))
                                      : 0,
                 .sp = ctx.r1.u32,
                 .lr = u32(ctx.lr)});
  ctx.r3.u64 = 0;
}

// Predicated tiling. PGR4 tiles every race frame: two tiles, 1280x480 and
// 1280x240, of a 1280x720 2x MSAA scene. The guest's BeginTiling/EndTiling
// replay the command buffer per tile and resolve each one, through ring code
// the native device does not set up. The host has no EDRAM limit, so the
// draws between them render the whole extent once, into host surfaces of the
// extent's size bound in place of the tile-sized guest ones
// (pgr4::gpu::SetTiledExtent). BeginTiling reduces to that, the first tile's
// clear, and the device fields that make sub_826C6100 clamp viewports to the
// extent rather than to the bound surface; EndTiling to one whole-surface
// resolve. The tile count stays 0, so the rest of D3D behaves untiled.
//
// (device, flags, tile count, tile rects, clear color, ClearZ in f1, stencil
// in r9). Flags bit 0 skips the clear; otherwise the guest clears all four
// colour targets, depth and stencil (ClearF flags 0x3F).
REX_HOOK_RAW(D3DDevice_BeginTiling) {
  const u32 device_va = ctx.r3.u32;
  auto *dev = pgr4::mem::at<pgr4::gpu::D3DDevice>(device_va);
  // D3DRECT {x1,y1,x2,y2}; the extent is the largest x2 and y2, as the guest
  // stores it.
  u32 width = 0, height = 0;
  if (const auto *r = pgr4::mem::at<const be_u32>(ctx.r6.u32); r && ctx.r6.u32)
    for (u32 i = 0; i < ctx.r5.u32; ++i) {
      width = std::max(width, u32(r[i * 4 + 2]));
      height = std::max(height, u32(r[i * 4 + 3]));
    }
  if (TracingThisFrame())
    PGR4_INFO("[trace] BeginTiling flags=0x{:X} tiles={} extent={}x{} lr=0x{:08X}",
              ctx.r4.u32, ctx.r5.u32, width, height, u32(ctx.lr));
  if (!dev || !width || !height)
    return;
  dev->tileExtentWidth = width;
  dev->tileExtentHeight = height;
  for (u32 i = 0; i < 4; ++i)
    dev->tiledRenderTargets[i] = u32(dev->renderTargets[i]);
  dev->tiledDepthStencil = u32(dev->depthStencil);
  dev->tilingFlags = dev->tilingFlags | 0x20u;
  pgr4::gpu::SetTiledExtent(ctx, base, device_va, width, height);
  if (!(ctx.r4.u32 & 1)) {
    u32 color = 0;
    if (const auto *c = pgr4::mem::at<const be_f32>(ctx.r7.u32); c && ctx.r7.u32)
      color = FloatColorToArgb(c);
    color = SceneClearColor(pgr4::gpu::state().render_target, color);
    Video::Clear(pgr4::gpu::kClearAll, color, static_cast<float>(ctx.f1.f64), ctx.r9.u32);
  }
  // The guest ends on a full viewport, which now clamps to the extent:
  // D3DVIEWPORT9 {X, Y, Width, Height, MinZ, MaxZ} on an isolated frame.
  rex::CallFrame frame(ctx);
  const u32 vp_va = (ctx.r1.u32 - 0x100) & ~0xFu;
  if (auto *vp = pgr4::mem::at<be_u32>(vp_va)) {
    vp[0] = 0;
    vp[1] = 0;
    vp[2] = width;
    vp[3] = height;
    reinterpret_cast<be_f32 *>(vp)[4] = 0.0f;
    reinterpret_cast<be_f32 *>(vp)[5] = 1.0f;
    frame.ctx.r1.u64 = vp_va - 0x80;
    frame.ctx.r3.u64 = device_va;
    frame.ctx.r4.u64 = vp_va;
    __imp__D3DDevice_SetViewport(frame.ctx, base);
  }
}

// (device, resolve flags, resolve rects, dest texture, clear color, ClearZ in
// f1, stencil in r9, parameters). With no destination the guest resolves
// nothing.
REX_HOOK_RAW(D3DDevice_EndTiling) {
  const u32 device_va = ctx.r3.u32;
  if (TracingThisFrame())
    PGR4_INFO("[trace] EndTiling flags=0x{:X} dest=0x{:08X} lr=0x{:08X}", ctx.r4.u32,
              ctx.r6.u32, u32(ctx.lr));
  // Resolve from the tiled surfaces, then put the untiled ones back.
  if (ctx.r6.u32)
    ResolveNative({.flags = ctx.r4.u32,
                 .rect_va = 0,
                 .dest = ctx.r6.u32,
                 .point_va = 0,
                 .level = 0,
                 .face = 0,
                 .clear_color_va = ctx.r7.u32,
                 .clear_z = ctx.f1.f64,
                 .clear_stencil = ctx.r9.u32,
                 .sp = ctx.r1.u32,
                 .lr = u32(ctx.lr)});
  if (auto *dev = pgr4::mem::at<pgr4::gpu::D3DDevice>(device_va);
      dev && (dev->tilingFlags & 0x20u)) {
    // Bit 0 too: SetRenderTarget recomputes it for the next bind.
    dev->tilingFlags = dev->tilingFlags & ~0x21u;
    pgr4::gpu::SetTiledExtent(ctx, base, device_va, 0, 0);
  }
}

REX_HOOK(D3DDevice_DrawVertices, D3DDevice_DrawVertices_hook);
REX_HOOK(D3DDevice_DrawIndexedVertices, D3DDevice_DrawIndexedVertices_hook);
