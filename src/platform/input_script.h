// pgr4 - ReXGlue Recompiled Project

#pragma once

#include <rex/types.h>

namespace pgr4::script {

// What a pgr4_input_script "dump@" or "trace@" entry asks for: a frame
// written as pgr4_dump_frames does, or traced as pgr4_trace_frame does.
// Timed like a press, it can follow a wait, so a capture lands on the same
// scene whatever the frame rate.
enum class Capture { Dump = 0, Trace = 1 };

// Whether a script entry asked for present frame 'frame' to be captured so.
bool CaptureWanted(Capture kind, u32 frame);

} // namespace pgr4::script
