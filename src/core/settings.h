// pgr4 - ReXGlue Recompiled Project
//
// App-wide settings. reblue keeps these in a full settings model with a UI;
// pgr3 and pgr4 start with plain cvars and grow this as needed.

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
};

} // namespace pgr4
