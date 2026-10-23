// pgr4 - ReXGlue Recompiled Project
//
// xenos plugin build only: pgr4_dump_frames="N,..." writes the destination
// texture of every D3DDevice_Resolve the guest issues during present frame N
// as pgr4_frame_N_r<S>_<VA>_<W>x<H>.bmp, S being the resolve's sequence number
// in the frame, so the two builds' intermediate surfaces can be compared
// directly. Both games resolve several steps of a chain into one aliased
// target (PGR4 resolves eight 128x128 targets in a row into one base before
// each race frame), so each resolve is dumped as soon as it lands: the hook
// inserts a fence after it and blocks on it, which with readback on means the
// texels are in guest memory. That stalls the GPU, so only the dumped frames
// pay for it. Every format a PGR4 race resolves is handled (see
// DumpTexture); the texel values written (alpha included, in the BMP's
// fourth byte) are what a shader would sample through the fetch swizzle. The
// plugin keeps resolve output on the GPU unless told otherwise, so run with
//   --readback_resolve=full --d3d12_readback_resolve=true
// Ported from pgr3, with its endian, tiled-offset and FP16 fixes.
//
// pgr4_trace_frame=N logs the D3D calls of present frames N to N+2: render
// targets and depth with their surface headers, draws with their textures,
// streams, index buffer, shaders and the main register shadows, clears,
// resolves and tiling. Every offset comes from gpu/d3d.h, so the log is also
// the live check of that header. Some 6132 paths log whenever they run,
// traced frame or not, because whether PGR4 reaches them at all is the
// question: a second Direct3D_CreateDevice (type 2 makes a command-buffer
// device), D3DDevice_RunCommandBuffer and the Z pass. shader_census.cpp
// warns on D3DDevice_Create{Vertex,Pixel}Shader the same way.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/settings.h"
#include "gpu/d3d.h"

REXCVAR_DEFINE_STRING(pgr4_dump_frames, "", kCvarGroup,
                      "Comma-separated present frame numbers whose resolve "
                      "targets are written as BMPs in the user data folder");

REXCVAR_DEFINE_INT32(pgr4_trace_frame, 0, kCvarGroup,
                     "Log the D3D calls of this present frame and the next two "
                     "(render targets, draws with their textures, resolves)");

REXCVAR_DEFINE_BOOL(pgr4_dump_raw, false, kCvarGroup,
                    "With pgr4_dump_frames, also write each resolve target's "
                    "memory (from 64 KiB before its base) as a .bin");

REX_EXTERN(__imp__D3DDevice_Resolve);
REX_EXTERN(__imp__D3DDevice_InsertFence);
REX_EXTERN(__imp__D3DDevice_BlockOnFence);
REX_EXTERN(__imp__D3DDevice_SetRenderTarget);
REX_EXTERN(__imp__D3DDevice_SetDepthStencilSurface);
REX_EXTERN(__imp__D3DDevice_DrawVertices);
REX_EXTERN(__imp__D3DDevice_DrawIndexedVertices);
REX_EXTERN(__imp__D3DDevice_BeginVertices);
REX_EXTERN(__imp__D3DDevice_Clear);
REX_EXTERN(__imp__D3DDevice_BeginTiling);
REX_EXTERN(__imp__D3DDevice_EndTiling);
REX_EXTERN(__imp__D3DDevice_BeginZPass);
REX_EXTERN(__imp__D3DDevice_EndZPass);
REX_EXTERN(__imp__D3DDevice_RunCommandBuffer);
REX_EXTERN(__imp__Direct3D_CreateDevice);

namespace {

namespace tu = rex::graphics::texture_util;
using pgr4::gpu::D3DDevice;

constexpr u32 kTextureFetchOffset = offsetof(pgr4::gpu::D3DTexture, Format);

std::mutex g_mutex;
std::vector<u32> g_wanted;
bool g_parsed = false;
u32 g_frame = 0;    // presents completed
u32 g_resolves = 0; // resolves so far in the current present

bool Tracing() {
  const i32 wanted = REXCVAR_GET(pgr4_trace_frame);
  // Three presents: the plugin build's frames do not always hold a whole
  // scene render between two presents.
  return wanted > 0 && g_frame + 1 >= static_cast<u32>(wanted) &&
         g_frame + 1 < static_cast<u32>(wanted) + 3;
}

template <typename T> const T *DeviceField(u32 dev, size_t offset) {
  return pgr4::mem::at<const T>(dev + static_cast<u32>(offset));
}

// The state every draw line carries: render target 0, depth, the first four
// textures, both shaders, the declaration and the register shadows that
// decide how a draw blends and tests.
void TraceCall(const char *what, PPCContext &ctx, const std::string &extra) {
  const u32 dev = ctx.r3.u32;
  const auto *d = pgr4::mem::at<const D3DDevice>(dev);
  if (!d) {
    PGR4_INFO("[xtrace] {}{} lr=0x{:08X}", what, extra, u32(ctx.lr));
    return;
  }
  std::string t;
  for (int i = 0; i < 4; ++i)
    if (u32(d->textures[i]))
      t += std::format(" t{}=0x{:08X}", i, u32(d->textures[i]));
  PGR4_INFO("[xtrace] {}{} rt=0x{:08X} z=0x{:08X}{} vs=0x{:08X} ps=0x{:08X} decl=0x{:08X} "
            "depth={:08X} blend0={:08X} color={:08X} mode={:08X} mask={:08X} lr=0x{:08X}",
            what, extra, u32(d->renderTargets[0]), u32(d->depthStencil), t,
            u32(d->vertexShader), u32(d->pixelShader), u32(d->vertexDeclaration),
            u32(d->rbDepthControl), u32(d->rbBlendControl0), u32(d->rbColorControl),
            u32(d->paSuScModeCntl), u32(d->rbColorMask), u32(ctx.lr));
}

// The resource headers a draw reads, decoded as gpu/d3d.h lays them out:
// Common (type in bits 0-3: 1 VB, 2 IB), then a VB's fetch words at
// +0x18/+0x1C, or an IB's base and size there (INDEX32 in Common bit 31).
// Each stream also shows its device fetch constant (95 - stream), where
// SetStreamSource folds in the offset.
std::string DescribeBuffers(u32 dev) {
  std::string out;
  const auto *d = pgr4::mem::at<const D3DDevice>(dev);
  if (!d)
    return out;
  for (int i = 0; i < 4; ++i) {
    const u32 vb = u32(d->streams[i]);
    if (!vb)
      continue;
    const auto *h = pgr4::mem::at<const pgr4::gpu::D3DVertexBuffer>(vb);
    const u32 slot = 95 - i;
    const auto *fc = DeviceField<be_u32>(dev, offsetof(D3DDevice, fetchConstants) + slot * 8);
    if (h && fc)
      out += std::format(" s{}=0x{:08X}{{c={:08X} rc={} f0={:08X} f1={:08X} dev={:08X}/{:08X} "
                         "stride={}}}",
                         i, vb, u32(h->resource.Common), u32(h->resource.ReferenceCount),
                         u32(h->Fetch0), u32(h->Fetch1), u32(fc[0]), u32(fc[1]),
                         u32(d->streamStrides[i]) * 4);
  }
  const u32 ib = u32(d->indexBuffer);
  if (const auto *h = ib ? pgr4::mem::at<const pgr4::gpu::D3DIndexBuffer>(ib) : nullptr)
    out += std::format(" ib=0x{:08X}{{c={:08X} base={:08X} size={}}}", ib,
                       u32(h->resource.Common), u32(h->Address), u32(h->Size));
  return out;
}

// A render target or depth surface: Common, RB_SURFACE_INFO (+0x18: pitch
// bits 0-13, MSAA bits 16-17), RB_COLOR_INFO / RB_DEPTH_INFO (+0x1C: EDRAM
// base bits 0-11, format bits 16-19), size and D3DFORMAT.
std::string DescribeSurface(u32 surface) {
  const auto *h = surface ? pgr4::mem::at<const pgr4::gpu::D3DSurface>(surface) : nullptr;
  if (!h)
    return std::string();
  const u32 info = u32(h->SurfaceInfo), edram = u32(h->EdramInfo), size = u32(h->Size);
  return std::format(" {{c={:08X} rc={} pitch={} msaa={} base={} fmt={} {}x{} d3dfmt={:08X} "
                     "edram={}}}",
                     u32(h->resource.Common), u32(h->resource.ReferenceCount), info & 0x3FFF,
                     (info >> 16) & 3, edram & 0xFFF, (edram >> 16) & 0xF,
                     ((size >> 18) & 0x3FFF) + 1, ((size >> 3) & 0x7FFF) + 1, u32(h->Format),
                     u32(h->EdramSize));
}

// A texture's Common and the size and format from its fetch constant.
std::string DescribeTexture(u32 texture) {
  const auto *h = texture ? pgr4::mem::at<const pgr4::gpu::D3DTexture>(texture) : nullptr;
  if (!h)
    return std::string();
  const u32 fc1 = u32(h->Format.dword[1]), fc2 = u32(h->Format.dword[2]);
  return std::format(" {{c={:08X} fmt={} base={:08X} {}x{}}}", u32(h->resource.Common),
                     fc1 & 0x3F, fc1 & 0xFFFFF000, (fc2 & 0x1FFF) + 1,
                     ((fc2 >> 13) & 0x1FFF) + 1);
}

void ParseWanted() {
  if (g_parsed)
    return;
  g_parsed = true;
  const std::string text = REXCVAR_GET(pgr4_dump_frames);
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find(',', pos);
    if (end == std::string::npos)
      end = text.size();
    if (end > pos)
      g_wanted.push_back(static_cast<u32>(std::stoul(text.substr(pos, end - pos))));
    pos = end + 1;
  }
}

bool WriteBmp(const std::filesystem::path &path, u32 width, u32 height,
              const std::vector<u8> &bgra) {
  FILE *f = std::fopen(path.string().c_str(), "wb");
  if (!f)
    return false;
  const u32 image = width * height * 4;
  u8 header[54] = {'B', 'M'};
  auto put32 = [&](int off, u32 v) { std::memcpy(header + off, &v, 4); };
  auto put16 = [&](int off, u16 v) { std::memcpy(header + off, &v, 2); };
  put32(2, 54 + image);
  put32(10, 54);
  put32(14, 40);
  put32(18, width);
  put32(22, static_cast<u32>(-static_cast<i32>(height))); // top-down
  put16(26, 1);
  put16(28, 32);
  put32(34, image);
  std::fwrite(header, 1, sizeof(header), f);
  std::fwrite(bgra.data(), 1, image, f);
  std::fclose(f);
  return true;
}

void DumpTexture(u32 frame, u32 seq, u32 va, u32 slice, const std::filesystem::path &dir) {
  const auto *words = pgr4::mem::at<const be_u32>(va + kTextureFetchOffset);
  auto *memory = REX_KERNEL_MEMORY();
  if (!words || !memory)
    return;
  u32 fc[6];
  for (int i = 0; i < 6; ++i)
    fc[i] = u32(words[i]);
  const u32 format = fc[1] & 0x3F;
  const u32 endian = (fc[1] >> 6) & 3;
  u32 base = fc[1] & 0xFFFFF000;
  const bool tiled = (fc[0] >> 31) & 1;
  const u32 pitch = ((fc[0] >> 22) & 0x1FF) * 32;
  const u32 width = (fc[2] & 0x1FFF) + 1;
  const u32 height = ((fc[2] >> 13) & 0x1FFF) + 1;
  const u32 swizzle = (fc[3] >> 1) & 0xFFF;
  // 32bpp: k_8_8_8_8, k_2_10_10_10 and k_2_10_10_10_AS_16_16_16_16 (PGR4's
  // front buffer), k_8_8_8_8_A, k_16_16_FLOAT.
  // 64bpp: k_16_16_16_16_FLOAT, the HDR scene format of both games, and
  // k_16_16_16_16_EXPAND, which samples as FP16 too (the SDK maps it to
  // R16G16B16A16_FLOAT). Floats are clamped to 0..1 for the BMP and the
  // largest component is logged.
  // 16bpp: k_6_5_5 (PGR3's tree impostor atlas), x in bits 0-4, y 5-9,
  // z 10-15.
  // Single channel: k_32_FLOAT (PGR4's 1x1 adaptation targets) and
  // k_24_8_FLOAT (depth: 20e4 in bits 8-31, stencil below). Written as grey
  // stretched over the range found, which is logged.
  const bool half = format == 31 || format == 32 || format == 29;
  const bool f655 = format == 5;
  const bool rgb10 = format == 7 || format == 54;
  const bool scalar = format == 36 || format == 23;
  const u32 bpp = format == 32 || format == 29 ? 8 : f655 ? 2 : 4;
  if (format != 6 && format != 14 && !rgb10 && !half && !f655 && !scalar) {
    PGR4_INFO("pgr4_dump_frames: skip 0x{:08X} format {}", va, format);
    return;
  }
  // With readback on, tiled resolve output lands 4 KiB past the fetch
  // constant's base, at 32bpp and 64bpp alike (fitted in pgr3 from raw dumps).
  const u32 aligned_h = (height + 31) & ~31u;
  // A cube face or array slice: the slices follow one another, each padded
  // to 4 KiB. PGR4 resolves its six environment faces into one cube.
  base += slice * ((pitch * aligned_h * bpp + 4095) & ~4095u);
  const u8 *src = memory->TranslatePhysical(base) + (tiled ? 4096u : 0u);
  if (REXCVAR_GET(pgr4_dump_raw)) {
    // Raw bytes for working out layouts offline: the window starts 64 KiB
    // below the base and runs 64 KiB past the aligned extent.
    constexpr u32 kSlack = 64 * 1024;
    const u32 lo = base >= kSlack ? base - kSlack : 0;
    const size_t size = size_t(base - lo) + size_t(pitch) * aligned_h * bpp + kSlack;
    const auto raw = dir / std::format("pgr4_frame_{}_r{:02}_{:08X}_{}x{}_fmt{}_e{}_p{}_t{}_base{:08X}_lo{:08X}.bin",
                                       frame, seq, va, width, height, format, endian,
                                       pitch, u32(tiled), base, lo);
    if (FILE *f = std::fopen(raw.string().c_str(), "wb")) {
      std::fwrite(memory->TranslatePhysical(lo), 1, size, f);
      std::fclose(f);
    }
  }
  auto swap = [endian](const u8 *b) -> u32 {
    switch (endian) {
    case 2: // 8in32
      return u32(b[0]) << 24 | u32(b[1]) << 16 | u32(b[2]) << 8 | b[3];
    case 1: // 8in16
      return u32(b[2]) << 24 | u32(b[3]) << 16 | u32(b[0]) << 8 | b[1];
    case 3: // 16in32
      return u32(b[1]) << 24 | u32(b[0]) << 16 | u32(b[3]) << 8 | b[2];
    default:
      return u32(b[3]) << 24 | u32(b[2]) << 16 | u32(b[1]) << 8 | b[0];
    }
  };
  auto half_to_float = [](u32 h) {
    const u32 e = (h >> 10) & 0x1F, m = h & 0x3FF;
    const float mag = e == 0    ? std::ldexp(float(m), -24)
                      : e == 31 ? 65504.0f
                                : std::ldexp(float(m | 0x400), int(e) - 25);
    return (h & 0x8000) ? -mag : mag;
  };
  float peak = 0.0f;
  std::vector<u8> bgra(size_t(width) * height * 4);
  std::vector<float> values(scalar ? size_t(width) * height : 0);
  for (u32 y = 0; y < height; ++y) {
    for (u32 x = 0; x < width; ++x) {
      const u32 off = tiled ? u32(tu::GetTiledOffset2D(i32(x), i32(y), pitch,
                                                       bpp == 8 ? 3 : bpp == 2 ? 1 : 2))
                            : (y * pitch + x) * bpp;
      if (off + bpp > pitch * aligned_h * bpp)
        continue;
      // A 16bpp texel is swapped within its own two bytes (8in16).
      const u32 v = f655 ? (endian == 1 || endian == 2
                                ? u32(src[off]) << 8 | src[off + 1]
                                : u32(src[off + 1]) << 8 | src[off])
                         : swap(src + off);
      if (scalar) {
        values[size_t(y) * width + x] =
            format == 23 ? rex::graphics::xenos::Float20e4To32(v >> 8) : std::bit_cast<float>(v);
        continue;
      }
      u8 comp[6] = {u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24), 0, 255};
      if (f655) {
        comp[0] = u8((v & 0x1F) * 255 / 31);
        comp[1] = u8(((v >> 5) & 0x1F) * 255 / 31);
        comp[2] = u8(((v >> 10) & 0x3F) * 255 / 63);
        comp[3] = 255;
      } else if (rgb10) { // 2_10_10_10: keep the top 8 bits of each channel
        comp[0] = u8((v >> 2) & 0xFF);
        comp[1] = u8((v >> 12) & 0xFF);
        comp[2] = u8((v >> 22) & 0xFF);
        comp[3] = u8(((v >> 30) & 3) * 85);
      } else if (half) {
        const u32 v1 = bpp == 8 ? swap(src + off + 4) : 0x3C000000u; // B 0, A 1
        const u32 h[4] = {v & 0xFFFF, v >> 16, v1 & 0xFFFF, v1 >> 16};
        for (int c = 0; c < 4; ++c) {
          const float f = half_to_float(h[c]);
          peak = std::max(peak, f);
          comp[c] = u8(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
      }
      auto pick = [&](u32 sel) { return comp[std::min<u32>(sel, 5)]; };
      u8 *out = bgra.data() + (size_t(y) * width + x) * 4;
      out[2] = pick(swizzle & 7);
      out[1] = pick((swizzle >> 3) & 7);
      out[0] = pick((swizzle >> 6) & 7);
      out[3] = pick((swizzle >> 9) & 7);
    }
  }
  std::string range;
  if (scalar) {
    const auto [lo_it, hi_it] = std::minmax_element(values.begin(), values.end());
    const float lo = *lo_it, hi = *hi_it, span = hi > lo ? hi - lo : 1.0f;
    for (size_t i = 0; i < values.size(); ++i) {
      const u8 g = u8(std::clamp((values[i] - lo) / span, 0.0f, 1.0f) * 255.0f + 0.5f);
      bgra[i * 4] = bgra[i * 4 + 1] = bgra[i * 4 + 2] = g;
      bgra[i * 4 + 3] = 255;
    }
    range = std::format(" range={:.6g}..{:.6g}", lo, hi);
  }
  const auto path = dir / std::format("pgr4_frame_{}_r{:02}_{:08X}{}_{}x{}.bmp", frame,
                                      seq, va, slice ? std::format("_s{}", slice) : std::string(),
                                      width, height);
  if (WriteBmp(path, width, height, bgra))
    PGR4_INFO("pgr4_dump_frames: wrote {} fmt={} endian={} tiled={} swz={:03X}{}{}",
             path.string(), format, endian, u32(tiled), swizzle,
             half ? std::format(" peak={:.3f}", peak) : std::string(), range);
}

} // namespace

// (device, index, surface).
REX_HOOK_RAW(D3DDevice_SetRenderTarget) {
  if (Tracing())
    PGR4_INFO("[xtrace] SetRenderTarget {} 0x{:08X}{} lr=0x{:08X} frame={}", ctx.r4.u32,
              ctx.r5.u32, DescribeSurface(ctx.r5.u32), u32(ctx.lr), g_frame + 1);
  __imp__D3DDevice_SetRenderTarget(ctx, base);
}

// (device, surface).
REX_HOOK_RAW(D3DDevice_SetDepthStencilSurface) {
  if (Tracing())
    PGR4_INFO("[xtrace] SetDepthStencilSurface 0x{:08X}{} lr=0x{:08X} frame={}", ctx.r4.u32,
              DescribeSurface(ctx.r4.u32), u32(ctx.lr), g_frame + 1);
  __imp__D3DDevice_SetDepthStencilSurface(ctx, base);
}

// (device, primitive, start vertex, vertex count).
REX_HOOK_RAW(D3DDevice_DrawVertices) {
  if (Tracing())
    TraceCall("Draw", ctx, std::format(" prim={} n={}{}", ctx.r4.u32, ctx.r6.u32,
                                       DescribeBuffers(ctx.r3.u32)));
  __imp__D3DDevice_DrawVertices(ctx, base);
}

// (device, primitive, base vertex, start index, index count).
REX_HOOK_RAW(D3DDevice_DrawIndexedVertices) {
  if (Tracing())
    TraceCall("DrawIndexed", ctx,
              std::format(" prim={} base={} start={} n={}{}", ctx.r4.u32, ctx.r5.s32,
                          ctx.r6.u32, ctx.r7.u32, DescribeBuffers(ctx.r3.u32)));
  __imp__D3DDevice_DrawIndexedVertices(ctx, base);
}

// (device, primitive, vertex count, stride) -> vertex data.
REX_HOOK_RAW(D3DDevice_BeginVertices) {
  if (Tracing())
    TraceCall("BeginVertices", ctx,
              std::format(" prim={} n={} stride={}", ctx.r4.u32, ctx.r5.u32, ctx.r6.u32));
  __imp__D3DDevice_BeginVertices(ctx, base);
}

// (device, rect count, rects, flags, colour, z in f1, stencil).
REX_HOOK_RAW(D3DDevice_Clear) {
  if (Tracing())
    TraceCall("Clear", ctx, std::format(" flags=0x{:X} colour={:08X} z={:.4g} stencil={}",
                                        ctx.r6.u32, ctx.r7.u32, ctx.f1.f64, ctx.r9.u32));
  __imp__D3DDevice_Clear(ctx, base);
}

// (device, flags, tile count, tile D3DRECTs, clear colour, z in f1, stencil).
REX_HOOK_RAW(D3DDevice_BeginTiling) {
  if (Tracing()) {
    std::string rects;
    const auto *r = pgr4::mem::at<const be_i32>(ctx.r6.u32);
    for (u32 i = 0; r && i < std::min<u32>(ctx.r5.u32, 8); ++i)
      rects += std::format(" ({},{})-({},{})", i32(r[i * 4]), i32(r[i * 4 + 1]),
                           i32(r[i * 4 + 2]), i32(r[i * 4 + 3]));
    TraceCall("BeginTiling", ctx,
              std::format(" flags=0x{:X} tiles={}{}", ctx.r4.u32, ctx.r5.u32, rects));
  }
  __imp__D3DDevice_BeginTiling(ctx, base);
}

REX_HOOK_RAW(D3DDevice_EndTiling) {
  if (Tracing())
    TraceCall("EndTiling", ctx,
              std::format(" r4=0x{:X} r5=0x{:X} r6=0x{:08X} r7=0x{:08X}", ctx.r4.u32,
                          ctx.r5.u32, ctx.r6.u32, ctx.r7.u32));
  __imp__D3DDevice_EndTiling(ctx, base);
}

// The Z pass replays the draws between Begin and End on the GPU with colour
// off; the draws still pass through the hooks above once.
REX_HOOK_RAW(D3DDevice_BeginZPass) {
  PGR4_WARN_FIRST(4, "[xtrace] BeginZPass flags=0x{:X} lr=0x{:08X} frame={}", ctx.r4.u32,
                  u32(ctx.lr), g_frame + 1);
  if (Tracing())
    TraceCall("BeginZPass", ctx, std::format(" flags=0x{:X}", ctx.r4.u32));
  __imp__D3DDevice_BeginZPass(ctx, base);
}

REX_HOOK_RAW(D3DDevice_EndZPass) {
  if (Tracing())
    TraceCall("EndZPass", ctx, std::string());
  __imp__D3DDevice_EndZPass(ctx, base);
}

// A command buffer's draws reach the GPU without passing the draw hooks.
// PGR4's only caller is the vehicle renderer, which should not get here while
// Vehicle.DisableCommandBuffers is set.
REX_HOOK_RAW(D3DDevice_RunCommandBuffer) {
  const auto *disable = pgr4::mem::at<const u8>(pgr4::gpu::kVehicleDisableCommandBuffersGuestAddr);
  PGR4_WARN_FIRST(8, "[xtrace] RunCommandBuffer 0x{:08X} lr=0x{:08X} frame={} "
                     "Vehicle.DisableCommandBuffers={}",
                  ctx.r4.u32, u32(ctx.lr), g_frame + 1, disable ? u32(*disable) : 0xFFu);
  __imp__D3DDevice_RunCommandBuffer(ctx, base);
}

// (adapter, type, window, behaviour, params, out device).
REX_HOOK_RAW(Direct3D_CreateDevice) {
  const u32 type = ctx.r4.u32, behaviour = ctx.r6.u32, out = ctx.r8.u32, lr = u32(ctx.lr);
  __imp__Direct3D_CreateDevice(ctx, base);
  const auto *slot = out ? pgr4::mem::at<const be_u32>(out) : nullptr;
  const u32 device = slot ? u32(*slot) : 0;
  PGR4_INFO("[xtrace] Direct3D_CreateDevice type={} behaviour=0x{:X} -> 0x{:08X} hr=0x{:08X} "
            "lr=0x{:08X}",
            type, behaviour, device, ctx.r3.u32, lr);
}

// (device, flags, source rect, dest texture, dest point, level, slice, clear
// colour, z in f1, ...), as in 2099.
REX_HOOK_RAW(D3DDevice_Resolve) {
  if (Tracing())
    TraceCall("Resolve", ctx,
              std::format(" flags=0x{:X} dest=0x{:08X}{} slice={}", ctx.r4.u32, ctx.r6.u32,
                          DescribeTexture(ctx.r6.u32), ctx.r9.u32));
  const u32 device = ctx.r3.u32;
  const u32 dest = ctx.r6.u32;
  const u32 slice = ctx.r9.u32;
  bool wanted;
  u32 frame, seq;
  {
    std::lock_guard lock(g_mutex);
    ParseWanted();
    frame = g_frame + 1;
    seq = g_resolves++;
    wanted = dest && std::find(g_wanted.begin(), g_wanted.end(), frame) != g_wanted.end();
  }
  __imp__D3DDevice_Resolve(ctx, base);
  if (!wanted)
    return;
  // Resolve returns nothing, so the volatile registers are free to reuse.
  ctx.r3.u64 = device;
  __imp__D3DDevice_InsertFence(ctx, base);
  __imp__D3DDevice_BlockOnFence(ctx, base); // takes the fence InsertFence returned in r3
  std::filesystem::path dir = rex::Runtime::instance()
                                  ? rex::Runtime::instance()->user_data_root()
                                  : std::filesystem::path(".");
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  DumpTexture(frame, seq, dest, slice, dir);
}

// Called by input_script.cpp's Swap wrapper once per present.
void Pgr4XenosDumpOnPresent() {
  std::lock_guard lock(g_mutex);
  ++g_frame;
  g_resolves = 0;
}
