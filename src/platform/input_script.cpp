// pgr4 - ReXGlue Recompiled Project
//
// pgr4_input_script: scripted controller input for unattended testing, e.g.
//   --pgr4_input_script="start@900,a@1000+5,rt@1100+600"
// Each entry presses a button (or pulls a trigger) on present frame N and
// holds it for the given number of frames (default 6). A trailing 's' on
// either number counts seconds since the game first polled the pad instead.
// The xenos plugin build counts frames by wrapping the guest's
// D3DDevice_Swap (the game's frame end, sub_822B3B08, resolves and calls Swap
// itself, as PGR3's does; Swap has three other callers) and logs every 300th,
// so captures can be timed against the native renderer's frame numbers.
// Buttons: a b x y start
// back up down left right lb rb ls rs; triggers: lt rt. Applied to user 0 on
// top of whatever a real pad reports, and while a script is set a missing pad
// reads as connected, since remote sessions usually have none.
//
// A "wait:<event>#<n>" entry waits for the event's n-th occurrence (n
// defaults to 1). The entries after it count from that moment instead of from
// the first poll, and the entries before it that have not started by then are
// dropped. Load times vary from run to run, so a fixed time can land on a
// dialog that swallows the press; this lets a script dismiss the boot screens
// with spare presses and then walk the menus from a known point. Events:
//   content  XContentCreateEnumerator. The first call is at boot; the second
//            is the front end's content check, about a second before the main
//            menu appears ("wait:content#2").
//
// "dump@<when>" and "trace@<when>" press nothing: they dump (as
// pgr4_dump_frames does) or trace (as pgr4_trace_frame does) the first whole
// frame after that moment. The frame rate varies from run to run, so a
// capture timed in the script's own seconds lands on the scene it means to.
// "capture@<when>" asks RenderDoc, when the game runs under it, for a capture
// of the next frame presented.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <renderdoc/renderdoc_app.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include "platform/input_script.h"

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/settings.h"

REXCVAR_DEFINE_STRING(pgr4_input_script, "", kCvarGroup,
                      "Scripted pad input for testing: "
                      "button@frame[+hold],...,wait:event#n,...");

// Unattended runs over remote sessions can lose the audio endpoint, and the
// guest faults at boot when audio init fails. This swaps the SDL
// backend for a silent one (platform/silent_audio.h, read in Pgr4App::OnPreSetup).
REXCVAR_DEFINE_BOOL(pgr4_null_audio, false, kCvarGroup,
                    "Discard audio output (testing without an audio device)");

REX_EXTERN(__imp__XInputGetState);
REX_EXTERN(__imp__XContentCreateEnumerator);

namespace {
std::atomic<u32> g_content_enumerators{0};
}

REX_HOOK_RAW(XContentCreateEnumerator) {
  g_content_enumerators.fetch_add(1, std::memory_order_relaxed);
  __imp__XContentCreateEnumerator(ctx, base);
}

#if defined(PGR4_NATIVE_GPU)
namespace pgr4::gpu {
u32 PresentCount();
}
#else
REX_EXTERN(__imp__D3DDevice_Swap);
REXCVAR_DECLARE(std::string, pgr4_dump_frames);

namespace {
std::atomic<u32> g_presents{0};
}

void Pgr4XenosDumpOnPresent(); // platform/xenos_resolve_dump.cpp

REX_HOOK_RAW(D3DDevice_Swap) {
  const u32 n = g_presents.fetch_add(1, std::memory_order_relaxed) + 1;
  // Only while a tool that names present frames is on.
  if (n % 300 == 0 && (!REXCVAR_GET(pgr4_input_script).empty() ||
                       !REXCVAR_GET(pgr4_dump_frames).empty()))
    PGR4_INFO("pgr4_input_script: present {}", n);
  Pgr4XenosDumpOnPresent();
  __imp__D3DDevice_Swap(ctx, base);
}
#endif

namespace {

struct Press {
  u16 buttons = 0;
  u8 left_trigger = 0;
  u8 right_trigger = 0;
  i16 thumb_lx = 0;
  double start = 0; // frames, or seconds when 'seconds'
  double hold = 6;
  bool seconds = false;
  size_t segment = 0; // the wait it follows (Segments()), 0 for none
  // dump@ and trace@ entries press nothing: they ask for a capture of the
  // next whole frame (pgr4::script::CaptureWanted).
  int capture = -1;
};

// A capture@ entry's 'capture': no frame list, RenderDoc picks the frame.
constexpr int kRenderDocCapture = 2;

// The frames dump@ and trace@ entries asked for, by pgr4::script::Capture.
std::mutex g_capture_mutex;
std::vector<u32> g_capture_frames[2];
std::atomic<bool> g_any_capture{false};

// RenderDoc's in-application API when the game was launched under it.
void TriggerRenderDocCapture() {
#if defined(_WIN32)
  static RENDERDOC_API_1_0_1 *api = [] {
    RENDERDOC_API_1_0_1 *out = nullptr;
    if (HMODULE mod = GetModuleHandleA("renderdoc.dll")) {
      auto get = reinterpret_cast<pRENDERDOC_GetAPI>(
          GetProcAddress(mod, "RENDERDOC_GetAPI"));
      if (!get || !get(eRENDERDOC_API_Version_1_0_1,
                       reinterpret_cast<void **>(&out)))
        out = nullptr;
    }
    return out;
  }();
  if (api) {
    api->TriggerCapture();
    PGR4_INFO("pgr4_input_script: RenderDoc capture triggered");
    return;
  }
#endif
  PGR4_WARN("pgr4_input_script: capture@ without RenderDoc attached");
}

// A wait entry and the entries after it. Segment 0 starts at the first poll.
struct Segment {
  const std::atomic<u32> *counter = nullptr;
  u32 count = 0;
  bool started = false;
  double origin_seconds = 0;
  double origin_frame = 0;
};

// XINPUT_STATE.
struct InputState {
  be_u32 dwPacketNumber;
  be_u16 wButtons;
  u8 bLeftTrigger;
  u8 bRightTrigger;
  be_i16 sThumbLX;
  be_i16 sThumbLY;
  be_i16 sThumbRX;
  be_i16 sThumbRY;
};

bool ParseButton(const std::string &name, Press &p) {
  static constexpr struct {
    const char *name;
    u16 mask;
  } kButtons[] = {{"up", 0x0001},     {"down", 0x0002}, {"left", 0x0004},
                  {"right", 0x0008},  {"start", 0x0010}, {"back", 0x0020},
                  {"ls", 0x0040},     {"rs", 0x0080},   {"lb", 0x0100},
                  {"rb", 0x0200},     {"a", 0x1000},    {"b", 0x2000},
                  {"x", 0x4000},      {"y", 0x8000}};
  for (const auto &b : kButtons) {
    if (name == b.name) {
      p.buttons = b.mask;
      return true;
    }
  }
  if (name == "lt") {
    p.left_trigger = 255;
    return true;
  }
  if (name == "rt") {
    p.right_trigger = 255;
    return true;
  }
  if (name == "lsleft" || name == "lsright") {
    p.thumb_lx = name == "lsleft" ? -32767 : 32767;
    return true;
  }
  if (name == "dump" || name == "trace") {
    p.capture = int(name == "dump" ? pgr4::script::Capture::Dump
                                   : pgr4::script::Capture::Trace);
    return true;
  }
  if (name == "capture") {
    p.capture = kRenderDocCapture;
    return true;
  }
  return false;
}

// "content#2" -> the counter it waits on and the count; false for an unknown
// event.
bool ParseWait(const std::string &spec, Segment &s) {
  const size_t hash = spec.find('#');
  const std::string event = spec.substr(0, hash);
  if (event == "content")
    s.counter = &g_content_enumerators;
  else
    return false;
  s.count = hash == std::string::npos ? 1u : u32(std::stoul(spec.substr(hash + 1)));
  return s.count > 0;
}

struct ParsedScript {
  std::vector<Press> presses;
  std::vector<Segment> segments{1}; // origins are set while polling
};

ParsedScript &Script() {
  static std::once_flag once;
  static ParsedScript script;
  std::call_once(once, [] {
    auto &presses = script.presses;
    const std::string text = REXCVAR_GET(pgr4_input_script);
    size_t pos = 0;
    while (pos < text.size()) {
      size_t end = text.find(',', pos);
      if (end == std::string::npos)
        end = text.size();
      const std::string entry = text.substr(pos, end - pos);
      pos = end + 1;
      if (entry.rfind("wait:", 0) == 0) {
        Segment s;
        if (ParseWait(entry.substr(5), s))
          script.segments.push_back(s);
        else
          PGR4_WARN("pgr4_input_script: unknown wait '{}'", entry);
        continue;
      }
      const size_t at = entry.find('@');
      if (at == std::string::npos)
        continue;
      Press p;
      if (!ParseButton(entry.substr(0, at), p)) {
        PGR4_WARN("pgr4_input_script: unknown button in '{}'", entry);
        continue;
      }
      const size_t plus = entry.find('+', at);
      const std::string when = entry.substr(at + 1, plus - at - 1);
      p.seconds = !when.empty() && when.back() == 's';
      p.start = std::stod(when);
      if (plus != std::string::npos)
        p.hold = std::stod(entry.substr(plus + 1));
      else if (p.seconds)
        p.hold = 0.2;
      p.segment = script.segments.size() - 1;
      presses.push_back(p);
    }
    if (!presses.empty())
      PGR4_INFO("pgr4_input_script: {} scripted press(es), {} wait(s)",
                presses.size(), script.segments.size() - 1);
  });
  return script;
}

} // namespace

bool pgr4::script::CaptureWanted(Capture kind, u32 frame) {
  // Asked per draw while tracing is possible: no lock until a capture exists.
  if (!g_any_capture.load(std::memory_order_acquire))
    return false;
  std::lock_guard lock(g_capture_mutex);
  const auto &frames = g_capture_frames[int(kind)];
  return std::find(frames.begin(), frames.end(), frame) != frames.end();
}

// (user, state) -> ERROR_SUCCESS / ERROR_DEVICE_NOT_CONNECTED
REX_HOOK_RAW(XInputGetState) {
  const u32 user = ctx.r3.u32;
  const u32 state_va = ctx.r4.u32;
  __imp__XInputGetState(ctx, base);
  auto &parsed = Script();
  const auto &script = parsed.presses;
  if (script.empty() || user != 0)
    return;
  auto *state = pgr4::mem::at<InputState>(state_va);
  if (!state)
    return;
  if (ctx.r3.u32 != 0) {
    // No pad: report a neutral connected one so the script has a target.
    std::memset(state, 0, sizeof(InputState));
    ctx.r3.u64 = 0;
  }
  using Clock = std::chrono::steady_clock;
  static const Clock::time_point t0 = Clock::now();
  const double secs =
      std::chrono::duration<double>(Clock::now() - t0).count();
#if defined(PGR4_NATIVE_GPU)
  const double frame = pgr4::gpu::PresentCount();
#else
  const double frame = g_presents.load(std::memory_order_relaxed);
#endif
  // A wait's segment starts at the first poll after its event, once the
  // segment before it has started.
  auto &segments = parsed.segments;
  for (size_t i = 0; i < segments.size(); ++i) {
    Segment &s = segments[i];
    if (s.started)
      continue;
    if (i > 0 && (!segments[i - 1].started ||
                  s.counter->load(std::memory_order_relaxed) < s.count))
      break;
    s.started = true;
    if (i == 0)
      continue; // frames count from boot, seconds from the first poll
    s.origin_seconds = secs;
    s.origin_frame = frame;
    PGR4_INFO("pgr4_input_script: wait {} over at present {} ({:.1f} s)", i,
                u32(frame), secs);
  }
  // Each press's first poll is logged with the present it landed on, so a run
  // whose menus went astray shows which press a page swallowed.
  static std::vector<bool> logged(script.size());
  u16 buttons = 0;
  u8 lt = 0, rt = 0;
  i16 lx = 0;
  for (size_t i = 0; i < script.size(); ++i) {
    const Press &p = script[i];
    const Segment &s = segments[p.segment];
    if (!s.started)
      continue;
    const double now = p.seconds ? secs : frame;
    const double start =
        p.start + (p.seconds ? s.origin_seconds : s.origin_frame);
    // A press the next wait overtook before it began is dropped.
    if (p.segment + 1 < segments.size() && segments[p.segment + 1].started) {
      const Segment &next = segments[p.segment + 1];
      if (start >= (p.seconds ? next.origin_seconds : next.origin_frame))
        continue;
    }
    if (now >= start && now < start + p.hold) {
      if (!logged[i]) {
        logged[i] = true;
        PGR4_INFO("pgr4_input_script: press {} at present {} ({:.1f} s)", i,
                  u32(frame), secs);
        if (p.capture == kRenderDocCapture) {
          TriggerRenderDocCapture();
        } else if (p.capture >= 0) {
          // The frame being built is under way; the one after is whole.
          std::lock_guard lock(g_capture_mutex);
          g_capture_frames[p.capture].push_back(u32(frame) + 2);
          g_any_capture.store(true, std::memory_order_release);
          PGR4_INFO("pgr4_input_script: {} present {}",
                    p.capture == int(pgr4::script::Capture::Dump) ? "dump"
                                                                  : "trace",
                    u32(frame) + 2);
        }
      }
      if (p.capture >= 0)
        continue;
      buttons |= p.buttons;
      lt = std::max(lt, p.left_trigger);
      rt = std::max(rt, p.right_trigger);
      if (p.thumb_lx)
        lx = p.thumb_lx;
    }
  }
  if (!buttons && !lt && !rt && !lx)
    return;
  if (lx)
    state->sThumbLX = lx;
  state->wButtons = u16(state->wButtons) | buttons;
  state->bLeftTrigger = std::max<u8>(state->bLeftTrigger, lt);
  state->bRightTrigger = std::max<u8>(state->bRightTrigger, rt);
  state->dwPacketNumber =
      u32(state->dwPacketNumber) + 1 + static_cast<u32>(secs * 60.0);
}
