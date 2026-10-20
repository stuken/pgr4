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

#ifdef PGR4_NATIVE_GPU
#include <cstdlib>

#include <rex/logging.h>
#endif

REXCVAR_DECLARE(bool, pgr4_null_audio);

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
    // Null graphics makes Runtime::Setup skip the SDK GPU; the native
    // renderer is to come up in OnPreLaunchModule.
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

  void OnPreLaunchModule() override {
    pgr4::platform::InstallGuestAbortLogger();
#ifdef PGR4_NATIVE_GPU
    // The native build links the shader cache but has no renderer yet
    // (docs/native-renderer-plan.md, Phase 5), and the game cannot run
    // without a GPU. The module launches as soon as this returns, so a queued
    // quit would leave the guest running GPU-less; exit here instead.
    REXLOG_ERROR("pgr4: the native renderer is not implemented yet; "
                 "build with PGR4_NATIVE_GPU=OFF to play");
    rex::FlushLogging();
    std::_Exit(1);
#endif
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
};
