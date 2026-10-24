// pgr4 - ReXGlue Recompiled Project
//
// pgr4_perf_gpu: GPU time per pass, reported with pgr4_perf_stats. A timestamp
// goes down when a frame's command list begins, at every framebuffer bind and
// host copy, and at the end of the frame. Each interval is charged to the mark
// that began it, labelled by what it was and its target's host size, format
// and sample count, so a pass interrupted by host copies shows as several
// intervals under the same label.

#pragma once

#include <string>

#include <rex/types.h>

namespace pgr4::gpu {
struct VideoState;
struct GuestTexture;
} // namespace pgr4::gpu

namespace pgr4::gpu::perf {

bool GpuTimingEnabled();

// The recording slot's list has just begun.
void GpuFrameBegin(VideoState &s);
// What follows on the open list, until the next mark. 'what' must be a string
// literal: labels are keyed by its address.
void GpuMark(VideoState &s, const char *what, const GuestTexture *target);
// The last timestamp, before the list ends.
void GpuFrameEnd(VideoState &s);
// After the slot's fence: charges its intervals.
void GpuCollect(u32 slot);
// The totals since the last report, per frame of 'frames', heaviest first;
// resets them. Empty when nothing was timed.
std::string GpuReport(u64 frames);
// Video memory in use against the OS budget, local (VRAM, or an iGPU's
// carve-out) and on D3D12 non-local (shared system memory). Past the local
// budget the OS pages allocations between submissions, which no timestamp
// inside a command list sees.
std::string MemoryReport();

} // namespace pgr4::gpu::perf
