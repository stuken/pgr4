// pgr4 - ReXGlue Recompiled Project
//
// Resource lifetime for the native renderer. The guest's D3D still creates its
// own buffers, textures and surfaces, and the host mirrors them by guest address
// (gpu/guest_buffers.h, gpu/native_texture_mirror.h, hooks/state.cpp).
// D3DResource_Destroy, which D3DResource_Release and the device's own
// unbinding call once a count reaches zero, is where those addresses die, so
// it drops the mirrors before the guest frees the memory. Without it a texture
// created later at a freed header's address, with the same fetch constant (a
// course reloaded into the same heap), would keep the old mirror, and a freed
// render texture's resolve target would shadow whatever replaced it.
//
// Course and menu textures never reach D3DResource_Destroy: RR6 and PGR3 built
// their headers inside loaded data and freed the memory wholesale. So XPhysicalAlloc
// allocations are tracked too, and freeing one drops every texture and buffer
// mirror whose data lay inside it.

#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/settings.h"
#include "gpu/d3d.h"
#include "gpu/guest_buffers.h"
#include "gpu/native_texture_mirror.h"
#include "gpu/resources.h"

REXCVAR_DEFINE_BOOL(pgr4_log_destroy, false, kCvarGroup,
                    "Log every D3DResource_Destroy and the mirror it drops");

REX_EXTERN(__imp__D3DResource_Destroy);
// XPhysicalAlloc(size, address, alignment, protect) -> va, and XPhysicalFree(va)
// (MmAllocatePhysicalMemoryEx / MmFreePhysicalMemory, their only callers).
REX_EXTERN(__imp__XPhysicalAlloc);
REX_EXTERN(__imp__XPhysicalFree);

namespace pgr4::gpu {
bool ForgetGuestSurface(u32 surface_va);
bool ForgetGuestShader(u32 shader_va);
bool ForgetGuestVertexDeclaration(u32 decl_va);
}

namespace {

using pgr4::gpu::D3DResourceType;

const char *TypeName(D3DResourceType type) {
  switch (type) {
  case D3DResourceType::kVertexBuffer:
    return "vertex buffer";
  case D3DResourceType::kIndexBuffer:
    return "index buffer";
  case D3DResourceType::kTexture:
    return "texture";
  case D3DResourceType::kSurface:
    return "surface";
  case D3DResourceType::kVertexDeclaration:
    return "vertex declaration";
  case D3DResourceType::kVertexShader:
    return "vertex shader";
  case D3DResourceType::kPixelShader:
    return "pixel shader";
  case D3DResourceType::kConstantBuffer:
    return "constant buffer";
  case D3DResourceType::kCommandBuffer:
    return "command buffer";
  default:
    return "resource";
  }
}

// D3DResource_Destroy (0x826CF0C8) switches on the same Common type bits.
void EvictMirrors(u32 va) {
  const auto *hdr = pgr4::mem::at<const pgr4::gpu::D3DResource>(va);
  if (!hdr)
    return;
  const u32 common = hdr->Common;
  const D3DResourceType type = pgr4::gpu::CommonType(common);
  bool evicted = false;
  switch (type) {
  case D3DResourceType::kVertexBuffer: {
    const auto *vb = pgr4::mem::at<const pgr4::gpu::D3DVertexBuffer>(va);
    evicted = pgr4::gpu::EvictGuestBuffer(u32(vb->Fetch0) & ~0x3u,
                                         pgr4::gpu::ResourceType::VertexBuffer);
    break;
  }
  case D3DResourceType::kIndexBuffer: {
    const auto *ib = pgr4::mem::at<const pgr4::gpu::D3DIndexBuffer>(va);
    evicted = pgr4::gpu::EvictGuestBuffer(u32(ib->Address),
                                         pgr4::gpu::ResourceType::IndexBuffer);
    break;
  }
  case D3DResourceType::kTexture:
    evicted = pgr4::gpu::EvictGuestTexture(va);
    break;
  case D3DResourceType::kSurface:
    evicted = pgr4::gpu::ForgetGuestSurface(va);
    break;
  case D3DResourceType::kVertexDeclaration:
    evicted = pgr4::gpu::ForgetGuestVertexDeclaration(va);
    break;
  case D3DResourceType::kVertexShader:
  case D3DResourceType::kPixelShader:
    evicted = pgr4::gpu::ForgetGuestShader(va);
    break;
  default:
    break;
  }
  if (REXCVAR_GET(pgr4_log_destroy))
    PGR4_INFO("D3DResource_Destroy 0x{:08X}: {} (Common 0x{:08X}){}", va,
             TypeName(type), common, evicted ? ", mirror dropped" : "");
}

// Live XPhysicalAlloc allocations, virtual base -> size. Fetch constants and
// buffer headers name their data by these virtual addresses.
std::mutex g_alloc_mutex;
std::unordered_map<u32, u32> g_allocs;

} // namespace

// (resource). Runs before the guest frees the header and its memory.
REX_HOOK_RAW(D3DResource_Destroy) {
  EvictMirrors(ctx.r3.u32);
  __imp__D3DResource_Destroy(ctx, base);
}

REX_HOOK_RAW(XPhysicalAlloc) {
  const u32 size = ctx.r3.u32;
  __imp__XPhysicalAlloc(ctx, base);
  if (!ctx.r3.u32 || !size)
    return;
  std::lock_guard lock(g_alloc_mutex);
  g_allocs[ctx.r3.u32] = size;
}

REX_HOOK_RAW(XPhysicalFree) {
  const u32 va = ctx.r3.u32;
  u32 size = 0;
  {
    std::lock_guard lock(g_alloc_mutex);
    if (auto it = g_allocs.find(va); it != g_allocs.end()) {
      size = it->second;
      g_allocs.erase(it);
    }
  }
  if (size) {
    const u32 textures = pgr4::gpu::EvictGuestTexturesInRange(va, va + size);
    const u32 buffers = pgr4::gpu::EvictGuestBuffersInRange(va, va + size);
    if (REXCVAR_GET(pgr4_log_destroy) && (textures || buffers))
      PGR4_INFO("XPhysicalFree 0x{:08X} ({} bytes): {} texture and {} buffer "
               "mirrors dropped",
               va, size, textures, buffers);
  }
  __imp__XPhysicalFree(ctx, base);
}
