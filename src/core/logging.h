// pgr4 - ReXGlue Recompiled Project
//
// PGR4_* logging macros bound to the pgr4 log category. Ported from reblue's
// core/logging.h (Copyright (c) 2026 Tom Clay, BSD 3-Clause).

#pragma once

#include <atomic>

#include <rex/logging.h>

#include "core/settings.h"

REXLOG_DEFINE_CATEGORY(pgr4)

#define PGR4_TRACE(...) REXLOG_CAT_TRACE(::rex::log::pgr4(), __VA_ARGS__)
#define PGR4_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::pgr4(), __VA_ARGS__)
#define PGR4_INFO(...) REXLOG_CAT_INFO(::rex::log::pgr4(), __VA_ARGS__)
#define PGR4_WARN(...) REXLOG_CAT_WARN(::rex::log::pgr4(), __VA_ARGS__)
#define PGR4_ERROR(...) REXLOG_CAT_ERROR(::rex::log::pgr4(), __VA_ARGS__)
#define PGR4_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::pgr4(), __VA_ARGS__)

#define PGR4_DEV_WARN(...)                                                      \
  do {                                                                         \
    if (::pgr4::Settings::Get().Devmode())                                      \
      PGR4_WARN(__VA_ARGS__);                                                   \
  } while (0)

#define PGR4_DEV_INFO(...)                                                      \
  do {                                                                         \
    if (::pgr4::Settings::Get().Devmode())                                      \
      PGR4_INFO(__VA_ARGS__);                                                   \
  } while (0)

// Warns on the first `limit` hits of one call site. Marks paths the renderer
// doesn't model yet, so a run shows which ones the game reaches.
#define PGR4_WARN_FIRST(limit, ...)                                             \
  do {                                                                         \
    static std::atomic<unsigned> s_pgr4_warned{0};                              \
    if (s_pgr4_warned.fetch_add(1, std::memory_order_relaxed) < (limit))        \
      PGR4_WARN(__VA_ARGS__);                                                   \
  } while (0)

// The same at the debug level, for paths that are modelled but worth seeing
// while debugging.
#define PGR4_DEBUG_FIRST(limit, ...)                                            \
  do {                                                                         \
    static std::atomic<unsigned> s_pgr4_noted{0};                               \
    if (s_pgr4_noted.fetch_add(1, std::memory_order_relaxed) < (limit))         \
      PGR4_DEBUG(__VA_ARGS__);                                                  \
  } while (0)
