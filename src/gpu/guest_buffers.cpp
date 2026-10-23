// pgr4 - ReXGlue Recompiled Project

#include "gpu/guest_buffers.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <xxhash.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/byte_swap.h"
#include "gpu/device.h"
#include "gpu/frame.h"
#include "gpu/resources.h"
#include "gpu/perf_stats.h"

namespace pgr4::gpu {

namespace {

struct Mirror {
  std::unique_ptr<GuestBuffer> buffer;
  u64 hash = 0;
  u64 sparse = 0;     // SparseHash at the last full hash
  u32 stable = 0;     // full hashes in a row that matched
  u64 verified_frame = ~0ull;
};

// A buffer whose full hash has matched this many frames running is cold: it
// is fully rehashed only every kColdPeriod frames (staggered by address), and
// checked in between by a sparse hash. Buffers the CPU rewrites stay hot and
// are fully hashed every frame. A small write to a long-static buffer shows
// within kColdPeriod frames.
constexpr u32 kColdAfter = 60;
constexpr u32 kColdPeriod = 8;

// 16 16-byte samples spread over the buffer: each is a likely cache miss in
// guest memory, and the full rehash every kColdPeriod frames backs them up.
u64 SparseHash(const u8 *src, u32 size) {
  constexpr u32 kSamples = 16, kSampleBytes = 16;
  if (size <= kSamples * kSampleBytes)
    return XXH3_64bits(src, size);
  u8 samples[kSamples * kSampleBytes];
  for (u32 i = 0; i < kSamples; ++i) {
    const u32 off = u32(u64(size - kSampleBytes) * i / (kSamples - 1));
    std::memcpy(samples + i * kSampleBytes, src + off, kSampleBytes);
  }
  return XXH3_64bits(samples, sizeof(samples));
}

std::mutex g_mutex;
std::unordered_map<u64, Mirror> g_mirrors; // (base_va, type) -> mirror
std::vector<std::unique_ptr<plume::RenderBuffer>> g_graveyard[kNumFrames];
// Evicted mirrors keep their GuestBuffer alive until the slot drains too, since
// Video's index binding holds the GuestBuffer pointer itself.
std::vector<std::unique_ptr<GuestBuffer>> g_dead_mirrors[kNumFrames];
// Bumped whenever a mirror is re-uploaded or evicted, so the drawing
// thread's lookup cache (MirrorGuestBuffer) never returns one that changed.
std::atomic<u64> g_buffer_generation{1};

// Atomic so GuestMirrorFrame reads it without the mirror lock.
std::atomic<u64> g_frame{0};

u64 Key(u32 base_va, ResourceType type) {
  return (u64(static_cast<u32>(type)) << 32) | base_va;
}

std::unique_ptr<plume::RenderBuffer> Upload(plume::RenderDevice *device,
                                            const u8 *src, u32 size,
                                            bool is_index, u32 element_size) {
  const plume::RenderHeapType heap =
      GeometryHeapType(device, GeometryClass::Static);
#if defined(PGR4_D3D12)
  constexpr plume::RenderBufferFlags kVertexFlags = plume::RenderBufferFlag::NONE;
#else
  // Pulled vertex shaders read vertex streams through device addresses, which
  // plume enables for constant-flagged buffers.
  constexpr plume::RenderBufferFlags kVertexFlags =
      plume::RenderBufferFlag::CONSTANT |
      plume::RenderBufferFlag::DEVICE_ADDRESSABLE;
#endif
  const plume::RenderBufferDesc desc =
      is_index ? plume::RenderBufferDesc::IndexBuffer(size, heap)
               : plume::RenderBufferDesc::VertexBuffer(size, heap, kVertexFlags);
  auto buffer = CreateHostBuffer(device, desc,
                                 is_index ? "guest-ib" : "guest-vb");
  if (!buffer)
    return nullptr;
  void *mapped = buffer->map();
  if (!mapped)
    return nullptr;
  ByteSwapElements(mapped, src, size, element_size);
  buffer->unmap();
  return buffer;
}

} // namespace

GuestBuffer *MirrorGuestBuffer(u32 base_va, u32 size, ResourceType type,
                               u32 element_size, plume::RenderFormat format) {
  if (!base_va || !size)
    return nullptr;
  const auto *src = pgr4::mem::at<const u8>(base_va);
  auto *device = Video::HostDevice();
  if (!src || !device)
    return nullptr;

  // Draws look the same few buffers up over and over: a buffer already
  // verified this frame, with no upload or eviction since, is returned from a
  // per-thread cache without the lock.
  struct CacheEntry {
    u64 key = 0;
    u32 size = 0;
    plume::RenderFormat format{};
    u64 frame = ~0ull;
    u64 generation = 0;
    GuestBuffer *buffer = nullptr;
  };
  thread_local CacheEntry t_cache[256];
  const u64 key = Key(base_va, type);
  CacheEntry &e = t_cache[(base_va >> 4 ^ base_va >> 12) & 255];
  const u64 generation = g_buffer_generation.load(std::memory_order_acquire);
  if (e.key == key && e.size == size && e.format == format &&
      e.frame == g_frame.load(std::memory_order_relaxed) &&
      e.generation == generation)
    return e.buffer;

  std::lock_guard lock(g_mutex);
  Mirror &m = g_mirrors[key];
  auto remember = [&](GuestBuffer *b) {
    e = {key, size, format, g_frame.load(std::memory_order_relaxed),
         g_buffer_generation.load(std::memory_order_acquire), b};
    return b;
  };
  const bool same_shape =
      m.buffer && m.buffer->dataSize == size && m.buffer->format == format;
  const u64 frame = g_frame.load(std::memory_order_relaxed);
  if (same_shape && m.verified_frame == frame)
    return remember(m.buffer.get());

  if (same_shape && m.stable >= kColdAfter &&
      (frame + (base_va >> 4)) % kColdPeriod != 0) {
    u64 sparse;
    {
      perf::Scope timer(perf::Counter::BufferHash, 16 * 16);
      sparse = SparseHash(src, size);
    }
    if (sparse == m.sparse) {
      m.verified_frame = frame;
      return remember(m.buffer.get());
    }
  }

  u64 hash;
  {
    perf::Scope timer(perf::Counter::BufferHash, size);
    hash = XXH3_64bits(src, size);
  }
  if (same_shape && hash == m.hash) {
    if (m.stable < kColdAfter) {
      ++m.stable;
      if (m.stable == kColdAfter)
        m.sparse = SparseHash(src, size);
    }
    m.verified_frame = frame;
    return remember(m.buffer.get());
  }
  m.stable = 0;

  std::unique_ptr<plume::RenderBuffer> fresh;
  {
    perf::Scope timer(perf::Counter::BufferUpload, size);
    fresh = Upload(device, src, size, type == ResourceType::IndexBuffer,
                   element_size);
  }
  if (!fresh) {
    PGR4_ERROR("MirrorGuestBuffer: upload failed (0x{:08X}, {} bytes)", base_va,
              size);
    return m.buffer.get(); // stale beats nothing
  }
  if (!m.buffer) {
    m.buffer = std::make_unique<GuestBuffer>(type);
  } else if (m.buffer->buffer) {
    // Bound views may still name the old buffer in the recording list.
    Video::ScrubBufferBindings(m.buffer->buffer.get());
    g_graveyard[Video::RetireSlot("guest buffer mirror")].push_back(
        std::move(m.buffer->buffer));
  }
  m.buffer->buffer = std::move(fresh);
  m.buffer->guestMirrorVa = base_va;
  m.buffer->dataSize = size;
  m.buffer->format = format;
  m.hash = hash;
  m.verified_frame = frame;
  g_buffer_generation.fetch_add(1, std::memory_order_acq_rel);
  return remember(m.buffer.get());
}

bool EvictGuestBuffer(u32 base_va, ResourceType type) {
  g_buffer_generation.fetch_add(1, std::memory_order_acq_rel);
  std::unique_ptr<GuestBuffer> dead;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_mirrors.find(Key(base_va, type));
    if (it == g_mirrors.end())
      return false;
    dead = std::move(it->second.buffer);
    g_mirrors.erase(it);
  }
  if (!dead)
    return true;
  Video::ScrubBufferBindings(dead->buffer.get());
  Video::ForgetIndices(dead.get());
  std::lock_guard lock(g_mutex);
  g_dead_mirrors[Video::RetireSlot("evicted guest buffer")].push_back(
      std::move(dead));
  return true;
}

u32 EvictGuestBuffersInRange(u32 begin, u32 end) {
  g_buffer_generation.fetch_add(1, std::memory_order_acq_rel);
  std::vector<std::pair<u32, ResourceType>> dead;
  {
    std::lock_guard lock(g_mutex);
    for (const auto &[key, m] : g_mirrors) {
      const u32 base = u32(key);
      if (base >= begin && base < end)
        dead.emplace_back(base, static_cast<ResourceType>(key >> 32));
    }
  }
  for (const auto &[base, type] : dead)
    EvictGuestBuffer(base, type);
  return u32(dead.size());
}

void AdvanceGuestBufferFrame() {
  std::lock_guard lock(g_mutex);
  ++g_frame;
}

u64 GuestMirrorFrame() { return g_frame.load(std::memory_order_relaxed); }

void DrainGuestBufferMirrors(u32 slot) {
  std::vector<std::unique_ptr<plume::RenderBuffer>> dead;
  std::vector<std::unique_ptr<GuestBuffer>> dead_mirrors;
  {
    std::lock_guard lock(g_mutex);
    dead.swap(g_graveyard[slot]);
    dead_mirrors.swap(g_dead_mirrors[slot]);
  }
}

} // namespace pgr4::gpu
