// pgr4 - ReXGlue Recompiled Project
//
// pgr4_dump_image: writes the guest image exactly as the game runs it. The
// SDK applies the sibling default.xexp (title update) while loading
// default.xex, for codegen and for the runtime alike, so anything derived from
// the image at build time (the shader set) has to come from the loaded image
// rather than from the file on disk. This brings up a tool-mode Runtime the
// way rexglue codegen does, loads the XEX, and writes the module's image from
// base_address() for image_size() bytes. The image keeps its PE header, whose
// ImageBase tells readers where it sits.
//
// Usage: pgr4_dump_image <game_data_root> <xex name> <out file>
// e.g.   pgr4_dump_image assets default.xex out/build/.../default.image

#include <cstdio>
#include <filesystem>
#include <string>

#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/system/xmemory.h>

using rex::X_STATUS;

int main(int argc, char **argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <game_data_root> <xex name> <out file>\n", argv[0]);
    return 2;
  }
  const std::filesystem::path root = std::filesystem::absolute(argv[1]);
  const std::string xex = argv[2];
  const std::filesystem::path out = argv[3];

  rex::InitLoggingEarly();
  auto config = rex::BuildLogConfig(nullptr, "warn", {});
  config.log_to_console = true;
  rex::InitLogging(config);

  rex::Runtime runtime(root);
  if (runtime.Setup(rex::RuntimeConfig{
          .kernel_init = rex::kernel::InitializeKernel,
          .tool_mode = true,
      }) != X_STATUS_SUCCESS) {
    std::fprintf(stderr, "pgr4_dump_image: runtime setup failed\n");
    return 1;
  }
  // Fails, rather than loading the base image, when a present patch does not
  // apply (UserModule::LoadFromFile).
  const auto status = runtime.LoadXexImage("game:\\" + xex);
  if (status != X_STATUS_SUCCESS) {
    std::fprintf(stderr, "pgr4_dump_image: loading %s failed (0x%08X)\n", xex.c_str(),
                 unsigned(status));
    return 1;
  }
  auto module = runtime.kernel_state()->GetExecutableModule();
  auto *xex_module = module ? module->xex_module() : nullptr;
  if (!xex_module) {
    std::fprintf(stderr, "pgr4_dump_image: no executable module\n");
    return 1;
  }
  const uint32_t base = xex_module->base_address();
  const uint32_t size = xex_module->image_size();
  const uint8_t *image = runtime.memory()->TranslateVirtual<const uint8_t *>(base);

  std::filesystem::create_directories(std::filesystem::absolute(out).parent_path());
  FILE *f = std::fopen(out.string().c_str(), "wb");
  if (!f || std::fwrite(image, 1, size, f) != size) {
    std::fprintf(stderr, "pgr4_dump_image: writing %s failed\n", out.string().c_str());
    if (f)
      std::fclose(f);
    return 1;
  }
  std::fclose(f);
  const bool patched = std::filesystem::exists(root / (xex + "p"));
  std::printf("pgr4_dump_image: %s%s, 0x%08X..0x%08X -> %s\n", xex.c_str(),
              patched ? " + patch" : "", base, base + size, out.string().c_str());
  return 0;
}
