// pgr4 - ReXGlue Recompiled Project

#pragma once

namespace pgr4::gpu {

struct VideoState;
struct GuestTexture;

// Called once per presented frame with the frame's source render target, after
// its command list was submitted. Dumps it when pgr4_dump_frames names this
// frame. Caller holds s.mutex.
void MaybeDumpFrame(VideoState &s, GuestTexture *rt);

} // namespace pgr4::gpu
