// pgr4 - ReXGlue Recompiled Project

#pragma once

#include <string>

namespace pgr4::gpu {

struct VideoState;
struct GuestTexture;

// Called once per presented frame with the frame's source render target, after
// its command list was submitted. Dumps it when pgr4_dump_frames names this
// frame. Caller holds s.mutex.
void MaybeDumpFrame(VideoState &s, GuestTexture *rt);

// pgr4_dump_resolve_sources: in a frame pgr4_dump_frames names, keeps a copy of
// src as it is now, written out with the frame as ..._sK_<name>.bmp. Takes
// s.mutex.
void SnapshotForFrameDump(GuestTexture *src, const std::string &name);

} // namespace pgr4::gpu
