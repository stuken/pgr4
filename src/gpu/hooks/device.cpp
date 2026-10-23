// pgr4 - ReXGlue Recompiled Project
//
// Device lifecycle hooks for the native renderer: Direct3D_CreateDevice builds
// the guest D3DDevice while plume owns the real swapchain, Clear and Swap
// drive the host frame, and D3D__MakeSpace turns the guest command ring into a
// discard buffer. Modelled on reblue's gpu/hooks/device.cpp (Copyright (c) 2026
// Tom Clay, BSD 3-Clause) by way of rr6 and pgr3, against XDK 2.0.6132's
// layout (gpu/d3d.h, config/d3d.toml).

#include <atomic>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/xmemory.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/native_texture_mirror.h"

REXCVAR_DECLARE(std::string, pgr4_dump_frames);
REXCVAR_DECLARE(int32_t, pgr4_trace_frame);
REXCVAR_DECLARE(std::string, pgr4_input_script);
REXCVAR_DECLARE(int32_t, pgr4_perf_stats);

namespace pgr4::gpu {
void NoteDefaultSurfaces(u32 back_buffer, u32 depth);
void AdvanceGuestBufferFrame();
void LogDrawStats(u32 frame);
bool TraceFrameActive();
u32 BoundRenderTargetVa();
void ClearEdramAliases(u32 cleared_va);
} // namespace pgr4::gpu

namespace {

// Whether a debugging tool that names present frames is on.
bool FrameNumbersWanted() {
  return !REXCVAR_GET(pgr4_dump_frames).empty() ||
         REXCVAR_GET(pgr4_trace_frame) > 0 ||
         !REXCVAR_GET(pgr4_input_script).empty() ||
         REXCVAR_GET(pgr4_perf_stats) > 0;
}

// Nothing consumes the guest's PM4 ring: every guest D3D function still
// running unhooked writes its packets here and D3D__MakeSpace rewinds them.
// The limit leaves half the buffer as headroom for the largest single batch
// written between two limit checks.
constexpr u32 kDiscardRingSize = 4u << 20;
constexpr u32 kDiscardRingLimit = kDiscardRingSize / 2;
u32 g_discard_ring = 0;

// Command memory D3D__AllocCommandMemory hands out (inline vertex data,
// clear and resolve constants), bump-allocated and wrapped. The guest only
// writes it, so reuse after a wrap is harmless while nothing reads it back.
constexpr u32 kDiscardCommandSize = 8u << 20;
u32 g_discard_command = 0;
u32 g_discard_command_used = 0;

REX_IMPORT(__imp__D3D__InitializeStateTables, InitializeStateTables,
           void(u32));
REX_IMPORT(__imp__D3DDevice_CreateSurface, CreateSurface,
           u32(u32, u32, u32, u32, u32));
REX_IMPORT(__imp__D3DDevice_SetRenderTarget, SetRenderTarget,
           void(u32, u32, u32));
REX_IMPORT(__imp__D3DDevice_SetDepthStencilSurface, SetDepthStencilSurface,
           void(u32, u32));

// D3DPRESENT_PARAMETERS, the fields the device's default surfaces need.
struct PresentParameters {
  be_u32 BackBufferWidth;        // +0x00
  be_u32 BackBufferHeight;       // +0x04
  be_u32 BackBufferFormat;       // +0x08
  be_u32 BackBufferCount;        // +0x0C
  be_u32 MultiSampleType;        // +0x10
  be_u32 MultiSampleQuality;     // +0x14
  be_u32 SwapEffect;             // +0x18
  be_u32 hDeviceWindow;          // +0x1C
  be_u32 Windowed;               // +0x20
  be_u32 EnableAutoDepthStencil; // +0x24
  be_u32 AutoDepthStencilFormat; // +0x28
};

// The real D3DDevice_Initialize binds a back buffer at RT0 (and a depth
// buffer when asked) before returning, and games read RT0 back (RR6 does
// straight away). The guest's own CreateSurface/Set* keep its bookkeeping
// consistent; the device holds the creation references.
void BindDefaultSurfaces(u32 device_va, u32 params_va) {
  const auto *pp = pgr4::mem::at<const PresentParameters>(params_va);
  if (!pp)
    return;
  const u32 width = pp->BackBufferWidth ? u32(pp->BackBufferWidth) : 1280u;
  const u32 height = pp->BackBufferHeight ? u32(pp->BackBufferHeight) : 720u;
  const u32 back = CreateSurface(width, height, pp->BackBufferFormat,
                                 pp->MultiSampleType, 0);
  if (back)
    SetRenderTarget(device_va, 0, back);
  u32 depth = 0;
  if (pp->EnableAutoDepthStencil) {
    depth = CreateSurface(width, height, pp->AutoDepthStencilFormat,
                          pp->MultiSampleType, 0);
    if (depth)
      SetDepthStencilSurface(device_va, depth);
  }
  pgr4::gpu::NoteDefaultSurfaces(back, depth);
  PGR4_INFO("Direct3D_CreateDevice: {}x{} back buffer 0x{:08X} (format "
           "0x{:08X}), depth 0x{:08X} (format 0x{:08X})",
           width, height, back, u32(pp->BackBufferFormat), depth,
           u32(pp->AutoDepthStencilFormat));
}

// Any read of the GPU's fence write-back sees 0. Nothing advances the fence
// (the ring is discarded, and 6132's InsertFence only returns the counter
// MakeSpace would bump), so every fence the guest holds is 0, which
// IsFencePending and BlockOnFence treat as done before they dereference the
// write-back pointer at +0x2A90. This backs any path that does not.
u32 g_fence_readback = 0;

// DeviceType 1 is the game's device. PGR4 also makes a command-buffer device
// (type 2) for the vehicle renderer, which only records into it while
// Vehicle.DisableCommandBuffers is clear (gpu/d3d.h); it gets a device of its
// own here but neither the default surfaces nor VdGlobalDevice.
u32 Direct3D_CreateDevice_hook(u32 /*adapter*/, u32 device_type,
                               u32 /*focus_window*/, u32 behavior_flags,
                               u32 present_params, mapped_u32 out_device) {
  const bool command_buffer = device_type == pgr4::gpu::kD3DDevTypeCommandBuffer;
  auto *memory = REX_KERNEL_MEMORY();
  const u32 device_va =
      memory->SystemHeapAlloc(pgr4::gpu::kD3DDeviceAllocSize, 0x80);
  if (!g_discard_ring)
    g_discard_ring = memory->SystemHeapAlloc(kDiscardRingSize, 0x1000);
  if (!g_discard_command)
    g_discard_command = memory->SystemHeapAlloc(kDiscardCommandSize, 0x1000);
  if (!g_fence_readback)
    g_fence_readback = memory->SystemHeapAlloc(16, 16);
  if (!device_va || !g_discard_ring || !g_discard_command || !g_fence_readback) {
    PGR4_ERROR("Direct3D_CreateDevice: guest allocation failed");
    return 0x8007000E; // E_OUTOFMEMORY, as the guest returns
  }
  memory->Zero(device_va, pgr4::gpu::kD3DDeviceAllocSize);
  memory->Zero(g_fence_readback, 16);

  auto *device = pgr4::mem::at<pgr4::gpu::D3DDevice>(device_va);
  device->m_pRing = g_discard_ring;
  device->m_pRingEnd = g_discard_ring + kDiscardRingSize;
  device->m_pRingLimit = g_discard_ring + kDiscardRingLimit;
  device->m_ReferenceCount = 1;
  device->behaviorFlags = behavior_flags;
  device->fenceReadback = g_fence_readback;
  // The query-slot pool head as D3DDevice_Initialize leaves it: full, so the
  // guest's own CreateQuery allocates a page for its first query.
  if (auto *pool = pgr4::mem::at<be_u32>(device_va + pgr4::gpu::kDeviceQuerySlotPool)) {
    pool[0] = 0xFFFFFFFFu;
    pool[1] = 0xFFFFFFFFu;
    pool[2] = 0;
    pool[3] = 0;
  }

  if (!command_buffer) {
    if (auto *global = pgr4::mem::at<const be_u32>(pgr4::gpu::kVdGlobalDeviceImport)) {
      if (auto *slot = pgr4::mem::at<be_u32>(global->get()))
        *slot = device_va;
    }
  }

  // The guest's own seeding: copies both dispatch tables in, calls every Set
  // with its default so the register shadows start valid, and clears all
  // texture slots. Its packets land in the discard ring.
  InitializeStateTables(device_va);
  if (!command_buffer)
    BindDefaultSurfaces(device_va, present_params);

  device->viewport.Width = 1280.0f;
  device->viewport.Height = 720.0f;
  device->viewport.MaxZ = 1.0f;

  if (out_device)
    *out_device = device_va;
  PGR4_INFO("Direct3D_CreateDevice: guest {}device at 0x{:08X}",
            command_buffer ? "command-buffer " : "", device_va);
  return 0;
}

// The Xenon ABI reserves a GPR slot for every float argument, so Z in f1 also
// consumes r8 while SDK marshaling numbers integer args without skipping it.
// The placeholder absorbs r8 so Stencil reads r9 (D3DDevice_Clear, 0x826D1A08,
// passes r9 on to ClearF as 2135's and 2099's do).
u32 D3DDevice_Clear_hook(u32 /*device*/, u32 count, u32 rects, u32 flags,
                         u32 color, f64 z, u32 /*z_gpr_slot*/, u32 stencil) {
  if (pgr4::gpu::TraceFrameActive())
    PGR4_INFO("[trace] Clear flags=0x{:X} color=0x{:08X} z={}", flags, color, z);
  // D3DRECT {x1,y1,x2,y2}. Rects aren't modeled: the whole target clears.
  if (count && rects) {
    if (const auto *r = pgr4::mem::at<const be_i32>(rects))
      PGR4_WARN_FIRST(4, "Clear: {} rect(s) ignored, first {},{}-{},{} "
                     "flags=0x{:X}", count, i32(r[0]), i32(r[1]), i32(r[2]),
                     i32(r[3]), flags);
  }
  pgr4::gpu::Video::Clear(flags, color, static_cast<float>(z), stencil);
  if ((flags & pgr4::gpu::kClearTarget) && color == 0)
    pgr4::gpu::ClearEdramAliases(pgr4::gpu::BoundRenderTargetVa());
  return 0;
}

// (device, flags, rect, pColor, Z in f1, stencil in r8; Z's GPR slot is r7,
// as in Clear). Clear calls it, and so do BeginTiling, BeginZPass and six
// game functions (sub_82317448, sub_824B8620, sub_824C32F0, sub_824CCC70,
// sub_824CD158, sub_824CD9A8). In PGR3 the race cleared its scene to the fog
// colour this way after the rear-view mirror had drawn into the scene's
// corner; unhooked, the mirror's corner stayed in the scene as a second
// mirror. PGR4's in-car view draws its mirror into the scene targets too
// (docs/native-renderer-plan.md, Phase 4). The flags are Clear's: targets in
// bits 0-3, depth 0x10, stencil 0x20.
u32 D3DDevice_ClearF_hook(u32 /*device*/, u32 flags, u32 rect, u32 color_va,
                          f64 z, u32 /*z_gpr_slot*/, u32 stencil) {
  plume::RenderColor color(0.0f, 0.0f, 0.0f, 0.0f);
  if (const auto *c = pgr4::mem::at<const be_f32>(color_va); c && color_va)
    color = plume::RenderColor(c[0], c[1], c[2], c[3]);
  if (pgr4::gpu::TraceFrameActive())
    PGR4_INFO("[trace] ClearF flags=0x{:X} color=({:.3g},{:.3g},{:.3g},{:.3g}) z={}",
              flags, color.r, color.g, color.b, color.a, z);
  if (rect) {
    if (const auto *r = pgr4::mem::at<const be_i32>(rect))
      PGR4_WARN_FIRST(4, "ClearF: rect ignored, {},{}-{},{} flags=0x{:X}",
                     i32(r[0]), i32(r[1]), i32(r[2]), i32(r[3]), flags);
  }
  pgr4::gpu::Video::Clear(flags, color, static_cast<float>(z), stencil);
  if ((flags & pgr4::gpu::kClearTarget) && color.r == 0.0f && color.g == 0.0f &&
      color.b == 0.0f && color.a == 0.0f)
    pgr4::gpu::ClearEdramAliases(pgr4::gpu::BoundRenderTargetVa());
  return 0;
}

// The per-frame entry. The game's frame end (sub_822B3B08) resolves the back
// buffer into the front buffer texture itself and calls Swap with it, as
// PGR3's did; 6132's D3DDevice_Present exists but only mercury calls it, and
// it ends in Swap too. So the host presents that resolve target (or the back
// buffer, when that was drawn last; Video::Present picks). Plume owns the
// swapchain. The guest's Swap would also hand D3D's deferred physical frees
// to MmFreePhysicalMemory; rr6's Present hook skipped them the same way.
std::atomic<u32> g_presents{0};

u32 D3DDevice_Swap_hook(u32 /*device*/, u32 front_buffer) {
  const u32 s_presents = g_presents.fetch_add(1, std::memory_order_relaxed) + 1;
  if (s_presents == 1)
    PGR4_INFO("D3DDevice_Swap: first frame (front buffer 0x{:08X})", front_buffer);
  if (s_presents == 60 || (s_presents % 600) == 0)
    pgr4::gpu::LogDrawStats(s_presents);
  // Places log lines on the present count pgr4_trace_frame, pgr4_dump_frames
  // and pgr4_input_script take, while one of the tools that uses it is on.
  if (s_presents % 300 == 0 && FrameNumbersWanted())
    PGR4_INFO("present {}", s_presents);
  pgr4::gpu::Video::Present(pgr4::gpu::FindResolveTarget(front_buffer));
  pgr4::gpu::AdvanceGuestBufferFrame();
  return 0;
}

// Callers compare the ring pointer against the limit and call this when a
// batch would cross it, then write from the returned pointer.
u32 D3D__MakeSpace_hook(rex::MappedPtr<pgr4::gpu::D3DDevice> device) {
  if (device) {
    device->m_pRing = g_discard_ring;
    device->m_pRingEnd = g_discard_ring + kDiscardRingSize;
  }
  return g_discard_ring;
}

// The real allocator asks the ring-segment bookkeeping D3DDevice_Initialize
// sets up (+0x34BC) for space in the next segment. With no segments to manage,
// hand out discard memory with the requested alignment instead. 6132's
// signature is still (device, dwords, align): MakeSpace calls it with (4, 4).
u32 D3D__AllocCommandMemory_hook(u32 /*device*/, u32 dwords, u32 align) {
  const u32 bytes = dwords * 4;
  const u32 alignment = align ? align : 4;
  if (bytes > kDiscardCommandSize / 2) {
    PGR4_ERROR("D3D__AllocCommandMemory: {} bytes exceeds the discard buffer",
              bytes);
    return 0;
  }
  u32 offset = (g_discard_command_used + alignment - 1) & ~(alignment - 1);
  if (offset + bytes > kDiscardCommandSize)
    offset = 0;
  g_discard_command_used = offset + bytes;
  return g_discard_command + offset;
}

} // namespace

namespace pgr4::gpu {

// Frames the guest has presented, for tools that script against frame
// numbers (platform/input_script.cpp).
u32 PresentCount() { return g_presents.load(std::memory_order_relaxed); }

} // namespace pgr4::gpu

REX_HOOK(D3D__AllocCommandMemory, D3D__AllocCommandMemory_hook);
REX_HOOK(Direct3D_CreateDevice, Direct3D_CreateDevice_hook);
REX_HOOK(D3DDevice_Clear, D3DDevice_Clear_hook);
REX_HOOK(D3DDevice_ClearF, D3DDevice_ClearF_hook);
REX_HOOK(D3DDevice_Swap, D3DDevice_Swap_hook);
REX_HOOK(D3D__MakeSpace, D3D__MakeSpace_hook);
