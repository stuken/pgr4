// pgr4 - ReXGlue Recompiled Project

#include "gpu/occlusion.h"

#include <rex/cvar.h>

#include "core/settings.h"

REXCVAR_DEFINE_BOOL(pgr4_occlusion_queries, true, kCvarGroup,
                    "Answer the game's occlusion queries from host occlusion "
                    "queries (D3D12 only); off answers every query visible");

#if defined(PGR4_D3D12)

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <plume_d3d12.h>
#include <plume_render_interface.h>

#include "core/logging.h"
#include "gpu/device.h"

namespace pgr4::gpu::occlusion {

namespace {

// Host queries per frame. A race frame issues a few dozen; past the cap a
// guest query's later draws go uncounted.
constexpr u32 kQueries = 1024;

struct Issued {
  u32 query = 0;
  u64 generation = 0;
  u32 first = 0; // host query index
  u32 count = 0;
};

struct Slot {
  ID3D12QueryHeap *heap = nullptr;
  std::unique_ptr<plume::RenderBuffer> readback;
  bool created = false;
  bool recording = false; // FrameBegin ran for this list
  u32 used = 0;           // host queries written this frame
  u32 resolved = 0;       // of which FrameEnd resolved
  std::vector<Issued> issued;
};

struct Answer {
  u64 generation = 0;
  bool ready = true;
  u32 samples = 0;
};

// Guards everything below. Taken under the video state mutex on the drawing
// side, alone on the guest's Issue and GetData.
std::mutex g_mutex;
Slot g_slots[kNumFrames];
std::unordered_map<u32, Answer> g_answers;
u64 g_generation = 0;
// The guest query between BEGIN and END, if any, and its host queries so far.
bool g_counting = false;
Issued g_open;
// The host query open around the current draw.
bool g_in_draw = false;
// Answers since the last log line, and how many passed any samples.
u64 g_collects = 0;
u64 g_answered = 0;
u64 g_passed = 0;

bool Create(VideoState &s, Slot &slot) {
  slot.created = true;
  auto *dev = static_cast<plume::D3D12Device *>(s.device.get());
  D3D12_QUERY_HEAP_DESC desc{};
  desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  desc.Count = kQueries;
  if (FAILED(dev->d3d->CreateQueryHeap(&desc, IID_PPV_ARGS(&slot.heap)))) {
    PGR4_ERROR("occlusion: CreateQueryHeap failed");
    slot.heap = nullptr;
    return false;
  }
  slot.readback = s.device->createBuffer(
      plume::RenderBufferDesc::ReadbackBuffer(sizeof(u64) * kQueries));
  return slot.readback != nullptr;
}

Slot &Recording(VideoState &s) {
  return g_slots[s.frame.load(std::memory_order_relaxed)];
}

ID3D12GraphicsCommandList *List(VideoState &s) {
  return static_cast<plume::D3D12CommandList *>(s.command_list)->d3d;
}

} // namespace

bool Enabled() { return REXCVAR_GET(pgr4_occlusion_queries); }

void Issue(u32 query, bool begin, bool end) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (begin) {
    g_counting = true;
    g_open = {query, ++g_generation, 0, 0};
    // Until a list carrying it completes, the query is in flight.
    g_answers[query] = {g_open.generation, false, 0};
  }
  if (end && g_counting && g_open.query == query) {
    g_counting = false;
    // The slot the END lands in is the one whose fence settles the answer.
    // A query with no draws answers 0 at that fence.
    auto &s = state();
    g_slots[s.frame.load(std::memory_order_relaxed)].issued.push_back(g_open);
  }
}

void DrawBegin(VideoState &s) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_counting || !s.command_list)
    return;
  Slot &slot = Recording(s);
  if (!slot.recording || slot.used >= kQueries)
    return;
  const u32 index = slot.used++;
  // Each guest query's host queries are consecutive: nothing else begins one
  // while it counts.
  if (!g_open.count)
    g_open.first = index;
  ++g_open.count;
  g_in_draw = true;
  List(s)->BeginQuery(slot.heap, D3D12_QUERY_TYPE_OCCLUSION, index);
}

void DrawEnd(VideoState &s) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_in_draw)
    return;
  g_in_draw = false;
  List(s)->EndQuery(Recording(s).heap, D3D12_QUERY_TYPE_OCCLUSION,
                    Recording(s).used - 1);
}

void FrameBegin(VideoState &s) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Slot &slot = Recording(s);
  slot.recording = false;
  slot.used = 0;
  slot.resolved = 0;
  if (!Enabled() || !s.command_list)
    return;
  if (!slot.created && !Create(s, slot))
    return;
  slot.recording = slot.heap && slot.readback;
}

void FrameEnd(VideoState &s) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Slot &slot = Recording(s);
  if (!slot.recording || !s.command_list)
    return;
  slot.recording = false;
  slot.resolved = slot.used;
  if (slot.used)
    List(s)->ResolveQueryData(
        slot.heap, D3D12_QUERY_TYPE_OCCLUSION, 0, slot.used,
        static_cast<plume::D3D12Buffer *>(slot.readback.get())->d3d, 0);
}

void Collect(u32 slot_index, bool executed) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Slot &slot = g_slots[slot_index];
  std::vector<u64> counts(slot.resolved);
  if (executed && slot.resolved) {
    if (void *data = slot.readback->map()) {
      memcpy(counts.data(), data, sizeof(u64) * slot.resolved);
      slot.readback->unmap();
    }
  }
  for (const Issued &q : slot.issued) {
    auto it = g_answers.find(q.query);
    if (it == g_answers.end() || it->second.generation != q.generation)
      continue; // issued again since
    u64 samples = 0;
    for (u32 i = q.first; i < q.first + q.count && i < slot.resolved; ++i)
      samples += counts[i];
    it->second.ready = true;
    it->second.samples = u32(std::min<u64>(samples, 0xFFFFFFFFu));
    ++g_answered;
    g_passed += samples != 0;
  }
  slot.issued.clear();
  slot.resolved = 0;
  if (++g_collects % 1800 == 0 && g_answered) {
    PGR4_INFO("occlusion: {} queries answered over {} frames, {} passed samples",
              g_answered, 1800, g_passed);
    g_answered = 0;
    g_passed = 0;
  }
}

bool Result(u32 query, u32 &samples) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_answers.find(query);
  if (it == g_answers.end()) {
    samples = 0;
    return true;
  }
  samples = it->second.samples;
  return it->second.ready;
}

} // namespace pgr4::gpu::occlusion

#else

// Vulkan: no query ever becomes available on the Radeon 8060S driver, plume's
// timestamps included, even after vkDeviceWaitIdle (Phase 6 findings). Until
// that is understood every query answers visible, as before.
namespace pgr4::gpu::occlusion {

bool Enabled() { return false; }
void Issue(u32, bool, bool) {}
void DrawBegin(VideoState &) {}
void DrawEnd(VideoState &) {}
void FrameBegin(VideoState &) {}
void FrameEnd(VideoState &) {}
void Collect(u32, bool) {}
bool Result(u32, u32 &samples) {
  samples = 0;
  return true;
}

} // namespace pgr4::gpu::occlusion

#endif
