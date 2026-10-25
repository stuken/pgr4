/**
 * @file    gpu/resolve.cpp
 * @brief   EDRAM resolve emulation: surface-to-texture copy, the lazy resolve
 *          alias, and the fullscreen composite chain seeding.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/frame.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>
#include <rex/cvar.h>

#include "core/profiling.h"

#include "core/logging.h"
#include "core/settings.h" // kCvarGroup
#include "gpu/backend.h"
#include "gpu/format.h"
#include "gpu/gpu_timing.h"
#include "gpu/output.h"
#include "gpu/shaders/shader_cache.h"

REXCVAR_DEFINE_BOOL(pgr4_fxaa, false, kCvarGroup,
                    "FXAA on the 3D scene, before the HUD is drawn");
REXCVAR_DEFINE_BOOL(pgr4_scaled_post_filters, true, kCvarGroup,
                    "At a resolution scale, fit the race resolve's taps to a "
                    "host pixel and upscale the glow smoothly");
REXCVAR_DEFINE_BOOL(pgr4_scale_copy_cache, true, kCvarGroup,
                    "Performance experiment: reuse a texture's guest scale "
                    "copy until the texture is written again, instead of "
                    "copying it for every draw");
REXCVAR_DEFINE_BOOL(pgr4_sampled_slots_only, true, kCvarGroup,
                    "Performance experiment: bind, copy and describe only the "
                    "texture slots the draw's shaders fetch through");
// Off by default: an aliased destination is never written itself, and PGR3's
// crowd impostor atlas (resolved once while a course loads) came back as
// stale video memory once its 512x192 source surface moved on.
REXCVAR_DEFINE_BOOL(pgr4_resolve_alias, false, kCvarGroup,
                    "Let a same-size, same-format resolve alias its source "
                    "surface instead of copying it");

namespace pgr4::gpu {

namespace {

inline const plume::RenderTexture *const kDepthFbKey =
    reinterpret_cast<const plume::RenderTexture *>(uintptr_t{1});

GuestTexture *ResolveSourceForFlagsLocked(VideoState &s, u32 flags) {
  if (flags & kResolveDepthStencil)
    return s.depth_stencil;
  // D3DRESOLVE_RENDERTARGET1.
  if ((flags & 3) == 1)
    return s.render_target1;
  if (s.render_target)
    return s.render_target;
  return s.back_buffer_surface;
}

bool CanAliasResolveLocked(const GuestTexture *src, const GuestTexture *dst) {
  return src && dst && src != dst && src->texture && dst->texture &&
         src->sampleCount == plume::RenderSampleCount::COUNT_1 &&
         src->width == dst->width && src->height == dst->height &&
         src->format == dst->format && dst->resolveScale == 1.0f &&
         dst->viewDimension !=
             plume::RenderTextureViewDimension::TEXTURE_CUBE &&
         dst->mipLevels <= 1 && src->descriptorIndex != kInvalidDescriptorIndex;
}

} // namespace

void NoteTextureWrite(GuestTexture *t) {
  static u64 s_version = 0;
  if (t)
    t->contentVersion = ++s_version;
}

void DetachSourceSurfaceLocked(VideoState &s, GuestTexture *texture) {
  if (!texture || !texture->sourceSurface)
    return;
  GuestTexture *source = texture->sourceSurface;
  source->destinationTextures.erase(texture);
  texture->sourceSurface = nullptr;
}

namespace {

// A single-sample copy of a multisampled colour surface, made by the hardware
// resolve (the average of each pixel's samples, as a Xenos colour resolve
// gives), so every single-sample copy path below can take it as its source.
// One scratch texture per size and format, reused by later resolves.
GuestTexture *ResolvedSingleSampleLocked(VideoState &s, GuestTexture *src) {
  static std::unordered_map<u64, std::unique_ptr<GuestTexture>> s_scratch;
  const u64 key = u64(src->width) | (u64(src->height) << 16) |
                  (u64(static_cast<u32>(src->format)) << 32);
  auto &slot = s_scratch[key];
  if (!slot) {
    auto tex = std::make_unique<GuestTexture>();
    plume::RenderTextureDesc desc;
    desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
    desc.width = src->width;
    desc.height = src->height;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arraySize = 1;
    desc.format = src->format;
    desc.flags = plume::RenderTextureFlag::RENDER_TARGET;
    tex->textureHolder = CreateHostTexture(s.device.get(), desc, "msaa-resolve");
    tex->texture = tex->textureHolder.get();
    if (!tex->texture)
      return nullptr;
    tex->width = src->width;
    tex->height = src->height;
    tex->format = src->format;
    tex->viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
    slot = std::move(tex);
  }
  GuestTexture *tex = slot.get();
  tex->guestFormat = src->guestFormat;
  tex->hostScale = src->hostScale;
  if (tex->descriptorIndex == kInvalidDescriptorIndex)
    BindTextureSRVLocked(s, tex);
  // A single-sample source (pgr4_fxaa on a 1x target) is a plain copy.
  const bool msaa = src->sampleCount != plume::RenderSampleCount::COUNT_1;
  const auto src_layout = msaa ? plume::RenderTextureLayout::RESOLVE_SOURCE
                               : plume::RenderTextureLayout::COPY_SOURCE;
  const auto dst_layout = msaa ? plume::RenderTextureLayout::RESOLVE_DEST
                               : plume::RenderTextureLayout::COPY_DEST;
  plume::RenderTextureBarrier pre[2] = {
      plume::RenderTextureBarrier(src->texture, src_layout),
      plume::RenderTextureBarrier(tex->texture, dst_layout),
  };
  s.command_list->barriers(plume::RenderBarrierStage::COPY, pre, std::size(pre));
  src->layout = src_layout;
  tex->layout = dst_layout;
  if (msaa)
    s.command_list->resolveTexture(tex->texture, src->texture);
  else
    s.command_list->copyTexture(tex->texture, src->texture);
  return tex;
}

} // namespace

bool CopySurfaceToTextureLocked(VideoState &s, GuestTexture *src,
                                GuestTexture *dst, const char *reason,
                                CopyFilter filter) {
  if (!dst || !dst->texture || !src || !src->texture || src == dst) {
    return false;
  }
  if (!s.ready)
    return false;
  if (dst->sampleCount != plume::RenderSampleCount::COUNT_1) {
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
      PGR4_WARN("{}: MSAA destination {}x{} rejected (resolve targets must be "
              "single-sample)",
              reason, dst->width, dst->height);
    }
    return false;
  }

  BeginCommandList(s);
  if (!s.command_list_open)
    return false;
  perf::GpuMark(s, reason, dst);

  s.draw_framebuffer_bound = false;

  const bool dims_match =
      src->width == dst->width && src->height == dst->height;
  const bool format_match = src->format == dst->format;
  const float resolve_scale = dst->resolveScale;
  const bool needs_scale = (resolve_scale != 1.0f);
  const bool dst_is_cube =
      dst->viewDimension == plume::RenderTextureViewDimension::TEXTURE_CUBE;

  // A multisampled colour source resolves to one sample first: straight into
  // a destination of its own size and format (the scene resolves), else into
  // a scratch texture the paths below copy from. A depth one takes
  // resolve_msaa_ps's sample-0 path below.
  const bool msaa_color = src->sampleCount != plume::RenderSampleCount::COUNT_1 &&
                          !IsDepthFormat(src->format);
  if (msaa_color && dims_match && format_match && !needs_scale && !dst_is_cube &&
      filter == CopyFilter::Default && dst->mipLevels <= 1) {
    plume::RenderTextureBarrier pre[2] = {
        plume::RenderTextureBarrier(src->texture,
                                    plume::RenderTextureLayout::RESOLVE_SOURCE),
        plume::RenderTextureBarrier(dst->texture,
                                    plume::RenderTextureLayout::RESOLVE_DEST),
    };
    s.command_list->barriers(plume::RenderBarrierStage::COPY, pre, std::size(pre));
    src->layout = plume::RenderTextureLayout::RESOLVE_SOURCE;
    dst->layout = plume::RenderTextureLayout::RESOLVE_DEST;
    s.command_list->resolveTexture(dst->texture, src->texture);
    plume::RenderTextureBarrier post(dst->texture,
                                     plume::RenderTextureLayout::SHADER_READ);
    s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &post, 1);
    dst->layout = plume::RenderTextureLayout::SHADER_READ;
    NoteTextureWrite(dst);
    return true;
  }
  if (msaa_color) {
    src = ResolvedSingleSampleLocked(s, src);
    if (!src)
      return false;
  }

  if (dims_match && format_match && !needs_scale &&
      filter == CopyFilter::Default &&
      src->sampleCount == plume::RenderSampleCount::COUNT_1) {
    plume::RenderTextureBarrier pre[2] = {
        plume::RenderTextureBarrier(src->texture,
                                    plume::RenderTextureLayout::COPY_SOURCE),
        plume::RenderTextureBarrier(dst->texture,
                                    plume::RenderTextureLayout::COPY_DEST),
    };
    s.command_list->barriers(plume::RenderBarrierStage::COPY, pre,
                             std::size(pre));
    src->layout = plume::RenderTextureLayout::COPY_SOURCE;
    dst->layout = plume::RenderTextureLayout::COPY_DEST;
    if (dst_is_cube) {
      s.command_list->copyTextureRegion(
          plume::RenderTextureCopyLocation::Subresource(
              dst->texture, dst->resolveLevel, dst->resolveFace),
          plume::RenderTextureCopyLocation::Subresource(src->texture, 0, 0));
    } else {
      s.command_list->copyTexture(dst->texture, src->texture);
    }
  } else if (dst_is_cube) {
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
      PGR4_WARN("{}: cube resolve dst needs convert/scale (src={}x{} fmt={} "
              "dst fmt={} scale={}), no per-face RTV, skipping",
              reason, src->width, src->height, static_cast<int>(src->format),
              static_cast<int>(dst->format), resolve_scale);
    }
    return false;
  } else {
    if (src->descriptorIndex == kInvalidDescriptorIndex) {
      BindTextureSRVLocked(s, src);
    }
    if (src->descriptorIndex == kInvalidDescriptorIndex) {
      static std::atomic<u32> s_warn{0};
      if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
        PGR4_ERROR("{}: source RT 0x{:x} not in bindless heap, can't "
                 "shader-resolve",
                 reason, reinterpret_cast<uintptr_t>(src));
      }
      return false;
    }

    const bool depth_dst = IsDepthFormat(dst->format);
    if (!depth_dst && !IsRenderTargetCapable(dst->format)) {
      static std::atomic<u32> s_warn{0};
      if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
        PGR4_ERROR("{}: dst format {} is not RT-capable, skipping shader "
                 "resolve (src={}x{} fmt={} dst={}x{} fmt={})",
                 reason, static_cast<int>(dst->format), src->width, src->height,
                 static_cast<int>(src->format), dst->width, dst->height,
                 static_cast<int>(dst->format));
      }
      return false;
    }

    const plume::RenderTexture *fb_key = depth_dst ? kDepthFbKey : nullptr;

    auto it = dst->framebuffers.find(fb_key);
    plume::RenderFramebuffer *dst_fb = nullptr;
    if (it != dst->framebuffers.end()) {
      dst_fb = it->second.get();
    } else {
      std::unique_ptr<plume::RenderFramebuffer> fb;
      if (depth_dst) {
        plume::RenderFramebufferDesc desc;
        desc.depthAttachment = dst->texture;
        desc.colorAttachmentsCount = 0;
        fb = s.device->createFramebuffer(desc);
      } else {
        const plume::RenderTexture *attachments[1] = {dst->texture};
        plume::RenderFramebufferDesc desc(attachments, 1);
        fb = s.device->createFramebuffer(desc);
      }
      if (!fb) {
        static std::atomic<u32> s_warn{0};
        if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
          PGR4_ERROR("{}: createFramebuffer failed (depth_dst={}), skipping "
                   "shader resolve (src={}x{} dst={}x{})",
                   reason, depth_dst ? 1 : 0, src->width, src->height,
                   dst->width, dst->height);
        }
        return false;
      }
      dst_fb = fb.get();
      dst->framebufferAttached = true;
      dst->framebuffers.emplace(fb_key, std::move(fb));
      s.framebuffer_owners.insert(dst);
    }

    const plume::RenderTextureLayout dst_write_layout =
        depth_dst ? plume::RenderTextureLayout::DEPTH_WRITE
                  : plume::RenderTextureLayout::COLOR_WRITE;
    const bool discard_dst =
        (dst->layout == plume::RenderTextureLayout::UNKNOWN);
    plume::RenderTextureBarrier pre[2] = {
        plume::RenderTextureBarrier(src->texture,
                                    plume::RenderTextureLayout::SHADER_READ),
        plume::RenderTextureBarrier(dst->texture, dst_write_layout),
    };
    s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, pre,
                             std::size(pre));
    src->layout = plume::RenderTextureLayout::SHADER_READ;
    dst->layout = dst_write_layout;
    if (discard_dst)
      s.command_list->discardTexture(dst->texture);

    s.command_list->setFramebuffer(dst_fb);
    s.command_list->setViewports(
        plume::RenderViewport(0.0f, 0.0f, static_cast<float>(dst->width),
                              static_cast<float>(dst->height)));
    s.command_list->setScissors(plume::RenderRect(
        0, 0, static_cast<i32>(dst->width), static_cast<i32>(dst->height)));
    plume::RenderPipeline *pipeline = nullptr;
    const bool msaa_src = src->sampleCount != plume::RenderSampleCount::COUNT_1;
    if (msaa_src && depth_dst) {
      PGR4_WARN_FIRST(4, "{}: multisampled depth {}x{} into a depth target is "
                     "not modelled", reason, src->width, src->height);
      return false;
    }
    if (msaa_src) {
      pipeline = GetOrCreateResolveMSAAPipeline(s, dst->format, src->sampleCount);
    } else if (depth_dst) {
      pipeline = GetOrCreateCopyDepthPipeline(s, dst->format);
    } else {
      pipeline = GetOrCreateResolvePipeline(s, dst->format);
    }
    if (!pipeline) {
      static std::atomic<u32> s_warn{0};
      if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
        PGR4_ERROR("{}: createGraphicsPipeline failed for dst format {}", reason,
                 static_cast<int>(dst->format));
      }
      return false;
    }
    s.command_list->setPipeline(pipeline);
    const u32 descriptor_index = src->descriptorIndex;

    u32 box_ratio = 0u;
    if (!depth_dst && src->sampleCount == plume::RenderSampleCount::COUNT_1 &&
        dst->width != 0u && dst->height != 0u && src->width > dst->width &&
        src->height > dst->height && (src->width % dst->width) == 0u &&
        (src->height % dst->height) == 0u &&
        (src->width / dst->width) == (src->height / dst->height)) {
      box_ratio = src->width / dst->width;
    }

    struct CopyPushConstants {
      u32 resourceDescriptorIndex;
      u32 resourceDescriptorIndex2;
      float param0;
      float param1;
    } push{descriptor_index,
           // resolve_msaa_ps: bit 1 takes sample 0 of a depth source.
           msaa_src && IsDepthFormat(src->format) ? 2u : 0u,
           depth_dst ? 1.0f : resolve_scale,
           // copy_color_ps: a negative Param1 selects the B-spline upscale.
           filter == CopyFilter::SmoothUpscale && !depth_dst
               ? -1.0f
               : static_cast<float>(box_ratio)};
    s.command_list->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &push,
                                             kCopyPushConstantByteOffset,
                                             sizeof(push));
    s.command_list->drawInstanced(3, 1, 0, 0);

    s.command_list->setFramebuffer(nullptr);
    s.current_pso = nullptr;
    s.dirtyStates.pipelineState = true;
    s.draw_framebuffer_bound = false;
  }

  plume::RenderTextureBarrier post(dst->texture,
                                   plume::RenderTextureLayout::SHADER_READ);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &post, 1);
  dst->layout = plume::RenderTextureLayout::SHADER_READ;
  NoteTextureWrite(dst);
  return true;
}

namespace {

bool ClearColorTargetLocked(VideoState &s, GuestTexture *dst,
                            plume::RenderColor color) {
  if (!dst || !dst->texture || !s.ready)
    return false;
  if (IsDepthFormat(dst->format) ||
      dst->sampleCount != plume::RenderSampleCount::COUNT_1)
    return false;
  BeginCommandList(s);
  if (!s.command_list_open)
    return false;
  s.draw_framebuffer_bound = false;
  perf::GpuMark(s, "clear texture", dst);

  plume::RenderFramebuffer *dst_fb = GetFramebuffer(s, dst, nullptr);
  if (!dst_fb)
    return false;

  plume::RenderTextureBarrier pre(dst->texture,
                                  plume::RenderTextureLayout::COLOR_WRITE);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &pre, 1);
  dst->layout = plume::RenderTextureLayout::COLOR_WRITE;
  s.command_list->setFramebuffer(dst_fb);
  s.command_list->setViewports(
      plume::RenderViewport(0.0f, 0.0f, static_cast<float>(dst->width),
                            static_cast<float>(dst->height)));
  s.command_list->setScissors(plume::RenderRect(
      0, 0, static_cast<i32>(dst->width), static_cast<i32>(dst->height)));
  s.command_list->clearColor(0, color);
  s.command_list->setFramebuffer(nullptr);
  s.draw_framebuffer_bound = false;
  NoteTextureWrite(dst);

  plume::RenderTextureBarrier post(dst->texture,
                                   plume::RenderTextureLayout::SHADER_READ);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &post, 1);
  dst->layout = plume::RenderTextureLayout::SHADER_READ;
  return true;
}

} // namespace

void Video::ClearTexture(GuestTexture *texture, u32 color_argb) {
  if (!texture || !texture->texture)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  ClearColorTargetLocked(s, texture, ArgbToRenderColor(color_argb));
}

bool MaterializeOutboundLocked(VideoState &s, GuestTexture *source,
                               bool aliasable_only) {
  if (!source || source->destinationTextures.empty())
    return false;
  bool recorded = false;
  std::vector<GuestTexture *> destinations(source->destinationTextures.begin(),
                                           source->destinationTextures.end());
  for (GuestTexture *dst : destinations) {
    if (!dst || dst == source || dst->sourceSurface != source)
      continue;
    if (dst->pendingDestroy) {
      DetachSourceSurfaceLocked(s, dst);
      continue;
    }
    const bool one_to_one =
        source->width == dst->width && source->height == dst->height &&
        source->format == dst->format &&
        source->sampleCount == plume::RenderSampleCount::COUNT_1 &&
        dst->resolveScale == 1.0f &&
        dst->viewDimension != plume::RenderTextureViewDimension::TEXTURE_CUBE;
    if (aliasable_only && !one_to_one)
      continue;
    if (CopySurfaceToTextureLocked(s, source, dst, "Materialize")) {
      recorded = true;
      DetachSourceSurfaceLocked(s, dst);
    }
  }
  return recorded;
}

void MaterializeInboundLocked(VideoState &s, GuestTexture *dst) {
  if (!dst || !dst->sourceSurface || dst->sourceSurface == dst)
    return;
  if (CopySurfaceToTextureLocked(s, dst->sourceSurface, dst, "Materialize")) {
    DetachSourceSurfaceLocked(s, dst);
  }
}

bool FullscreenChainClassLocked(const VideoState &s, const GuestTexture *t) {
  const GuestTexture *bb = s.back_buffer_surface;
  if (!t || !bb || t == bb || IsDepthFormat(t->format) || t->reflection)
    return false;
  constexpr u32 kDesignWidth = 1280u;
  const u32 min_full_width =
      bb->width > kDesignWidth ? kDesignWidth : bb->width;
  if (t->width < min_full_width)
    return false;
  const u64 a = u64(t->width) * bb->height;
  const u64 b = u64(t->height) * bb->width;
  const u64 diff = a > b ? a - b : b - a;
  return diff * 100ull <= b * 3ull;
}

void Video::TrackResolveSource(u32 flags, GuestTexture *dst, u32 level,
                               u32 face) {
  if (!dst || !dst->texture)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return;
  dst->resolveLevel = level;
  dst->resolveFace = face;
  const int exp_bias = static_cast<i32>(flags) >> kResolveExponentBiasShift;
  dst->resolveScale = static_cast<float>(std::ldexp(1.0, exp_bias));
  GuestTexture *src = ResolveSourceForFlagsLocked(s, flags);
  if (!src || !src->texture || src == dst) {
    DetachSourceSurfaceLocked(s, dst);
    return;
  }
  if (dst->sourceSurface && dst->sourceSurface != src)
    dst->sourceSurface->destinationTextures.erase(dst);
  dst->sourceSurface = src;
  src->destinationTextures.insert(dst);
  BindTextureSRVLocked(s, src);
}

namespace {

void NoteTileContentLocked(VideoState &s, GuestTexture *dst) {
  if (!dst || !dst->texture || !FullscreenChainClassLocked(s, dst))
    return;
  s.fullscreen_chain_head = dst;
}

} // namespace

namespace {

// A partial resolve that converts formats (PGR3's 7e3 scene into its
// k_8_8_8_8 bloom textures): the colour copy shader, drawn over the source's
// full extent placed so its rect lands at the destination point, and
// scissored to that rect, so texels map 1:1 and the rest of the destination
// is kept.
bool ShaderCopyRegionLocked(VideoState &s, GuestTexture *src, GuestTexture *dst,
                            const plume::RenderRect *src_rect, u32 dst_x,
                            u32 dst_y) {
  if (src->hostScale != dst->hostScale || dst->resolveScale != 1.0f ||
      src->sampleCount != plume::RenderSampleCount::COUNT_1 ||
      dst->sampleCount != plume::RenderSampleCount::COUNT_1 ||
      dst->viewDimension == plume::RenderTextureViewDimension::TEXTURE_CUBE ||
      !IsRenderTargetCapable(dst->format))
    return false;
  i32 left = src_rect ? std::max(src_rect->left, 0) : 0;
  i32 top = src_rect ? std::max(src_rect->top, 0) : 0;
  i32 right = std::min(src_rect ? src_rect->right : i32(src->width), i32(src->width));
  i32 bottom = std::min(src_rect ? src_rect->bottom : i32(src->height), i32(src->height));
  right = std::min(right, left + i32(dst->width) - i32(dst_x));
  bottom = std::min(bottom, top + i32(dst->height) - i32(dst_y));
  if (right <= left || bottom <= top || dst_x >= dst->width || dst_y >= dst->height)
    return true;
  BeginCommandList(s);
  if (!s.command_list_open)
    return false;
  if (src->descriptorIndex == kInvalidDescriptorIndex)
    BindTextureSRVLocked(s, src);
  if (src->descriptorIndex == kInvalidDescriptorIndex)
    return false;
  auto it = dst->framebuffers.find(nullptr);
  plume::RenderFramebuffer *dst_fb = nullptr;
  if (it != dst->framebuffers.end()) {
    dst_fb = it->second.get();
  } else {
    const plume::RenderTexture *attachments[1] = {dst->texture};
    plume::RenderFramebufferDesc desc(attachments, 1);
    auto fb = s.device->createFramebuffer(desc);
    if (!fb)
      return false;
    dst_fb = fb.get();
    dst->framebufferAttached = true;
    dst->framebuffers.emplace(nullptr, std::move(fb));
    s.framebuffer_owners.insert(dst);
  }
  plume::RenderPipeline *pipeline = GetOrCreateResolvePipeline(s, dst->format);
  if (!pipeline)
    return false;
  s.draw_framebuffer_bound = false;
  plume::RenderTextureBarrier pre[2] = {
      plume::RenderTextureBarrier(src->texture,
                                  plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(dst->texture,
                                  plume::RenderTextureLayout::COLOR_WRITE),
  };
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, pre, std::size(pre));
  src->layout = plume::RenderTextureLayout::SHADER_READ;
  dst->layout = plume::RenderTextureLayout::COLOR_WRITE;
  s.command_list->setFramebuffer(dst_fb);
  s.command_list->setViewports(plume::RenderViewport(
      float(i32(dst_x) - left), float(i32(dst_y) - top), float(src->width),
      float(src->height)));
  s.command_list->setScissors(plume::RenderRect(
      i32(dst_x), i32(dst_y), i32(dst_x) + (right - left),
      i32(dst_y) + (bottom - top)));
  s.command_list->setPipeline(pipeline);
  struct CopyPushConstants {
    u32 resourceDescriptorIndex;
    u32 resourceDescriptorIndex2;
    float param0;
    float param1;
  } push{src->descriptorIndex, 0u, 1.0f, 0.0f};
  s.command_list->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &push,
                                           kCopyPushConstantByteOffset,
                                           sizeof(push));
  s.command_list->drawInstanced(3, 1, 0, 0);
  s.command_list->setFramebuffer(nullptr);
  s.current_pso = nullptr;
  s.dirtyStates.pipelineState = true;
  plume::RenderTextureBarrier post(dst->texture,
                                   plume::RenderTextureLayout::SHADER_READ);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &post, 1);
  dst->layout = plume::RenderTextureLayout::SHADER_READ;
  NoteTextureWrite(dst);
  return true;
}

// A resolve of part of the surface: a straight region copy, which needs the
// same format and size class on both sides (no scaling or MSAA).
bool CopySurfaceRegionLocked(VideoState &s, GuestTexture *src, GuestTexture *dst,
                             const plume::RenderRect *src_rect, u32 dst_x,
                             u32 dst_y) {
  // A multisampled colour source resolves to one sample first.
  if (src->sampleCount != plume::RenderSampleCount::COUNT_1 &&
      !IsDepthFormat(src->format) &&
      dst->sampleCount == plume::RenderSampleCount::COUNT_1) {
    BeginCommandList(s);
    if (!s.command_list_open)
      return false;
    src = ResolvedSingleSampleLocked(s, src);
    if (!src)
      return false;
  }
  // A format change (a 7e3 scene into an 8-bit texture, depth into R32_FLOAT)
  // runs the copy shader; a straight copy needs matching formats.
  if (src->format != dst->format)
    return ShaderCopyRegionLocked(s, src, dst, src_rect, dst_x, dst_y);
  if (src->format != dst->format || dst->resolveScale != 1.0f ||
      src->hostScale != dst->hostScale ||
      src->sampleCount != plume::RenderSampleCount::COUNT_1 ||
      dst->sampleCount != plume::RenderSampleCount::COUNT_1)
    return false;
  const bool dst_is_cube =
      dst->viewDimension == plume::RenderTextureViewDimension::TEXTURE_CUBE;
  const u32 level = dst_is_cube ? dst->resolveLevel : 0u;
  const u32 dst_w = std::max(dst->width >> level, 1u);
  const u32 dst_h = std::max(dst->height >> level, 1u);
  i32 left = src_rect ? src_rect->left : 0;
  i32 top = src_rect ? src_rect->top : 0;
  i32 right = src_rect ? src_rect->right : i32(src->width);
  i32 bottom = src_rect ? src_rect->bottom : i32(src->height);
  // Clip to the source surface, then to what fits at the destination point.
  left = std::max(left, 0);
  top = std::max(top, 0);
  right = std::min(right, i32(src->width));
  bottom = std::min(bottom, i32(src->height));
  right = std::min(right, left + i32(dst_w) - i32(dst_x));
  bottom = std::min(bottom, top + i32(dst_h) - i32(dst_y));
  if (right <= left || bottom <= top || dst_x >= dst_w || dst_y >= dst_h)
    return true; // nothing lands inside the destination

  BeginCommandList(s);
  if (!s.command_list_open)
    return false;
  s.draw_framebuffer_bound = false;

  plume::RenderTextureBarrier pre[2] = {
      plume::RenderTextureBarrier(src->texture,
                                  plume::RenderTextureLayout::COPY_SOURCE),
      plume::RenderTextureBarrier(dst->texture,
                                  plume::RenderTextureLayout::COPY_DEST),
  };
  s.command_list->barriers(plume::RenderBarrierStage::COPY, pre, std::size(pre));
  src->layout = plume::RenderTextureLayout::COPY_SOURCE;
  dst->layout = plume::RenderTextureLayout::COPY_DEST;
  const plume::RenderBox box(left, top, right, bottom);
  s.command_list->copyTextureRegion(
      plume::RenderTextureCopyLocation::Subresource(
          dst->texture, level, dst_is_cube ? dst->resolveFace : 0u),
      plume::RenderTextureCopyLocation::Subresource(src->texture, 0, 0), dst_x,
      dst_y, 0, &box);
  plume::RenderTextureBarrier post(dst->texture,
                                   plume::RenderTextureLayout::SHADER_READ);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &post, 1);
  dst->layout = plume::RenderTextureLayout::SHADER_READ;
  NoteTextureWrite(dst);
  return true;
}

} // namespace

void Video::ResolveRtToTexture(GuestTexture *dst,
                               const plume::RenderRect *src_rect, u32 dst_x,
                               u32 dst_y) {
  PGR4_CPU_ZONE("ResolveRtToTexture");
  if (!dst || !dst->texture)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return;
  GuestTexture *src = dst->sourceSurface;
  if (!src || src == dst || !src->texture)
    return;

  // The guest's rect and point are in guest pixels, and each host surface is
  // scaled by its own host scale.
  const i32 src_scale = i32(src->hostScale);
  plume::RenderRect scaled_rect;
  if (src_rect) {
    scaled_rect = {src_rect->left * src_scale, src_rect->top * src_scale,
                   src_rect->right * src_scale, src_rect->bottom * src_scale};
    src_rect = &scaled_rect;
  }
  dst_x *= dst->hostScale;
  dst_y *= dst->hostScale;

  // A sub-rectangle (or an offset destination) that is not simply the whole
  // surface onto the whole texture.
  // A whole-surface resolve from a surface larger than the destination copies
  // 1:1 and clips on Xenos, so it takes the destination-sized corner. PGR3's
  // shadow depth surface grows to cover the larger targets it is also bound
  // with, and scaling the whole of it shrank each car's shadow map into a
  // corner.
  plume::RenderRect corner;
  if (!src_rect && dst_x == 0 && dst_y == 0 && src->hostScale == dst->hostScale &&
      (src->width > dst->width || src->height > dst->height) &&
      dst->viewDimension != plume::RenderTextureViewDimension::TEXTURE_CUBE) {
    corner = {0, 0, i32(std::min(src->width, dst->width)),
              i32(std::min(src->height, dst->height))};
    src_rect = &corner;
  }
  const bool partial =
      (src_rect && (src_rect->left != 0 || src_rect->top != 0 ||
                    u32(src_rect->right) != src->width ||
                    u32(src_rect->bottom) != src->height)) ||
      dst_x != 0 || dst_y != 0;
  if (partial) {
    perf::GpuMark(s, "partial resolve", dst);
    if (CopySurfaceRegionLocked(s, src, dst, src_rect, dst_x, dst_y)) {
      s.last_resolved_dst = dst;
      DetachSourceSurfaceLocked(s, dst);
      return;
    }
    static std::atomic<u32> s_warn{0};
    if (s_warn.fetch_add(1, std::memory_order_relaxed) < 8) {
      PGR4_WARN("Resolve: partial resolve {}x{} fmt={} -> {}x{} fmt={} scale={} "
               "needs a conversion, resolving the whole surface",
               src->width, src->height, static_cast<int>(src->format),
               dst->width, dst->height, static_cast<int>(dst->format),
               dst->resolveScale);
    }
  }

  if (REXCVAR_GET(pgr4_resolve_alias) && CanAliasResolveLocked(src, dst)) {
    s.last_resolved_dst = dst;
    NoteTileContentLocked(s, dst);
    s.draw_framebuffer_bound = false;
    return;
  }

  if (CopySurfaceToTextureLocked(s, src, dst, "Resolve")) {
    s.last_resolved_dst = dst;
    NoteTileContentLocked(s, dst);
    DetachSourceSurfaceLocked(s, dst);
  }
}

u32 TargetScaleLocked(VideoState &s) {
  GuestTexture *rt = nullptr;
  GuestTexture *ds = nullptr;
  ResolveEffectiveTargets(s, rt, ds);
  if (rt)
    return rt->hostScale;
  if (ds)
    return ds->hostScale;
  return Output::ResolutionScale();
}

GuestTexture *SampledTextureLocked(const VideoState &s, GuestTexture *tex) {
  GuestTexture *src = tex ? tex->sourceSurface : nullptr;
  if (src && src->texture &&
      src->sampleCount == plume::RenderSampleCount::COUNT_1 &&
      src != s.render_target && src != s.depth_stencil &&
      src->descriptorIndex != kInvalidDescriptorIndex)
    return src;
  return tex;
}

// A null SetTexture leaves a slot's last texture bound (Video::SetTexture), so
// most slots hold textures from earlier draws that this draw's shaders never
// fetch. A shader without a cache entry keeps every slot of its stage.
u32 SampledSlotMaskLocked(const VideoState &s) {
  constexpr u32 kPixelSlots = (1u << kPixelSamplerSlots) - 1;
  constexpr u32 kVertexSlots = ((1u << kHostSamplerSlots) - 1) & ~kPixelSlots;
  if (!REXCVAR_GET(pgr4_sampled_slots_only))
    return kPixelSlots | kVertexSlots;
  auto stage = [](const GuestShader *gs, u32 all) -> u32 {
    if (!gs)
      return 0;
    return gs->shaderCacheEntry ? gs->shaderCacheEntry->samplerSlotMask & all : all;
  };
  return stage(s.pixel_shader, kPixelSlots) | stage(s.vertex_shader, kVertexSlots);
}

u32 Video::SampledSlotMask() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  return SampledSlotMaskLocked(s);
}

namespace {

// A render-target texture of `like`'s format at w x h, for a filtered copy.
// Never resized: a texture's size is fixed for its lifetime.
std::unique_ptr<GuestTexture> CreateGuestScaleCopy(VideoState &s,
                                                   const GuestTexture *like,
                                                   u32 w, u32 h,
                                                   u32 host_scale) {
  auto copy = std::make_unique<GuestTexture>(ResourceType::Texture);
  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = w;
  desc.height = h;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.format = like->format;
  desc.flags = plume::RenderTextureFlag::RENDER_TARGET;
  desc.committed = false;
  copy->textureHolder =
      CreateHostTexture(s.device.get(), desc, "guest-scale-copy");
  copy->texture = copy->textureHolder.get();
  if (!copy->texture)
    return nullptr;
  plume::RenderTextureViewDesc view_desc;
  view_desc.format = like->format;
  view_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  view_desc.mipLevels = 1;
  copy->textureView = copy->texture->createTextureView(view_desc);
  copy->width = w;
  copy->height = h;
  copy->depth = 1;
  copy->format = like->format;
  copy->guestFormat = like->guestFormat;
  copy->viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  copy->hostScale = host_scale;
  BindTextureSRVLocked(s, copy.get());
  if (copy->descriptorIndex == kInvalidDescriptorIndex)
    return nullptr;
  return copy;
}

} // namespace

// A pass must see the textures it samples at its own target's scale, as the
// guest does, wherever the two differ (Output::SurfaceScale keeps RR6's
// 320x180 glow at 1x):
// - Into a lower-scale target, a box-filtered copy. The glow bright-passes the
//   1280x720 frame into its buffer with one bilinear tap; reading the scaled
//   frame directly, that tap would cover a fraction of its 4x4 guest texels
//   and alias.
// - Into a higher-scale target, a B-spline upscale of a reduced-scale resolve
//   target. The glow composite stretches the 320x180 glow over the frame:
//   bilinear at 12x (3x scale) changes slope at every glow texel edge, 12
//   pixels apart, which reads as bands. Upscaled smoothly to 960x540, the
//   composite's own 4x stretch is then the guest's.
void BuildGuestScaleCopiesLocked(VideoState &s) {
  const u32 target_scale = TargetScaleLocked(s);
  const u32 sampled = SampledSlotMaskLocked(s);
  for (u32 i = 0; i < 16; ++i) {
    s.sample_overrides[i] = nullptr;
    GuestTexture *tex = s.textures[i];
    if (!(sampled >> i & 1u) || !tex || !tex->texture ||
        tex->hostScale == target_scale)
      continue;
    const bool down = tex->hostScale > target_scale;
    if ((!down && (!tex->reducedScale || !REXCVAR_GET(pgr4_scaled_post_filters))) ||
        tex->viewDimension != plume::RenderTextureViewDimension::TEXTURE_2D ||
        tex->mipLevels > 1 || !IsRenderTargetCapable(tex->format) ||
        tex->width % tex->hostScale || tex->height % tex->hostScale)
      continue;
    // A texture bound to several slots is copied once.
    for (u32 j = 0; j < i && !s.sample_overrides[i]; ++j) {
      if (s.textures[j] == tex)
        s.sample_overrides[i] = s.sample_overrides[j];
    }
    if (s.sample_overrides[i])
      continue;
    GuestTexture *src = SampledTextureLocked(s, tex);
    if (!src->texture || src->sampleCount != plume::RenderSampleCount::COUNT_1)
      continue;
    const u32 w = tex->width / tex->hostScale * target_scale;
    const u32 h = tex->height / tex->hostScale * target_scale;
    if (!tex->guestScaleCopy) {
      tex->guestScaleCopy = CreateGuestScaleCopy(s, tex, w, h, target_scale);
      if (!tex->guestScaleCopy) {
        PGR4_WARN_FIRST(4, "guest scale copy {}x{} fmt={} could not be created",
                       w, h, static_cast<int>(tex->format));
        continue;
      }
    }
    GuestTexture *copy = tex->guestScaleCopy.get();
    if (copy->width != w || copy->height != h)
      continue;
    // Current until its source is written again: the scene's 320x180
    // reflection view, upscaled for every scene draw, is drawn once a frame.
    if (REXCVAR_GET(pgr4_scale_copy_cache) && copy->scaledFrom == src &&
        src->contentVersion &&
        copy->scaledVersion == src->contentVersion) {
      s.sample_overrides[i] = copy;
      continue;
    }
    if (CopySurfaceToTextureLocked(s, src, copy,
                                   down ? "GuestScaleCopy" : "GuestScaleUpscale",
                                   down ? CopyFilter::Default
                                        : CopyFilter::SmoothUpscale)) {
      s.sample_overrides[i] = copy;
      copy->scaledFrom = src;
      copy->scaledVersion = src->contentVersion;
    }
  }
}

// PGR4's tone map (the FP16 scene in t0, the 320x180 bloom in t3) is the
// first draw of the final pass, a
// BeginVertices triangle over the whole 1280x720 front buffer; the HUD is
// drawn into the same target straight after. FXAA on its output, in place,
// anti-aliases the tone-mapped scene without softening the HUD; its input is
// still HDR, where FXAA's luma edges would be wrong. (PGR3 hooked its own
// composite, ps_8200E300, the same way.)
constexpr u64 kCompositePSHash = 0xDFF1E0F0383C494Bull;

void FxaaAfterCompositeLocked(VideoState &s) {
  if (!REXCVAR_GET(pgr4_fxaa))
    return;
  const GuestShader *ps = s.pixel_shader;
  if (!ps || !ps->shaderCacheEntry ||
      ps->shaderCacheEntry->hash != kCompositePSHash)
    return;
  GuestTexture *rt = s.bound_fb_rt;
  if (!rt || !rt->texture || IsDepthFormat(rt->format) || !s.command_list_open)
    return;
  plume::RenderPipeline *pipeline =
      GetOrCreateResolvePipeline(s, rt->format, true, rt->sampleCount);
  plume::RenderFramebuffer *fb = GetFramebuffer(s, rt, nullptr);
  if (!pipeline || !fb)
    return;
  perf::GpuMark(s, "fxaa", rt);
  GuestTexture *src = ResolvedSingleSampleLocked(s, rt);
  if (!src || src->descriptorIndex == kInvalidDescriptorIndex)
    return;
  plume::RenderTextureBarrier pre[2] = {
      plume::RenderTextureBarrier(src->texture,
                                  plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(rt->texture,
                                  plume::RenderTextureLayout::COLOR_WRITE),
  };
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, pre,
                           std::size(pre));
  src->layout = plume::RenderTextureLayout::SHADER_READ;
  rt->layout = plume::RenderTextureLayout::COLOR_WRITE;
  s.command_list->setFramebuffer(fb);
  s.command_list->setViewports(plume::RenderViewport(
      0.0f, 0.0f, float(rt->width), float(rt->height)));
  s.command_list->setScissors(
      plume::RenderRect(0, 0, i32(rt->width), i32(rt->height)));
  s.command_list->setPipeline(pipeline);
  struct CopyPushConstants {
    u32 resourceDescriptorIndex;
    u32 resourceDescriptorIndex2;
    float param0;
    float param1;
  } push{src->descriptorIndex, 0u, 1.0f, 0.0f};
  s.command_list->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &push,
                                           kCopyPushConstantByteOffset,
                                           sizeof(push));
  s.command_list->drawInstanced(3, 1, 0, 0);
  NoteTextureWrite(rt);
  // The next guest draw binds its own framebuffer, pipeline and viewport.
  s.current_pso = nullptr;
  s.dirtyStates.pipelineState = true;
  s.draw_framebuffer_bound = false;
}

} // namespace pgr4::gpu
