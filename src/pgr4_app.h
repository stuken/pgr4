// pgr4 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>

#ifdef _WIN32
#include <timeapi.h>
#endif

#include "icon.generated.h"

#include "platform/guest_abort_log.h"
#include "platform/silent_audio.h"
#include <rex/cvar.h>

REXCVAR_DECLARE(bool, pgr4_null_audio);

#ifdef PGR4_NATIVE_GPU
#include <rex/logging.h>

#include "core/shutdown.h"
#include "gpu/device.h"
#include "gpu/imgui_overlay_drawer.h"
#include "gpu/settings.h"
#endif

class Pgr4App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Pgr4App>(new Pgr4App(ctx, "pgr4",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
#ifdef PGR4_NATIVE_GPU
    // Null graphics makes Runtime::Setup skip the SDK GPU. The plume
    // renderer comes up in OnPreLaunchModule instead.
    config.graphics = nullptr;
    config.gpu_plugin.clear();
#else
    config.gpu_plugin = "xenos";
#endif
    if (REXCVAR_GET(pgr4_null_audio))
      config.audio_factory = REX_AUDIO_BACKEND(pgr4::platform::SilentAudioSystem);
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
      const auto base_dir = paths.config_path.parent_path();
      if (paths.game_data_root.empty()) { // Use default assets directory path if one isn't provided!
          if (std::filesystem::is_regular_file(base_dir / "assets/default.xex")) {
              paths.game_data_root = base_dir / "assets";
          }
      }
      paths.user_data_root = base_dir / "user";
      paths.cache_root = base_dir / "cache";
      paths.update_data_root = base_dir / "update";
  }

  void OnPostSetup() override{
    window()->SetIcon(icon::kIconPNG, icon::kIconPNGSize);
    window()->SetTitle("Project Gotham Racing 4");
  }

#ifdef PGR4_NATIVE_GPU
  // Binds to the plume device lazily, once OnPreLaunchModule creates it. The
  // SDK owns and tears this down.
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    return std::make_unique<pgr4::gpu::ImGuiOverlayDrawer>();
  }

  // The first hook after rex::cvar::LoadConfig: the renderer's settings adopt
  // the config file and command line here and follow later cvar writes.
  void OnPostInitLogging() override { pgr4::gpu::Settings::Get().Init(); }

  void OnPreLaunchModule() override {
    pgr4::platform::InstallGuestAbortLogger();
    // Registered here, where the UI thread is pumping: the shutdown sequence
    // has to run on it.
    pgr4::SetShutdownDispatcher([this](std::function<void()> fn) {
      return app_context().CallInUIThread(std::move(fn));
    });
    pgr4::SetShutdownUIPump(
        [this] { app_context().ExecutePendingFunctionsFromUIThread(); });

    if (!pgr4::gpu::Video::CreateHostDevice(window())) {
      REXLOG_ERROR("pgr4: native renderer init failed");
      app_context().QuitFromUIThread();
      return;
    }
    pgr4::gpu::Video::SetOverlayDrawHook(
        [this](plume::RenderCommandList* cmd, plume::RenderFramebuffer* fb,
               u32 w, u32 h) {
          if (!imgui_drawer() || pgr4::IsShuttingDown() ||
              !imgui_drawer()->HasDialogs())
            return;
          app_context().CallInUIThreadSynchronous([this, cmd, fb, w, h] {
            pgr4::gpu::ReblueUIDrawContext ctx(w, h, cmd, fb);
            imgui_drawer()->Draw(ctx);
          });
        });
  }

  void OnWindowPixelSizeChanged(u32, u32) override {
    pgr4::gpu::Video::RequestResize();
  }

  bool OnWindowCloseRequested() override {
    pgr4::RequestShutdown(pgr4::ShutdownReason::WindowClose);
    return false;  // unreachable
  }
#else
  void OnPreLaunchModule() override {
    pgr4::platform::InstallGuestAbortLogger();
  }
#endif

  // Override virtual hooks for customization:
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
};
