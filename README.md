# pgr4

pgr4 rebuilds Project Gotham Racing 4 (Xbox 360) as a native PC program using static recompilation. The game's PowerPC code is translated to C++ with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and compiled for the host, so the game runs directly instead of inside an emulated console.

> [!IMPORTANT]
> This repository contains nothing from the game. You need the files from your own retail copy of Project Gotham Racing 4, together with its title update, and both building and running need them. Everything derived from the game is produced on your machine at build time and is never committed:
> - the recompiled C++ that codegen generates from `default.xex` (`generated/default/`, gitignored);
> - the 990 Xenos shaders taken from the patched game image, and the DXIL and SPIR-V that XenosRecomp makes from them (inside the build directory);
> - the window icon, also taken from `default.xex` (`src/icon.generated.*`, gitignored).

## Status

pgr4 is at the start of its native renderer port, following [pgr3](https://github.com/stuken/pgr3). Today it builds with the SDK's xenos plugin (Xenia-derived GPU emulation) only. With it, the game boots, goes through the front end and races (checked so far: a day race in St Petersburg). `docs/native-renderer-plan.md` tracks the plan, what has been found so far, and what is still open.

| Platform | State |
|---|---|
| Windows x64 | Builds and runs with the xenos plugin. |
| Linux x64, macOS | Not tried. |

Geometry Wars (`gw.xex`) is a separate executable, and the SDK cannot run a second XEX, so it is not supported.

## Building from scratch (Windows)

The build has three stages:

1. Add your game files.
2. Build and install the SDK.
3. Configure, run codegen, and build.

### 1. Prerequisites

- **Visual Studio 2022** (Community is fine) with these components:
  - Desktop development with C++.
  - C++ Clang Compiler for Windows. pgr4 and the SDK build with clang, not MSVC.
  - C++ CMake tools for Windows. This provides CMake and Ninja; standalone CMake 3.25+ and Ninja also work.
  - A Windows SDK.
- **Git**.
- **Python 3**. The build scripts use only the standard library.

Run every command below from an **x64 Native Tools Command Prompt for VS 2022**, with VS's clang on `PATH`:

```bat
set "PATH=%VCINSTALLDIR%Tools\Llvm\x64\bin;%PATH%"
clang --version
```

### 2. Fetch the submodules and add the game files

`thirdparty/XenosRecomp` is a submodule with submodules of its own:

```bat
git submodule update --init --recursive
```


Extract the contents of your Project Gotham Racing 4 disc into `assets/`, using a tool such as extract-xiso, so that you end up with `assets/default.xex` next to the `Game`, `Movies` and `UI` folders. From the title update, put `default.xexp` next to `default.xex`, and put `Pgr4TitleUpdate.bin` in `update/`. Both folders are gitignored.

The game always runs patched: the SDK applies `default.xexp` when it loads `default.xex`, both for codegen and at run time. The function addresses in `pgr4_config.toml` and `pgr4_manifest.toml` are specific to this pair of files:

| | |
|---|---|
| Title ID | `4D5307F9` |
| Title update | 0.0.2.1 |
| `default.xex` MD5 | `c29caed4497c61b47e2f1007f025d20d` |
| `default.xexp` MD5 | `e97b06129bd1e465eab5ea0f51b74e2d` |
| `Pgr4TitleUpdate.bin` MD5 | `2cb08fdc58f78f61fdf1c29757c351ae` |

Check your copy against these. A different `default.xex`, or a missing or different `default.xexp`, will not recompile correctly against these configs. The game reads `Pgr4TitleUpdate.bin` from `update:` at run time.

### 3. Build and install the ReXGlue SDK

pgr4 needs SDK version 0.10.0. It is developed against [stuken/rexglue-sdk](https://github.com/stuken/rexglue-sdk) at commit `905694f`, the one pgr3 pins, which adds GPU, kernel and XAM fixes on top of upstream. It is not yet a submodule here. Clone it next to pgr4 and install it:

```bat
git clone --recursive https://github.com/stuken/rexglue-sdk.git
cd rexglue-sdk
git checkout 905694f
git submodule update --init --recursive
cmake --preset win-amd64
cmake --build out/build/win-amd64 --config RelWithDebInfo --target install
cd ..
```

This installs the SDK to `rexglue-sdk\out\install\win-amd64`. The SDK uses a multi-config generator. Install each configuration you plan to build pgr4 in: run the build command again with `--config Release` or `--config Debug`.

### 4. Configure, run codegen, and build

Set `CMAKE_PREFIX_PATH` to the SDK install from step 3:

```bat
set REXSDK=C:\path\to\rexglue-sdk\out\install\win-amd64
set BUILD=out\build\win-amd64-relwithdebinfo

cmake --preset win-amd64-relwithdebinfo -B %BUILD% -DCMAKE_PREFIX_PATH=%REXSDK%
cmake --build %BUILD% --target pgr4_codegen
cmake --preset win-amd64-relwithdebinfo -B %BUILD%
cmake --build %BUILD%
```

The commands do the following:

1. **Configure.**
2. **Codegen.** The SDK's `rexglue codegen` recompiles the patched `default.xex` into `generated/default/`, following `pgr4_manifest.toml`.
3. **Reconfigure.** CMake only picks up the generated sources on a configure that runs after they exist. Without this step the first build fails to link. Later builds re-run codegen by themselves when its inputs change, so you need the reconfigure only once per fresh tree.
4. **Build.** This produces `pgr4.exe` in the build directory, along with the DLLs it needs from the SDK. It also extracts the game's shaders:
   - `pgr4_dump_image` loads the game through the SDK, title update applied, and writes the patched image.
   - `scripts/extract_shaders.py` pulls the 990 Xenos shaders out of it into `generated/shaders`, including ten the game builds on the stack.
   - XenosRecomp (`thirdparty/XenosRecomp`) turns them into DXIL and SPIR-V, compiled into `pgr4_shader_cache` for the native renderer to come.

Debug and Release presets work the same way (`win-amd64-debug`, `win-amd64-release`).

## Running

`pgr4.exe` looks for the game in `assets\default.xex` next to itself, and for the title update data in `update\`. Copy or link both folders into the build directory, for example with junctions:

```bat
mklink /J %BUILD%\assets C:\path\to\pgr4\assets
mklink /J %BUILD%\update C:\path\to\pgr4\update
```

Alternatively, point the game at `assets` directly with `--game_data_root="C:\path\to\pgr4\assets"`. `update\` must still be next to the executable.

Next to the executable, pgr4 uses:

- `pgr4.toml`: the config file. Any cvar can go there, and the same cvar can be given on the command line as `--name=value`.
- `user\`: saves and the gamer profile.
- `cache\`: the xenos plugin's shader storage.

To play with a keyboard, pass `--mnk_mode=true`. Return is Start and Space is A, and the SDK's `keybind_*` cvars list the rest.

### Bring-up and debugging tools

These are for working on the renderer rather than playing.

| Cvar | Effect |
|---|---|
| `pgr4_input_script` | Scripted pad input, `button@frame[+hold],...`; a trailing `s` counts seconds from the first pad poll. Buttons: `a b x y start back up down left right lb rb ls rs`, triggers `lt rt`. |
| `pgr4_null_audio` | Discard audio output, for machines without an audio device. |
| `pgr4_dump_frames` | Write every resolve target of the listed present frames as BMPs to `user\`. Add `--readback_resolve=full --d3d12_readback_resolve=true`. |
| `pgr4_dump_raw` | With `pgr4_dump_frames`, also write each resolve target's raw memory as a `.bin`. |
| `pgr4_shader_census` | Write every shader container the game creates to `user\shader_census`. `python scripts\check_shader_census.py <that folder> <build>\generated\shaders` then checks it against the extracted set. |

For example, this reaches a St Petersburg street race once a profile exists (the first run creates one and needs about 40 s more of A presses):

```bat
pgr4.exe --pgr4_input_script=start@10s,start@15s,start@20s,start@25s,start@30s,start@35s,start@40s,start@45s,start@50s,start@55s,start@60s,a@65s,a@70s,a@75s,a@80s,a@85s,a@90s,a@95s,a@100s,rt@115s+400
```

## Repository layout

| Path | Contents |
|---|---|
| `src/` | The app (`main.cpp`, `pgr4_app.h`), shared support (`core/`) and test tooling (`platform/`). |
| `config/` | Names for the game's statically linked XDK 6132 Direct3D and XAPI functions, used by codegen and the tooling's hooks. |
| `pgr4_config.toml`, `pgr4_manifest.toml` | Codegen configuration. |
| `cmake/` | The shader extraction and recompile build (`pgr4_shaders`, `pgr4_shader_cache_gen`). |
| `scripts/` | Shader extraction, the census check, the icon generator and reverse-engineering helpers (`scripts/re/`). |
| `tools/` | `pgr4_dump_image`. |
| `thirdparty/` | XenosRecomp. |
| `docs/` | The native renderer plan and findings. |

## License

pgr4's own code is under the [BSD Zero Clause License](LICENSE), as pgr3's and rr6's are: use it for anything, with no conditions. The code ported from re:Blue, by way of rr6 and pgr3, stays under re:Blue's BSD 3-Clause License ([licenses/reblue-BSD-3-Clause.txt](licenses/reblue-BSD-3-Clause.txt)), as its file headers say. The submodules keep their own licenses. No license here covers Project Gotham Racing 4 itself.

## Credits

- [pgr3](https://github.com/stuken/pgr3) and [rr6](https://github.com/stuken/rr6), the ports this one follows.
- [re:Blue](https://github.com/zolaware/reblue), whose native renderer those are ported from.
- The [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) team, for the recompilation toolchain.
- The [Xenia](https://github.com/xenia-project/xenia) project and the wider Xbox 360 emulation scene.
