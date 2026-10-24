// pgr4 - ReXGlue Recompiled Project

#include "gpu/gpu_timing.h"

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

#include <plume_render_interface.h>
#if defined(PGR4_D3D12)
#include <plume_d3d12.h>
#endif

#include <rex/cvar.h>

#include "core/settings.h"
#include "gpu/device.h"
#include "gpu/perf_stats.h"

REXCVAR_DEFINE_BOOL(pgr4_perf_gpu, false, kCvarGroup,
                    "With pgr4_perf_stats, time each pass on the GPU and "
                    "report the heaviest");

namespace pgr4::gpu::perf {

namespace {

// Marks per frame. A race frame has a few hundred framebuffer binds; past
// the cap the rest of the frame goes to the last mark.
constexpr u32 kMarks = 8192;

struct Label {
  const char *what = "";
  u32 width = 0;
  u32 height = 0;
  u32 format = 0;
  u32 samples = 0;
  auto key() const { return std::tie(what, width, height, format, samples); }
  bool operator<(const Label &o) const { return key() < o.key(); }
};

struct Slot {
  std::unique_ptr<plume::RenderQueryPool> pool;
  std::vector<Label> marks;
  bool ended = false;
  bool pending = false;
};
Slot g_slots[kNumFrames];

struct Total {
  u64 ns = 0;
  u64 count = 0;
};
// Touched only on the drawing thread, under the video state mutex.
std::map<Label, Total> g_totals;
u64 g_frame_ns = 0;
u64 g_frames = 0;
// The GPU's idle time between one timed list's end and the next one's start.
u64 g_gap_ns = 0;
u64 g_last_end = 0;

const char *FormatName(plume::RenderFormat f) {
  using F = plume::RenderFormat;
  switch (f) {
  case F::R16G16B16A16_FLOAT: return "rgba16f";
  case F::R8G8B8A8_UNORM: return "rgba8";
  case F::B8G8R8A8_UNORM: return "bgra8";
  case F::R32_FLOAT: return "r32f";
  case F::R16G16_FLOAT: return "rg16f";
  case F::D32_FLOAT: return "d32";
  case F::D32_FLOAT_S8_UINT: return "d32s8";
  default: return nullptr;
  }
}

// plume's D3D12 writeTimestamp resolves each query as it goes, a copy on the
// queue per mark, and a frame can have over a thousand marks. On D3D12 the
// queries are ended here and resolved in one call at the end of the list.
void Timestamp(VideoState &s, Slot &slot, u32 index) {
#if defined(PGR4_D3D12)
  auto *list = static_cast<plume::D3D12CommandList *>(s.command_list);
  auto *pool = static_cast<plume::D3D12QueryPool *>(slot.pool.get());
  list->d3d->EndQuery(pool->d3d, D3D12_QUERY_TYPE_TIMESTAMP, index);
#else
  s.command_list->writeTimestamp(slot.pool.get(), index);
#endif
}

void Write(VideoState &s, Slot &slot, const Label &label) {
  const u32 index = u32(slot.marks.size());
  if (index >= kMarks)
    return;
  Timestamp(s, slot, index);
  slot.marks.push_back(label);
}

} // namespace

bool GpuTimingEnabled() { return Enabled() && REXCVAR_GET(pgr4_perf_gpu); }

void GpuFrameBegin(VideoState &s) {
  Slot &slot = g_slots[s.frame.load(std::memory_order_relaxed)];
  slot.marks.clear();
  slot.ended = false;
  slot.pending = false;
  if (!GpuTimingEnabled() || !s.command_list)
    return;
  if (!slot.pool) {
    slot.pool = s.device->createQueryPool(kMarks + 1);
    if (!slot.pool)
      return;
  }
  s.command_list->resetQueryPool(slot.pool.get(), 0, kMarks + 1);
  slot.pending = true;
  Write(s, slot, {"(list start)"});
}

void GpuMark(VideoState &s, const char *what, const GuestTexture *target) {
  Slot &slot = g_slots[s.frame.load(std::memory_order_relaxed)];
  if (!slot.pending || slot.ended || !s.command_list)
    return;
  Label label{what};
  if (target) {
    label.width = target->width;
    label.height = target->height;
    label.format = u32(target->format);
    label.samples = u32(target->sampleCount);
  }
  // Repeated binds of one target in a row are one pass.
  if (!slot.marks.empty() && !(slot.marks.back() < label) &&
      !(label < slot.marks.back()))
    return;
  Write(s, slot, label);
}

void GpuFrameEnd(VideoState &s) {
  Slot &slot = g_slots[s.frame.load(std::memory_order_relaxed)];
  if (!slot.pending || slot.ended || !s.command_list)
    return;
  // Written past the cap too, so the frame total stays whole.
  const u32 index = std::min(u32(slot.marks.size()), kMarks);
  Timestamp(s, slot, index);
  slot.marks.resize(index);
  slot.marks.push_back({"(end)"});
  slot.ended = true;
#if defined(PGR4_D3D12)
  auto *list = static_cast<plume::D3D12CommandList *>(s.command_list);
  auto *pool = static_cast<plume::D3D12QueryPool *>(slot.pool.get());
  list->d3d->ResolveQueryData(
      pool->d3d, D3D12_QUERY_TYPE_TIMESTAMP, 0, index + 1,
      static_cast<plume::D3D12Buffer *>(pool->readbackBuffer.get())->d3d, 0);
#endif
}

void GpuCollect(u32 slot_index) {
  Slot &slot = g_slots[slot_index];
  if (!slot.pending || !slot.ended || slot.marks.size() < 2) {
    slot.pending = false;
    return;
  }
  slot.pending = false;
  slot.pool->queryResults();
  const u64 *t = slot.pool->getResults();
  const u32 n = std::min(u32(slot.marks.size()), kMarks + 1);
  for (u32 i = 0; i + 1 < n; ++i) {
    const u64 dt = t[i + 1] >= t[i] ? t[i + 1] - t[i] : 0;
    Total &total = g_totals[slot.marks[i]];
    total.ns += dt;
    ++total.count;
  }
  if (t[n - 1] >= t[0])
    g_frame_ns += t[n - 1] - t[0];
  if (g_last_end && t[0] > g_last_end)
    g_gap_ns += t[0] - g_last_end;
  g_last_end = t[n - 1];
  ++g_frames;
}

std::string GpuReport(u64 frames) {
  if (!g_frames || !frames) {
    g_totals.clear();
    return {};
  }
  std::vector<std::pair<Label, Total>> sorted(g_totals.begin(), g_totals.end());
  std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
    return a.second.ns > b.second.ns;
  });
  const double per = 1e6 * double(g_frames);
  std::string out =
      std::format("GPU {:.2f} ms/frame over {} frames, idle {:.2f} ms/frame "
                  "between lists:",
                  double(g_frame_ns) / per, g_frames, double(g_gap_ns) / per);
  constexpr size_t kShown = 24;
  for (size_t i = 0; i < sorted.size() && i < kShown; ++i) {
    const auto &[label, total] = sorted[i];
    std::string target;
    if (label.width) {
      const char *name = FormatName(plume::RenderFormat(label.format));
      target = std::format(" {}x{} {}{}", label.width, label.height,
                           name ? name : std::format("fmt{}", label.format),
                           label.samples > 1 ? std::format(" {}x", label.samples)
                                             : std::string());
    }
    out += std::format("\n    {:7.3f} ms {:7.1f} x  {}{}",
                       double(total.ns) / per,
                       double(total.count) / double(g_frames), label.what,
                       target);
  }
  g_totals.clear();
  g_frame_ns = 0;
  g_gap_ns = 0;
  g_frames = 0;
  return out;
}

std::string MemoryReport() {
  constexpr double kMiB = 1024.0 * 1024.0;
  auto &s = state();
  if (!s.device)
    return {};
#if defined(PGR4_D3D12)
  auto *dev = static_cast<plume::D3D12Device *>(s.device.get());
  IDXGIAdapter3 *adapter = nullptr;
  if (!dev->adapter || FAILED(dev->adapter->QueryInterface(IID_PPV_ARGS(&adapter))))
    return {};
  DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
  adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
  adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
  adapter->Release();
  return std::format("video memory: local {:.0f} of {:.0f} MiB budget, "
                     "non-local {:.0f} of {:.0f} MiB",
                     double(local.CurrentUsage) / kMiB, double(local.Budget) / kMiB,
                     double(shared.CurrentUsage) / kMiB,
                     double(shared.Budget) / kMiB);
#else
  const auto m = Video::MemoryUsage();
  return std::format("video memory: device-local {:.0f} of {:.0f} MiB budget",
                     double(m.used) / kMiB, double(m.budget) / kMiB);
#endif
}

} // namespace pgr4::gpu::perf
