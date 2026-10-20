// pgr4 - ReXGlue Recompiled Project

#include "core/settings.h"

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(pgr4_devmode, false, kCvarGroup,
                    "Extra renderer diagnostics and developer logging");

namespace pgr4 {

Settings &Settings::Get() {
  static Settings settings;
  return settings;
}

bool Settings::Devmode() const { return REXCVAR_GET(pgr4_devmode); }

} // namespace pgr4
