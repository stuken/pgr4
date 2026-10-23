/**
 * @file    gpu/d3d.h
 * @brief   Guest layouts of the XDK 2.0.6132 D3D9 objects PGR4's code reads and
 *          writes.
 *
 * Structured after pgr3's XDK 2.0.2099 header, itself ported from rr6's and
 * reblue's (Copyright (c) 2026 Tom Clay, BSD 3-Clause; see licenses/). 6132
 * rearranged almost everything, so every offset here was re-derived from
 * PGR4's own D3D code (docs/native-renderer-plan.md, Phase 4):
 *
 * - The register shadows from the draw-time flush that DrawVertices,
 *   DrawIndexedVertices and BeginVertices inline. It writes each register
 *   group from one contiguous device block (see D3DDevice), so a field's
 *   register follows from its offset.
 * - The render- and sampler-state setters, paired with the D3DRS/D3DSAMP enum
 *   by the {Get, Set, Default} tables (scripts/re/state_setters.py). 6132's
 *   render-state enum is the later one (101 entries; 2099 had 97), and each
 *   setter writes the register its state names (MULTISAMPLEMASK
 *   PA_SC_AA_MASK, LASTPIXEL PA_SC_LINE_CNTL, the guard bands
 *   PA_CL_GB_*_ADJ, ...).
 * - The resource headers from the creators, GetDesc, Destroy's per-type
 *   cases and the Set* functions that read them. D3DResource grew from 12 to
 *   24 bytes, which moves every resource's payload.
 *
 * Not yet checked against live data (a destroy log and pgr4_trace_frame, as
 * pgr3 did); fields only the code shows are marked as such.
 */
#pragma once

#include <cstddef>
#include <rex/types.h>

namespace pgr4::gpu {

// D3DResource::Common as 6132 packs it:
//   bits 0-3    D3DRESOURCETYPE (D3DResourceType below), what Destroy switches
//               on (2099: a header kind in bits 16-18)
//   bit 20      set on headers D3D creates (CreateSurface, CreatePixelShader,
//               CreateIndexBuffer)
//   bit 21      created with usage 0x4, its data in cached memory
//               (CreateIndexBuffer; 2099: bit 30)
//   bit 30      a surface that is a texture level: Parent (+0x18) owns the
//               memory, and AddRef/Release forward to it
//   bit 31      INDEX32 on an index buffer. On a surface, EDRAM that
//               CreateSurface allocated and Destroy returns.
// The reference count has its own dword (+0x04) in 6132.
constexpr u32 kCommonTypeMask = 0x0000000Fu;
constexpr u32 kCommonD3DCreated = 0x00100000u;
constexpr u32 kCommonCachedMemory = 0x00200000u;
constexpr u32 kCommonLevelSurface = 0x40000000u;
constexpr u32 kCommonIndex32 = 0x80000000u;
constexpr u32 kCommonEdramAllocated = 0x80000000u;

// D3DRESOURCETYPE: Common bits 0-3, and what GetDesc reports (16 for a
// volume). D3DResource_Destroy handles 1-9.
enum class D3DResourceType : u32 {
  kVertexBuffer = 1,
  kIndexBuffer = 2,
  kTexture = 3, // 2D, cube, volume or array: the fetch constant says which
  kSurface = 4,
  kVertexDeclaration = 5,
  kVertexShader = 6,
  kPixelShader = 7,
  kConstantBuffer = 8,
  kCommandBuffer = 9,
  kVolume = 16,
};

inline D3DResourceType CommonType(u32 common) {
  return D3DResourceType(common & kCommonTypeMask);
}

// 24-byte header shared by every D3D9 resource, shaders and declarations
// included.
struct D3DResource {
  be_u32 Common;         // +0x00  the kCommon* bits above
  be_u32 ReferenceCount; // +0x04  D3DResource_AddRef/Release
  // +0x08  GPU fence of the last use: unbinding stores the device's current
  // fence (+0x2A9C), and Destroy blocks on it.
  be_u32 Fence;
  be_u32 ReadFence;      // +0x0C  (by layout; not seen read yet)
  be_u32 Identifier;     // +0x10  (by layout; not seen read yet)
  be_u32 BaseFlush;      // +0x14  0xFFFF0000 at create
};
static_assert(sizeof(D3DResource) == 24);

// The device's cached viewport at D3DDevice+0x3160. 6132 keeps it as floats:
// D3DDevice_SetViewport (0x826C6378) converts the API's D3DVIEWPORT9 and
// passes the floats to sub_826C6100, which stores them.
struct D3DViewportF {
  be_f32 X;
  be_f32 Y;
  be_f32 Width;  // clamped to the render target
  be_f32 Height;
  be_f32 MinZ;
  be_f32 MaxZ;
  be_u32 Unused18; // zeroed by every store
};
static_assert(sizeof(D3DViewportF) == 28);

// Guest RECT (LTRB), the device's scissor rect at D3DDevice+0x317C.
struct D3DRect {
  be_i32 left;
  be_i32 top;
  be_i32 right;
  be_i32 bottom;
};
static_assert(sizeof(D3DRect) == 16);

// GPUTEXTURE_FETCH_CONSTANT: the 6-dword Xenos texture descriptor that
// textures carry at +0x1C. native_texture_mirror decodes it.
struct GpuTextureFetchConstant {
  be_u32 dword[6];
};
static_assert(sizeof(GpuTextureFetchConstant) == 24);

// D3DTexture / D3DCubeTexture / D3DVolumeTexture / D3DArrayTexture, 52 bytes
// (D3DDevice_CreateTexture, 0x826C89F8). D3DResource_Destroy frees the base
// (dword 1, +0x20) and mip (dword 5, +0x30) allocations the fetch constant
// names; D3DDevice_SetTexture copies the fetch constant into the device.
struct D3DTexture {
  D3DResource resource;           // +0x00
  be_u32 MipFlush;                // +0x18
  GpuTextureFetchConstant Format; // +0x1C
};
static_assert(sizeof(D3DTexture) == 52);
static_assert(offsetof(D3DTexture, Format) == 0x1C);

// D3DSurface, 48 bytes (D3DDevice_CreateSurface, 0x826C8B18; 2099: 64). It no
// longer carries a fetch constant.
struct D3DSurface {
  D3DResource resource; // +0x00
  // +0x18  RB_SURFACE_INFO of a created surface (D3DSurface_GetDesc reports
  // bits 16-17 as the MultiSampleType), or, with kCommonLevelSurface, the
  // D3DTexture* it is a level of.
  be_u32 SurfaceInfo;
  // +0x1C  RB_COLOR_INFO / RB_DEPTH_INFO: EDRAM base tile in bits 0-11
  // (Destroy frees from there). SetRenderTarget copies it into the shadow.
  // A level surface keeps its level in bits 28-31 instead.
  be_u32 EdramInfo;
  be_u32 HiZInfo; // +0x20  bit 0: owns the HiZ area (Destroy)
  // +0x24  width - 1 in bits 18-31, height - 1 in bits 3-17 (GetDesc, and
  // ClearF's default rect).
  be_u32 Size;
  be_u32 Format;   // +0x28  guest D3DFORMAT, as GetDesc reports it
  // +0x2C  bytes of EDRAM, a multiple of the 5 KB tile (Destroy).
  be_u32 EdramSize;
};
static_assert(sizeof(D3DSurface) == 48);
static_assert(offsetof(D3DSurface, EdramInfo) == 0x1C);
static_assert(offsetof(D3DSurface, Format) == 0x28);

// One EDRAM tile: 80x16 samples of 32 bits.
constexpr u32 kEdramTileBytes = 5120;

// D3DSURFACE_DESC, written by D3DSurface_GetDesc (0x826C8C40).
struct D3DSurfaceDesc {
  be_u32 Format;             // +0x00  guest D3DFORMAT
  be_u32 Type;               // +0x04  D3DRESOURCETYPE (4)
  be_u32 Usage;              // +0x08  0
  be_u32 Pool;               // +0x0C  0
  be_u32 MultiSampleType;    // +0x10  SurfaceInfo bits 16-17
  be_u32 MultiSampleQuality; // +0x14  0
  be_u32 Width;              // +0x18
  be_u32 Height;             // +0x1C
};
static_assert(sizeof(D3DSurfaceDesc) == 32);

// D3DVertexBuffer, 32 bytes. Fetch0/Fetch1 are the first two words of the
// buffer's Xenos vertex fetch constant, except that Fetch0 holds the data's
// virtual address | 3 (the fetch type): D3DDevice_SetStreamSource adds the
// stream offset, translates the address to physical and takes the offset off
// Fetch1's size (bits 2-25; endian swap in bits 0-1) before storing them as
// vertex fetch constant 95 - stream. Destroy frees Fetch0's address (bits
// 2-31). PGR4's own buffers carry Common 0x00200001: cached memory, without
// kCommonD3DCreated, so the game builds these headers itself, as PGR3 did
// (only mercury calls D3DDevice_CreateIndexBuffer).
struct D3DVertexBuffer {
  D3DResource resource; // +0x00
  be_u32 Fetch0;        // +0x18
  be_u32 Fetch1;        // +0x1C
};
static_assert(sizeof(D3DVertexBuffer) == 32);

// D3DIndexBuffer, 32 bytes (D3DDevice_CreateIndexBuffer, 0x826CF868): the
// data's VA (DrawIndexedVertices, Destroy) and its length in bytes. Common's
// kCommonIndex32 marks 32-bit indices.
struct D3DIndexBuffer {
  D3DResource resource; // +0x00
  be_u32 Address;       // +0x18
  be_u32 Size;          // +0x1C
};
static_assert(sizeof(D3DIndexBuffer) == 32);

// Shaders are resources in 6132. A pixel shader's header is 40 bytes with the
// physical microcode at +0x18; a vertex shader's is 872 bytes with it at
// +0x20 (XGRegisterVertexShader, Destroy). Behind the fixed part, both keep a
// table of the shader's literal constants, which the draw-time flush loads
// with LOAD_ALU_CONSTANT straight from the microcode (sub_826DEBD8), never
// through the float-constant shadow. XenosRecomp bakes those literals into
// the HLSL, so the renderer does not need them.

// The microcode container the creators and XGGetMicrocodeShaderParts are
// handed (revision 0x11), layout mirrors
// thirdparty/XenosRecomp/XenosRecomp/shader.h.
struct ShaderContainer {
  be_u32 flags;
  be_u32 virtualSize;
  be_u32 physicalSize;
  be_u32 fieldC;
  be_u32 constantTableOffset;
  be_u32 definitionTableOffset;
  be_u32 shaderOffset;
  be_u32 field1C;
  be_u32 field20;
};
static_assert(sizeof(ShaderContainer) == 0x24);

// Direct3D_CreateDevice (0x826CD270) allocates this through D3D_AllocDevice,
// 128-byte aligned, with the allocation's own address in the dword before it
// (D3DDevice_Release frees through that). 2099: 0x5080.
constexpr u32 kD3DDeviceAllocSize = 0x5780;

// Direct3D_CreateDevice's DeviceType for a device that records into a
// command buffer instead of the ring. The vehicle renderer makes one
// (sub_82423218) unless Vehicle.DisableCommandBuffers is set.
constexpr u32 kD3DDevTypeCommandBuffer = 2;

// The game tweakable Vehicle.DisableCommandBuffers (object 0x82AAABC4, value
// byte at +32). Its default is 1: vehicles draw through the device rather
// than recording a command buffer and running it.
constexpr u32 kVehicleDisableCommandBuffersGuestAddr = 0x82AAABE4;

// dword[0] bits [1:0] of a fetch constant tag what it describes.
constexpr u32 kFetchTypeMask = 0x3;
constexpr u32 kFetchTypeTexture = 2;
constexpr u32 kFetchTypeVertex = 3;

// 24-byte Xenos GPU fetch descriptor. D3DDevice holds 32 in fetchConstants[].
struct DeviceFetchConstant {
  be_u32 dword[6];
};
static_assert(sizeof(DeviceFetchConstant) == 24);

// Render and sampler state dispatch, {Get, Set, Default} per entry.
// D3D__InitializeStateTables (0x826DA5B0) copies Set/Get into the device and
// calls Set(Default). Render states 0-9 have a getter returning 0 and an
// empty setter; 10 is D3DRS_ZENABLE (value 40).
constexpr u32 kRenderStateTableGuestAddr = 0x82A640D0;
constexpr u32 kRenderStateTableEntries = 101;
constexpr u32 kSamplerStateTableGuestAddr = 0x82A64590;
constexpr u32 kSamplerStateTableEntries = 20;
constexpr u32 kSamplerCount = 26;

// Shared guest memory. Only fields with a citation in config/d3d.toml are
// named. Everything else is padding until a function proves its meaning.
//
// The draw-time flush writes each register group from one block, selected by
// the dirty masks:
//   m_Mask[0]                   VS float constants (one bit per 4)
//   m_Mask[1]                   PS float constants
//   m_Mask[2] bits 42-57        0x2000-0x200F from +0x2880
//             bits 21-41        0x2100-0x2114 from +0x28CC
//             bits 12-16        0x2180-0x2184 from +0x2920
//             bits 0-11         0x2200-0x220B from +0x2934
//             bits 17-20        derived state (sub_826DEDC8): shaders,
//                               declaration, depth and HiZ
//   m_Mask[3] bits 0-31         fetch constants (sub_826DDED0)
//             bits 34-54        0x2280-0x2294 from +0x2964
//   m_Mask[4] bits 0-37         0x2300-0x2325 from +0x29B8
//             bits 38-45        0x2380-0x2387 from +0x2A50
//             bits 49-54        user clip planes from +0x2820 (sub_826DDD50)
//             bit 56            bool and loop constants from +0x2780
//   m_Mask[5]                   ANDed with m_Mask[2]: the states
//                               sub_826DD8F8 rewrites for tiling and HiZ
//                               (RB_HIZCONTROL among them)
// Register r of a group sits at block + 4 * (r - first), as the named fields
// below show. The window group (0x2080-0x2082) is not on a mask:
// sub_826C33C0 writes it into the ring at once, one rect per tile when tiling.
struct D3DDevice {
  be_u64 m_Mask[6];        // +0x000  dirty masks the Set* functions OR into
  be_u32 m_pRing;          // +0x030  write pointer; D3D__MakeSpace returns it
  be_u32 m_pRingEnd;       // +0x034  hard end of the segment
  be_u32 m_pRingLimit;     // +0x038  end - 160; draws compare against it
  be_u32 m_ReferenceCount; // +0x03C  D3DDevice_Release
  be_u32 m_SetRenderStateCall[kRenderStateTableEntries];   // +0x040
  be_u32 m_SetSamplerStateCall[kSamplerStateTableEntries]; // +0x1D4
  be_u32 m_GetRenderStateCall[kRenderStateTableEntries];   // +0x224
  be_u32 m_GetSamplerStateCall[kSamplerStateTableEntries]; // +0x3B8
  u8 pad_408[0x480 - 0x408];
  // D3DDevice_SetTexture (sampler s -> slot s) and SetStreamSource (stream n
  // -> vertex fetch constant 95 - n, the third in slot 31 - n / 3).
  DeviceFetchConstant fetchConstants[32]; // +0x480
  be_f32 vsFloatConstants[256][4];        // +0x780  SetVertexShaderConstantFN
  be_f32 psFloatConstants[256][4];        // +0x1780 SetPixelShaderConstantFN
  // The bool and loop constants, in register order (SHADER_CONSTANT_BOOL
  // 0x4900-0x4907, then LOOP 0x4908-0x4927), as the flush uploads them. Bool
  // register n of the unified file (VS b0-b127, then PS b0-b127) is bit
  // n % 32 of dword n / 32. Loop constants: VS i0-i15, then PS (+0x27E0).
  be_u32 boolConstants[8];  // +0x2780
  be_u32 loopConstants[32]; // +0x27A0
  be_f32 clipPlanes[6][4];  // +0x2820  D3DDevice_SetClipPlane
  // 0x2000 group: RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO,
  // RB_COLOR1-3_INFO, then COHER_DEST_BASE_0-7 and PA_SC_SCREEN_SCISSOR_TL/BR.
  be_u32 rbSurfaceInfo;       // +0x2880  RB_SURFACE_INFO (0x2000)
  be_u32 rbColorInfo;         // +0x2884  RB_COLOR_INFO (0x2001)
  be_u32 rbDepthInfo;         // +0x2888  RB_DEPTH_INFO (0x2002)
  be_u32 rbColor1Info[3];     // +0x288C  RB_COLOR1-3_INFO (0x2003-0x2005)
  u8 pad_2898[0x28C4 - 0x2898];
  // PA_SC_WINDOW_SCISSOR_TL/BR (0x2081/0x2082): x in bits 0-14, y in 16-30,
  // BR exclusive. D3DDevice_SetScissorRect stores the viewport rectangle,
  // clipped to the scissor rect when D3DRS_SCISSORTESTENABLE is on, and has
  // sub_826C33C0 write it.
  be_u32 paScWindowScissorTL; // +0x28C4
  be_u32 paScWindowScissorBR; // +0x28C8
  // 0x2100 group.
  be_u32 vgtMaxVtxIndx;          // +0x28CC  VGT_MAX_VTX_INDX (0x2100)
  be_u32 vgtMinVtxIndx;          // +0x28D0  VGT_MIN_VTX_INDX (0x2101)
  be_u32 indexOffset;            // +0x28D4  VGT_INDX_OFFSET (0x2102): the draws write it
  be_u32 vgtMultiPrimIbResetIndx; // +0x28D8 (0x2103) D3DRS_PRIMITIVERESETINDEX
  be_u32 rbColorMask;            // +0x28DC  RB_COLOR_MASK (0x2104)
  be_f32 rbBlendColor[4];        // +0x28E0  RB_BLEND_RED..ALPHA (0x2105-0x2108), D3DRS_BLENDFACTOR
  u8 pad_28F0[0x28FC - 0x28F0];
  be_u32 rbStencilRefMaskBF;     // +0x28FC  RB_STENCILREFMASK_BF (0x210C)
  be_u32 rbStencilRefMask;       // +0x2900  RB_STENCILREFMASK (0x210D)
  be_f32 rbAlphaRef;             // +0x2904  RB_ALPHA_REF (0x210E), 0..1
  // PA_CL_VPORT_{X,Y,Z}{SCALE,OFFSET} (0x210F-0x2114), in register order.
  be_f32 viewportScaleOffset[6]; // +0x2908
  // 0x2180 group.
  be_u32 sqProgramCntl;       // +0x2920  SQ_PROGRAM_CNTL (0x2180)
  be_u32 sqContextMisc;       // +0x2924  SQ_CONTEXT_MISC (0x2181)
  be_u32 sqInterpolatorCntl;  // +0x2928  SQ_INTERPOLATOR_CNTL (0x2182)
  be_u32 sqWrapping[2];       // +0x292C  SQ_WRAPPING_0/1 (0x2183/0x2184), D3DRS_WRAP0-15
  // 0x2200 group.
  be_u32 rbDepthControl;      // +0x2934  RB_DEPTHCONTROL (0x2200)
  be_u32 rbBlendControl0;     // +0x2938  RB_BLENDCONTROL0 (0x2201)
  be_u32 rbColorControl;      // +0x293C  RB_COLORCONTROL (0x2202)
  be_u32 rbHiZControl;        // +0x2940  RB_HIZCONTROL (0x2203)
  // PA_CL_CLIP_CNTL (0x2204): bits 0-5 enable user clip planes 0-5.
  be_u32 paClClipCntl;        // +0x2944
  be_u32 paSuScModeCntl;      // +0x2948  PA_SU_SC_MODE_CNTL (0x2205)
  // PA_CL_VTE_CNTL (0x2206), written by D3DRS_VIEWPORTENABLE (0x826C4908).
  be_u32 paClVteCntl;         // +0x294C
  u8 pad_2950[0x2954 - 0x2950];
  be_u32 rbModeControl;       // +0x2954  RB_MODECONTROL (0x2208)
  be_u32 rbBlendControl1[3];  // +0x2958  RB_BLENDCONTROL1..3 (0x2209..)
  // 0x2280 group: PA_SU_POINT_SIZE (0x2280) to VGT_ENHANCE (0x2294).
  be_u32 paSuPointSize;       // +0x2964  PA_SU_POINT_SIZE (0x2280)
  u8 pad_2968[0x29B8 - 0x2968];
  // 0x2300 group: PA_SC_LINE_CNTL (0x2300) to RB_SAMPLE_COUNT_ADDR (0x2325).
  be_u32 paScLineCntl;        // +0x29B8  PA_SC_LINE_CNTL (0x2300), D3DRS_LASTPIXEL
  be_u32 paScAaConfig;        // +0x29BC  PA_SC_AA_CONFIG (0x2301)
  // Bit 0 is PA_SU_VTX_CNTL.PIX_CENTER, which D3DRS_HALFPIXELOFFSET
  // (0x826C4CC0) writes.
  be_u32 paSuVtxCntl;         // +0x29C0  PA_SU_VTX_CNTL (0x2302)
  // PA_CL_GB_{VERT,HORZ}_{CLIP,DISC}_ADJ (0x2303-0x2306): D3DRS_GUARDBAND_Y,
  // DISCARDBAND_Y, GUARDBAND_X, DISCARDBAND_X.
  be_f32 paClGbAdj[4];        // +0x29C4
  u8 pad_29D4[0x2A00 - 0x29D4];
  be_u32 paScAaMask;          // +0x2A00  PA_SC_AA_MASK (0x2312), D3DRS_MULTISAMPLEMASK
  u8 pad_2A04[0x2A50 - 0x2A04];
  // 0x2380 group. PA_SU_POLY_OFFSET_{FRONT,BACK}_{SCALE,OFFSET}:
  // D3DRS_SLOPESCALEDEPTHBIAS (0x826C41C8) and D3DRS_DEPTHBIAS (0x826C4290).
  be_f32 polyOffsetFrontScale;  // +0x2A50
  be_f32 polyOffsetFrontOffset; // +0x2A54
  be_f32 polyOffsetBackScale;   // +0x2A58
  be_f32 polyOffsetBackOffset;  // +0x2A5C
  u8 pad_2A60[0x2A90 - 0x2A60];
  be_u32 fenceReadback;       // +0x2A90  pointer to the GPU's fence write-back (IsFencePending)
  u8 pad_2A94[0x2A9C - 0x2A94];
  be_u32 currentFence;        // +0x2A9C  InsertFence returns it; unbinding stamps it into resources
  u8 pad_2AA0[0x2AB0 - 0x2AA0];
  be_u32 lastInsertedFence;   // +0x2AB0
  u8 pad_2AB4[0x2E24 - 0x2AB4];
  be_u32 vertexDeclaration;   // +0x2E24  D3DDevice_SetVertexDeclaration (2099: +0x2C90)
  u8 pad_2E28[0x2E40 - 0x2E28];
  // Blend as set through D3DRS (SRCBLEND..BLENDOPALPHA), applied to the four
  // RB_BLENDCONTROLs only while ALPHABLENDENABLE (blendFlags bit 31) is on;
  // off, they get 0x00010001. Bit 30 is SEPARATEALPHABLENDENABLE.
  be_u32 blendStaged;         // +0x2E40
  be_u32 blendFlags;          // +0x2E44
  u8 pad_2E48[0x308C - 0x2E48];
  be_u32 indexBuffer;         // +0x308C  D3DDevice_SetIndices
  be_u32 renderTargets[4];    // +0x3090  D3DDevice_SetRenderTarget
  be_u32 depthStencil;        // +0x30A0  D3DDevice_SetDepthStencilSurface
  // Vertex buffer per stream. The stream offset has no field of its own in
  // 6132: SetStreamSource folds it into the vertex fetch constant.
  be_u32 streams[16];         // +0x30A4
  u8 pad_30E4[0x30E8 - 0x30E4];
  u8 streamStrides[16];       // +0x30E8  stride / 4
  be_u32 textures[kSamplerCount]; // +0x30F8  D3DDevice_SetTexture
  D3DViewportF viewport;      // +0x3160  sub_826C6100
  D3DRect scissorRect;        // +0x317C  D3DDevice_SetScissorRect
  be_u32 pixelShader;         // +0x318C  D3DDevice_SetPixelShader
  be_u32 vertexShader;        // +0x3190  D3DDevice_SetVertexShader (2099: +0x4FD8)
  u8 pad_3194[0x31C4 - 0x3194];
  // Predicated tiling. D3DDevice_BeginTiling stores the tile count here and
  // copies its D3DRECTs from +0x31C8; the render targets and depth it was
  // given are kept at +0x31B0..+0x31C0 for ClearF and SetViewport to compare.
  be_u32 tileCount;           // +0x31C4
  u8 pad_31C8[0x5708 - 0x31C8];
  be_u32 behaviorFlags;       // +0x5708  Direct3D_CreateDevice (2099: +0x5050)
  u8 pad_570C[0x5780 - 0x570C];
};
static_assert(sizeof(D3DDevice) == kD3DDeviceAllocSize);
static_assert(offsetof(D3DDevice, m_pRing) == 0x030);
static_assert(offsetof(D3DDevice, m_ReferenceCount) == 0x03C);
static_assert(offsetof(D3DDevice, m_SetRenderStateCall) == 0x040);
static_assert(offsetof(D3DDevice, m_SetSamplerStateCall) == 0x1D4);
static_assert(offsetof(D3DDevice, m_GetRenderStateCall) == 0x224);
static_assert(offsetof(D3DDevice, m_GetSamplerStateCall) == 0x3B8);
static_assert(offsetof(D3DDevice, fetchConstants) == 0x480);
static_assert(offsetof(D3DDevice, vsFloatConstants) == 0x780);
static_assert(offsetof(D3DDevice, psFloatConstants) == 0x1780);
static_assert(offsetof(D3DDevice, boolConstants) == 0x2780);
static_assert(offsetof(D3DDevice, loopConstants) == 0x27A0);
static_assert(offsetof(D3DDevice, clipPlanes) == 0x2820);
static_assert(offsetof(D3DDevice, rbSurfaceInfo) == 0x2880);
static_assert(offsetof(D3DDevice, rbDepthInfo) == 0x2888);
static_assert(offsetof(D3DDevice, paScWindowScissorTL) == 0x28C4);
static_assert(offsetof(D3DDevice, vgtMaxVtxIndx) == 0x28CC);
static_assert(offsetof(D3DDevice, indexOffset) == 0x28D4);
static_assert(offsetof(D3DDevice, rbColorMask) == 0x28DC);
static_assert(offsetof(D3DDevice, rbStencilRefMask) == 0x2900);
static_assert(offsetof(D3DDevice, rbAlphaRef) == 0x2904);
static_assert(offsetof(D3DDevice, viewportScaleOffset) == 0x2908);
static_assert(offsetof(D3DDevice, sqProgramCntl) == 0x2920);
static_assert(offsetof(D3DDevice, sqWrapping) == 0x292C);
static_assert(offsetof(D3DDevice, rbDepthControl) == 0x2934);
static_assert(offsetof(D3DDevice, rbColorControl) == 0x293C);
static_assert(offsetof(D3DDevice, paClClipCntl) == 0x2944);
static_assert(offsetof(D3DDevice, paSuScModeCntl) == 0x2948);
static_assert(offsetof(D3DDevice, paClVteCntl) == 0x294C);
static_assert(offsetof(D3DDevice, rbModeControl) == 0x2954);
static_assert(offsetof(D3DDevice, rbBlendControl1) == 0x2958);
static_assert(offsetof(D3DDevice, paSuPointSize) == 0x2964);
static_assert(offsetof(D3DDevice, paScLineCntl) == 0x29B8);
static_assert(offsetof(D3DDevice, paSuVtxCntl) == 0x29C0);
static_assert(offsetof(D3DDevice, paScAaMask) == 0x2A00);
static_assert(offsetof(D3DDevice, polyOffsetFrontScale) == 0x2A50);
static_assert(offsetof(D3DDevice, currentFence) == 0x2A9C);
static_assert(offsetof(D3DDevice, vertexDeclaration) == 0x2E24);
static_assert(offsetof(D3DDevice, blendFlags) == 0x2E44);
static_assert(offsetof(D3DDevice, indexBuffer) == 0x308C);
static_assert(offsetof(D3DDevice, renderTargets) == 0x3090);
static_assert(offsetof(D3DDevice, depthStencil) == 0x30A0);
static_assert(offsetof(D3DDevice, streams) == 0x30A4);
static_assert(offsetof(D3DDevice, streamStrides) == 0x30E8);
static_assert(offsetof(D3DDevice, textures) == 0x30F8);
static_assert(offsetof(D3DDevice, viewport) == 0x3160);
static_assert(offsetof(D3DDevice, scissorRect) == 0x317C);
static_assert(offsetof(D3DDevice, pixelShader) == 0x318C);
static_assert(offsetof(D3DDevice, vertexShader) == 0x3190);
static_assert(offsetof(D3DDevice, tileCount) == 0x31C4);
static_assert(offsetof(D3DDevice, behaviorFlags) == 0x5708);

// xboxkrnl VdGlobalDevice, imported at this address in the patched image (it
// resolves to the same kernel variable as pgr3's 0x820005A0). D3D's resource
// code reads the device back through it (D3DResource_Destroy).
constexpr u32 kVdGlobalDeviceImport = 0x82000898;

constexpr u32 kClearTarget = 0x1u;
constexpr u32 kClearZBuffer = 0x10u;
constexpr u32 kClearStencil = 0x20u;
constexpr u32 kClearAll = kClearTarget | kClearZBuffer | kClearStencil;

constexpr u32 kResolveDepthStencil = 0x4u;
constexpr u32 kResolveExponentBiasShift = 26;

} // namespace pgr4::gpu
