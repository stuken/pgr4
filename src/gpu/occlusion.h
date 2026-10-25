// pgr4 - ReXGlue Recompiled Project
//
// Guest occlusion queries on host occlusion queries. The guest brackets its
// test draws with D3DQuery_Issue BEGIN and END; every host draw recorded in
// between gets a host query of its own (so no query spans a render pass on
// Vulkan), and the guest query's count is their sum. A frame slot's queries
// are resolved at the end of its list and read after its fence, so a result
// arrives a frame or two after the issue and GetData answers "not yet" until
// then, as on the 360. D3D12 only for now: on Vulkan every query still
// answers visible (see occlusion.cpp).
//
// PGR4's light glows (sub_8233F890 issues, sub_8233F708 reads) fade each
// light's glow in while its query passes any samples and out while none do;
// the glows themselves draw with no depth test.

#pragma once

#include <rex/types.h>

namespace pgr4::gpu {
struct VideoState;
} // namespace pgr4::gpu

namespace pgr4::gpu::occlusion {

bool Enabled();

// D3DQuery_Issue of an occlusion query: BEGIN starts counting the draws that
// follow for 'query', END stops.
void Issue(u32 query, bool begin, bool end);

// Around one guest draw's host draws, under the video state mutex.
void DrawBegin(VideoState &s);
void DrawEnd(VideoState &s);
// After the guest render state is read, before the pipeline is bound: makes
// a draw that will be counted run its pixel shader when only the shader's
// invocations can count it (see occlusion.cpp).
void KeepPixelShader(VideoState &s);

// The recording slot's list has just begun / is about to end.
void FrameBegin(VideoState &s);
void FrameEnd(VideoState &s);
// When the slot comes round again, after its fence if its list was
// submitted: the queries it carried get their results (0 samples if it was
// not).
void Collect(u32 slot, bool executed);

// D3DQuery_GetData: false while the query's result is in flight. A query
// never issued reads 0 samples.
bool Result(u32 query, u32 &samples);

} // namespace pgr4::gpu::occlusion
