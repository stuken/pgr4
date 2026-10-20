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
// Ported from pgr3, with its endian, tiled-offset and FP16 fixes. pgr3's
// pgr3_trace_frame lives here too; it waits for Phase 4 to name the draw and
// state functions it hooks in XDK 6132.

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

REXCVAR_DEFINE_STRING(pgr4_dump_frames, "", kCvarGroup,
                      "Comma-separated present frame numbers whose resolve "
                      "targets are written as BMPs in the user data folder");

REXCVAR_DEFINE_BOOL(pgr4_dump_raw, false, kCvarGroup,
                    "With pgr4_dump_frames, also write each resolve target's "
                    "memory (from 64 KiB before its base) as a .bin");

REX_EXTERN(__imp__D3DDevice_Resolve);
REX_EXTERN(__imp__D3DDevice_InsertFence);
REX_EXTERN(__imp__D3DDevice_BlockOnFence);

namespace {

namespace tu = rex::graphics::texture_util;

// D3DBaseTexture: XDK 6132's resource header is larger than 2099's, and the
// fetch constant sits at +0x1C (2099: +0x10). D3DDevice_Resolve reads the
// destination's pitch from +0x1C, base and format from +0x20, size from +0x24
// and dimension from +0x30.
constexpr u32 kTextureFetchOffset = 0x1C;

std::mutex g_mutex;
std::vector<u32> g_wanted;
bool g_parsed = false;
u32 g_frame = 0;    // presents completed
u32 g_resolves = 0; // resolves so far in the current present

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

// (device, flags, source rect, dest texture, dest point, level, slice, clear
// colour, z in f1, ...), as in 2099.
REX_HOOK_RAW(D3DDevice_Resolve) {
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
