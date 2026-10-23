// pgr4 - ReXGlue Recompiled Project
//
// App-wide settings the renderer reads. reblue keeps these in a full settings
// model with a UI; pgr4 starts with plain cvars and grows this as needed.

#pragma once

#include <rex/types.h>

// Group every pgr4 cvar is registered under. Global: the REXCVAR_DEFINE_*
// macros expand at namespace scope.
inline constexpr char kCvarGroup[] = "Project Gotham Racing 4";

namespace pgr4 {

class Settings {
public:
  static Settings &Get();

  // Extra diagnostics and PGR4_DEV_* logging.
  bool Devmode() const;
  // How long a quit may take before the watchdog terminates the process.
  i32 ShutdownTimeoutMs() const;
};

} // namespace pgr4
