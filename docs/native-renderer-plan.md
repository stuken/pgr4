# Native renderer plan for pgr4

Template: `D:\rexglue_projects\pgr3` (Project Gotham Racing 3). pgr3's native
renderer is a port of rr6's, which in turn is a port of re:Blue's. pgr3's
`docs/native-renderer-plan.md` records how each piece was found and fixed, and
`docs/engine-quirks.md` lists what Bizarre's engine needs from a renderer,
written for exactly this port. This plan follows the same phases and points at
those documents rather than repeating them. SDK: rexglue 0.10.0.

Addresses written `pgr3:0x...` are PGR3's, in its patched image. They are
there for finding the PGR4 equivalent by behaviour, not to reuse.

---

## Status (2026-10-20)

| Phase | State |
|---|---|
| 0. Baseline and tooling | **Exit met; references still to take.** Codegen and the xenos build work from a fresh tree. Ported from pgr3: `pgr4_input_script`, `pgr4_null_audio`, `pgr4_dump_frames`, `pgr4_dump_raw`, the abort logger, `scripts/re/funcdb.py` and `match_names.py`. A scripted run reaches a St Petersburg street race, and its dumps decode all 42 resolves of a race frame. Still open: `pgr4_trace_frame` (needs Phase 4 names), and reference dumps of the garage, a night race and a rain race. See Phase 0 findings. |
| 1. Shader extraction | **Exit met; coverage runs still to do.** Every build dumps the patched image (`pgr4_dump_image`) and extracts **990** shaders (`pgr4_shaders`): 980 from the byte scan and 10 built on the stack, 888 of them named. The runtime census (`pgr4_shader_census`, hooking `XGGetMicrocodeShaderParts`) matched all 970 containers a front-end-and-race run created, byte for byte, with none missing. The 23 not yet created are identified. See Phase 1 findings. |
| 2. XenosRecomp | **Not started.** |
| 3. Build integration | **Not started.** |
| 4. D3D identification (XDK 6132) | **Not started.** This is the largest unknown. |
| 5. Native renderer bring-up | **Not started.** |
| 6. Hardening and performance | **Not started.** |

## What we know about PGR4

Everything before "Phase 0 findings" comes from reading `assets/` (no build,
no codegen). The scripts are in `scratch/` (gitignored), and each has a usage
docstring.
`assets/default_patched.xex` is a hand-made copy of `default.xex` with
`default.xexp` applied. As in pgr3, it serves only for these early scans: the
build will take the patched image from `pgr4_dump_image` (Phase 1).

### The executable

| | PGR4 | PGR3 |
|---|---|---|
| Title ID | `4D5307F9` | `4D5307D1` |
| `default.xex` MD5 | `c29caed4497c61b47e2f1007f025d20d` (version `0x1`) | `00142082...` |
| `default.xexp` MD5 | `e97b06129bd1e465eab5ea0f51b74e2d` (delta-compressed, type 3) | `1b405bbc...` |
| `default_patched.xex` MD5 | `bcfa9fea1549449cbde3bb7123b5ae58` (version `0x201`; hand-made, scans only) | |
| Encryption / compression | retail AES, basic | the same |
| Image | `0x82000000`, `0xD70000` bytes | `0x82000000` |
| Entry point | `0x8267CA00` unpatched, `0x826B1E60` patched | |
| `setjmp` / `longjmp` | `0x828DE260` / `0x828DE7E0` (`pgr4_manifest.toml`) | `0x8247F3D0` / `0x8247F2B0` |

The `setjmp`/`longjmp` addresses in `pgr4_manifest.toml` are the patched
image's (checked in Phase 0): `0x828DE260` is the XDK `setjmp` (hook call,
then `mflr`/`mfcr` and `stfd f14,0(r3)` onwards) and `0x828DE7E0` is
`longjmp` (reads `jmpbuf+0x138`, tests `val`). The unpatched image has
unrelated code at both. The patch moves code (the entry point shifts by
`0x35460`), and codegen works on the patched image.

`assets/Pgr4TitleUpdate.bin` (a copy is also in `update/`, which
`pgr4_app.h` sets as `update_data_root`) is a 38 KB data file with an
`FLSK` header. It is title-update data that the game reads, not code. The
game opens it as `UPDATE:\Pgr4TitleUpdate.bin` (string at `0x821B5D58`), so
the `update/` copy is the one that counts.

### XDK libraries (`scratch/xex_info.py`)

| Library | PGR4 (patched) | PGR4 (unpatched) | PGR3 (patched) | RR6 |
|---|---|---|---|---|
| D3D9 | **2.0.6132.4** | 2.0.6132.0 | 2.0.2099.6 | 2.0.2135.0 |
| D3DX9, XGRAPHC | 2.0.6132.0 | 2.0.6132.0 | 2.0.2099.3 | 2.0.2135.0 |
| XAPILIB | 2.0.6132.6 | 2.0.6132.2 | 2.0.2099.11 | 2.0.2135.1 |
| Other | XFFB, XONLINE, XMP, XAUD, XHV, LIBCPMT | the same | XMP, XHV, XONLINE, XAUD, LIBCMT | XNET |

The title update relinks with a newer QFE of D3D9 (`.4`) and XAPILIB (`.6`).
The D3D functions the renderer hooks may therefore differ between the two
images, which is another reason for doing all of Phase 4 against the patched
image only.

PGR3 and RR6 sat one XDK revision apart, so most of rr6's D3D names carried
over to pgr3 at shape ratio 1.00. **6132 comes about four thousand builds after
2099.** Expect the same API names, a reorganised `D3DDevice`, and API that
2099 lacked. Every address, offset and register quirk in pgr3's `src/gpu/d3d.h`
and `config/d3d.toml` has to be re-derived, and shape matching against pgr3's
names will find fewer exact matches (Phase 4).

### Shaders (`scratch/scan_shaders.py`, `scratch/container_paths.py`, `scratch/compare_shaders.py`)

- **980 unique containers (359 VS, 621 PS) from 1,000 references** in the
  **patched** image, VA `0x820072F8`..`0x8223C9D8`, 160 to 4,368 bytes each
  (median 1.7 KB, 1.7 MB in all). The unpatched image has the same counts at
  `0x820070D8`..`0x82224808`. PGR3 has 305.
- All are **container revision 0x11** (`0x102A1100`/`0x102A1101`, with
  `field1C`/`field20` zero). This is the revision **upstream XenosRecomp**
  (Unleashed Recompiled) parses natively. rr6 and pgr3's 0x0E work does not
  apply, but their fork keeps the 0x11 path (`main.cpp`, `shader.h`).
- The CTAB creator is `2.0.6132.0`, and the containers are `ps_3_0`/`vs_3_0`.
- **878 of the 1,000 references carry their source path**
  (`c:\PGR4\01.28.b\Symbols\Src\code\gotham4\game\src\Graphics\shaders\h_Xbox 360\psBlur.updb`
  in the patched image; `08.31.a` in the unpatched one). That names 878
  of the unique shaders, each name used once (`psBlur`, `psToneMapSimple`,
  `psCarAlphaMask0`, `psBikeChain0`-`7`, ...), which pgr3 never had. The
  other 102 have no path, and two of those share microcode. The engine's own
  name for itself is **gotham4**.
- **The title update rebuilt the shaders but changed none of them.** Matched
  by name, or by microcode where there is no name, all 979 pairs have
  identical microcode and an identical constant table. In 878 (the ones with
  a path) the header and the path differ, because the build directory moved
  from `08.31.a` to `01.28.b`. The other 101 are byte for byte the same. This
  differs from PGR3, whose title update changed the microcode of 50 shaders.
  The shader cache is still keyed by XXH3 over the whole container, so a
  cache built from the unpatched image would miss on 878 of 980. The build
  must read the patched image, as in pgr3.
- **10 `lis rX,0x102A` instructions** in the patched image, in four clusters:
  `0x822BDBDC`/`0x822BE1EC`, `0x824F3EF8`..`0x824F52C4` (four),
  `0x825FEB14`/`0x825FEFD4` and `0x82627BA4`/`0x82627F90`. The unpatched
  image has the same pattern at other addresses. In PGR3 this pattern found
  the 4 stack-built shaders (two functions, a VS and PS each), so these are
  probably four or five functions building about ten shaders. A byte scan
  cannot find a stack-built container, so none of them is in the 980.

### Content

- **Cities:** London, New York, Las Vegas (night), Tokyo (night), Shanghai
  (night), Macau, Quebec, St Petersburg, the Nürburgring (with a snow
  variant), and the test track. There are also five garages and the front
  end.
- **Weather is per area, not just day and night.** Each area has
  `_sunny`/`_night` streams and lighting sets (`*.bpr`) named `_light`,
  `_heavy`, `_wet`, `_clear`, `_sunset`, `_smoggy`, `_apocalypse` and so on.
  File names include `_rain` (16), `_snow` (10), `_overcast` (8),
  `_storm` (7) and `_fog` (7). Rain, snow, wet roads and fog are render paths
  PGR3 never had.
- **Bikes**, which PGR3 did not have (`psBikeChain*`).
- **13 Bink movies** (`Movies/*.bik`), including intro, transition and rank
  movies.
- Streamed course data again (`.stream2`, `.pak`, `.pak_hrd`), so the
  streaming fixes in pgr3's quirks §8 should be expected to apply.
- `gw.xex` (Geometry Wars) is again a separate executable. It is out of
  scope, as it was in pgr3.

### Phase 0 findings

**The xenos build runs from a fresh tree.** Against the SDK install at
`C:\Users\stuar\rexglue_projects\rexglue-sdk` (stuken/rexglue-sdk `905694f`,
the commit pgr3 pins; that checkout carries two local edits, FidelityFX on
and full debug info for generated code):

```sh
cmake --preset win-amd64-relwithdebinfo -DCMAKE_PREFIX_PATH=<sdk>/out/install/win-amd64
cmake --build out/build/win-amd64-relwithdebinfo --target pgr4_codegen
cmake --preset win-amd64-relwithdebinfo
cmake --build out/build/win-amd64-relwithdebinfo
```

- **Codegen** takes 24 s and writes 271 files with no errors. It loads
  `game:\default.xex` and `UserModule` applies the sibling `default.xexp`
  (`user_module.cpp:105`), as for PGR3; it reports version `0.0.2.1`. The
  runtime does the same ("XEX patch applied successfully: base version
  0.0.0.1, new version 0.0.2.1"), so codegen and the game both see the
  patched image. The 17 functions in `pgr4_config.toml` raise nothing.
- **The build** (142 steps) takes 51 s with no warnings.
- **Running** needs `assets\` and `update\` next to `pgr4.exe`
  (`OnConfigurePaths` takes both from the exe's folder); directory junctions
  into the build folder work.

**How far it gets, unattended** (window captures every 10-15 s,
`scratch/run1`, `scratch/run2`): the Dolby logo, then the full intro movie
(about three minutes, through every city), the PGR logo and the title screen
with the Enzo at about 200 s, then back into the attract loop. It ran five
minutes without a crash or a log error. The front end starts loading behind
the title screen (`frontend_mad.bpr`, `glass.dds`, the Enzo's
`VehiclePvsData`, all probed and absent, as are the `lang.ini` files,
`debug_frontend.xml`, `d:\DebugOptions.ini` and `Game:\Vince\`).

(`--mnk_mode` keys only reach the pad while the window has focus,
`mnk_input_driver.cpp:484`, so keys posted to a background window do
nothing. Scripted runs use `pgr4_input_script` instead.)

**The frame end and the tooling hooks.** As in PGR3, the game's frame end,
`sub_822B3B08`, resolves the front buffer itself (`D3DDevice_Resolve`, flags
`0x300`) and then calls `D3DDevice_Swap`; there is no `Present` on its path.
It then waits on the previous frame's fence and inserts a new one. Found from
the kernel imports with `funcdb.py` and named in `config/d3d.toml` and
`config/xapi.toml`:

| Function | PGR4 | PGR3 | Found by |
|---|---|---|---|
| `D3DDevice_Swap` | `0x826CB148` | `0x823CADB8` | `VdSwap`'s only caller; (dev, front buffer, params) |
| `D3DDevice_Resolve` | `0x826CBEE0` | `0x823CEF68` | called by the frame end with `0x300`; 913 instructions, 39 callers, 2099's arguments |
| `D3DDevice_InsertFence` | `0x826CE9C0` | `0x823D1280` | the frame end; returns the counter at `+0x2A9C` |
| `D3DDevice_IsFencePending` | `0x826CE9F8` | `0x823D12B8` | the frame end |
| `D3DDevice_BlockOnFence` | `0x826CE320` | `0x823D0D50` | the frame end; waits with reason 6 (2099: 13) |
| `XInputGetState` | `0x826A8F00` | `0x823C5600` | `XamInputGetState`'s only caller; passes flags 1 (2099: 0) |

First 6132 layout facts, from `Resolve` (more in Phase 4):

- **Render targets at device `+0x3090`**, depth at `+0x30A0` (2099:
  `+0x30F8`). So pgr3's device offsets do not carry over.
- **The texture fetch constant is at `+0x1C`** in `D3DBaseTexture` (2099:
  `+0x10`); `Resolve` reads the destination's pitch, base and format, size
  and dimension at `+0x1C`, `+0x20`, `+0x24` and `+0x30`. The dumps decode
  with it.

**A scripted run reaches a race** (`scratch/run3`..`run6`). With the profile
saved by an earlier run:

```
--pgr4_input_script=start@10s,start@15s,...,start@60s,a@65s,...,a@100s,rt@115s+400
```

(Start every 5 s to 60 s, A every 5 s to 100 s, then throttle.) It goes
through the warning, Gamer Profiles ("Welcome to PGR4!"; the first run creates
the profile and takes about 40 s longer), Select Driver, Arcade Mode, "Catch
Me If You Can", into the St Petersburg street race (day, overcast). The car
drives straight into the wall at the first bend, which is enough for
reference frames. With `--readback_resolve=full` the game runs at about 10
presents a second, so present 2100 falls in the race; the script's seconds
keep the menus on time regardless.

**A race frame** (present 2100, `scratch/run6`): 42 resolves, and every one
decodes.

| Resolves | Target | Format | What |
|---|---|---|---|
| 1 | 200x200 | `k_8_8_8_8` | minimap |
| 8 | 128x128, one base | `k_8_8_8_8` | a grey noise pattern built in eight passes; purpose unknown (not rain: this race is dry) |
| 6 | 256x256 cube, faces 0-5 (`r9`) | `k_16_16_16_16_EXPAND` | the environment cube, rendered every frame from the car |
| 1 | 512x512 | `k_24_8_FLOAT` | car shadow map (depth, 0.33-1) |
| 3 | 320x180, one base | `k_2_10_10_10_AS_16_16_16_16` | the scene downsampled and blurred twice (depth of field or motion blur?) |
| 1 | 1280x720 | `k_24_8_FLOAT` | scene depth, **0 far to 0.048 near: reversed Z** |
| 3 | 1280x720 | `k_16_16_16_16_FLOAT` | the HDR scene |
| 3 | 64x64, 16x16, 4x4 | `k_16_16_16_16_FLOAT` | luminance chain |
| 2 | 1x1, two bases | `k_32_FLOAT` | adaptation (PGR3: `k_16_16_FLOAT`) |
| 13 | 320x180, then 160x90, 80x45, 40x22 three times each, then 320x180 | `k_8_8_8_8` | bloom |
| 1 | 1280x720 | `k_2_10_10_10_AS_16_16_16_16` | the front buffer, tone-mapped with the HUD |

So against PGR3's race frame: **1280x720 instead of 1024x600**, a **10-bit
front buffer**, an FP32 adaptation value, a per-frame environment **cube**,
reversed-Z depth, and a downsample-and-blur pass PGR3 did not have. MSAA is
not visible from resolves; Phase 4's trace will show it.

Things to watch later:

- `NtCreateFile('GAME:\Game\Replay')` fails with access denied
  (`0xC0000022`) at boot, so saving replays may need the directory to be
  redirected to a writable device.
- At the title the game probes `\Device\Image` and calls
  `IoDismountVolumeByFileHandle` (stubbed) twice.
- The image names some shaders outside the 878 `.updb` paths:
  `DontShip/Shaders/{psCloth,psClothNight,vsCloth,psParticle,vsBink,psBink,psBinkNoAlpha}`
  (`0x8215FA38`, `0x82236250`, `0x8223B08C`). Phase 1 found they are labels
  for resource accounting, not files.

### Phase 1 findings

**Shader creation is XG-style in 6132.** `D3DDevice_Create{Vertex,Pixel}Shader`
is not linked. The game's `sub_822B34A0` (VS) and `sub_822B35F0` (PS) call
`XGGetMicrocodeShaderParts(container, &parts)`, allocate the header and the
physical microcode themselves (from a pool, or through `sub_8230BEB0`), fill
the header with `XGSet{Vertex,Pixel}ShaderHeader`, copy the microcode and call
`XGRegister{Vertex,Pixel}Shader(header, physical)`. The engine core's loaders
(`sub_8286F7B0` VS, `sub_8286F9F0` PS) do the same with their own allocator.
All five XG functions are named in `config/d3d.toml`. For Phase 4 and 5:
the native renderer has to hook `XGRegister*Shader` (or the two creator
pairs), not `Create*Shader` as in pgr3.

**The extracted set** (`pgr4_shaders`, from `pgr4_dump_image`'s image,
identical to a scan of the hand-patched XEX): 990 unique shaders, 364 VS and
626 PS, from 1,010 container references. **All ten `lis rX,0x102A` are
stack-built shaders**, and the replay takes all ten (it now looks at r3, r4
and r5): `vs/psDefaultEffect` (`sub_822BDBD0`, the game's fallback effect),
`vs/psTelemetryStrip` and `vs/psTelemetryData` (`sub_824F3EE8`),
`vs/psCircuitMap` (`sub_825FEA68`) and `vs/psRouteMap` (`sub_82627B00`). They
carry `.updb` paths too, so 888 of the 990 have a source name in
`shaders.csv`.

**The census** (`scratch/census1`: boot, front end, the St Petersburg race):
970 distinct containers created. Every one matches its extracted file byte for
byte, and none is missing from the set. The ten stack-built ones are created
from stack addresses and match by content. Nothing came from game data
(`.pak`, `.stream2`), and almost everything was created during the front end,
not as the course loaded.

**The 23 not yet created** all belong to the engine core, which the source
calls **mercury** (`code\mercury\library\core`). Its shaders have no
`.updb` path, but each loader call passes a name and the embedded container
(`scratch/loader_names.py` pairs them):

- the 11 particle pixel shaders (`psParticle`, `psParticleA8`, `...Soft`,
  `...Decal`, `...Refract`, `...PointSprite`, ...), whose vertex shaders were
  created;
- `LuminanceInitialHDR`, `LuminanceInitialHDRU32`, `TonemapHDR`,
  `BrightPassHDR` and mercury's own `Tonemap`. The game uses `BrightPass`,
  `LuminanceInitial` and its own `Tonemap` (`sub_824BFBD0`);
- mercury's `MotionBlur` (the game has its own `MotionBlur`,
  `MotionBlurSplitScreen` and `MotionBlurDisabled`);
- the debug and basic-render shaders (`vsDebug2D`, `psDebug`,
  `vsBasicRender{Coloured3D,Textured2D,Textured3D}`, `psBasicRenderTextured`).

The particle shaders are the ones to watch: a rain race (`vsDrop`,
`psRainDrop`, `vsSplash`) or tyre smoke may create them.

**No hot-reload path.** mercury's loaders take a name such as
`DontShip/Shaders/psParticle` or `Shaders/psGaussBlur` alongside the embedded
container, but use it only to label the resource in its memory report
(`"%s PixelShader '%.1fmb'"`); nothing opens a file. Several mercury shaders
are byte-identical copies of others (the particle loader's second table
repeats its first; `psRainDrop` is `psDrop`), which the extractor's aliases
already cover.

The Perlin noise (`vs/psPerlin`, `psPerlin2`, `psPerlin4`) is the
eight-pass 128x128 target in the race frame (Phase 0).

---

## Approach

The same as pgr3's, for the same reasons (pgr3 plan, "Approach"):

- **Wrap, don't replace.** The game's own D3D still creates and owns every
  guest object. The host mirrors them by guest address and reads draw state
  from the device's register shadows.
- **Port from pgr3, not rr6.** pgr3 is the latest renderer in the chain and
  the same engine (gotham3 to gotham4). It already carries the Bizarre
  quirks: ring-written constants, `ClearF`, EDRAM aliases, vertex pulling,
  MSAA, 7e3, MRT, depth resolves, rect lists and streaming hashes.
- **Keep the xenos build working** behind `PGR4_NATIVE_GPU=OFF`. It is the
  reference every native step is compared against.
- **Use `docs/engine-quirks.md` from pgr3 as the bring-up checklist.** Copy
  it into this repo as a starting point, and mark each item confirmed,
  changed or absent for PGR4.

### What ports from pgr3

| | Files (pgr3 `src/` unless noted) |
|---|---|
| **As is** (rename `pgr3`→`pgr4`, `PGR3_`→`PGR4_`) | `gpu/`: `device*`, `frame_ring`, `host_heap`, `host_resource_heap`, `bindless*`, `deferred_destroy`, `graveyard`, `device_lost`, `dred`, `format`, `byte_swap`, `sampler_cache`, `texture_upload`, `texture_replace`, `native_texture_mirror`, `surface_registry`, `guest_buffers`, `vertex_declaration`, `present`, `perf_stats`, `gpu_profiling`, `frame_dump`, `imgui_overlay_drawer`, `pipeline/{pipeline_cache,pipeline_state,pso_recorder}`, `shaders/{guest_shaders,shader_linker,dxc_link}`, `shaders/hlsl/*`; `core/*`; `platform/{input_script,silent_audio,guest_abort_log,fatal_dialog,native_window,xenos_resolve_dump,shader_census}` |
| **Re-derive for XDK 6132** | `gpu/d3d.h` (device and resource layouts), `config/d3d.toml`, `config/xapi.toml`, every address and register quirk in `gpu/hooks/*.cpp` (frame end, `Clear`/`ClearF` argument registers, `Resolve` stack arguments, the `Destroy` type switch, `BeginShaderConstantF4`, the query-slot pool, the tiling state) |
| **Revisit for PGR4's passes** | `gpu/output.*` and `resolve.cpp` (design size, the FXAA point on the final composite, `pgr3:ps_8200E300`), `constant_buffers.cpp`, `hooks/state.cpp` (depth growth), the shadow-scale rule (square 256/512 depth surfaces), `ClearEdramAliases`, the 7e3 alpha-on-resolve-clear workaround, `device.h` defaults |
| **Drop** | `platform/scenery_probe.cpp` (hooks PGR3's draw loop), `pipeline/cache/pipeline_state_cache.h` (PGR3 data; start empty), any hook keyed to a PGR3 shader hash |
| **Submodules** | `thirdparty/rexglue-sdk` (stuken `main`), `thirdparty/plume` (stuken `rr6`), `thirdparty/XenosRecomp` (stuken fork: a `pgr4` branch off `pgr3`, see Phase 2), plus vendored miniz and zstd |
| **Build** | `cmake/{native_gpu,shader_cache,shaders}.cmake`, `thirdparty/CMakeLists.txt`, `tools/{dump_image,prelink_shader_cache}.cpp`, the top-level `CMakeLists.txt` layout (`CMakePresets.json` is already identical) |
| **Scripts** | `scripts/extract_shaders.py` (rev 0x11, Phase 1), `scripts/check_shader_census.py`, `scripts/re/{funcdb,match_names,state_setters,offset_map}.py` |
| **Licences** | `LICENSE` (0BSD) and `licenses/reblue-BSD-3-Clause.txt` for the files ported from re:Blue, keeping their headers |

---

## Plan

### Phase 0: Baseline and comparison tooling

Every fix in pgr3 was found by diffing against the xenos build, so the
reference comes first.

1. **Done:** codegen and the xenos build from a fresh tree, against the SDK
   at the commit pgr3 pins (`905694f`). The SDK applies `default.xexp` in
   both codegen and the runtime. Unattended it reaches the title screen (see
   Phase 0 findings).
2. **Done:** the asset checksums are in the README table, with
   `Pgr4TitleUpdate.bin` (`2cb08fdc...`, read from `update:`). The title
   update's version is `0.0.2.1` (codegen and runtime agree).
3. **Done, except `pgr4_trace_frame`:** `pgr4_input_script`,
   `pgr4_null_audio`, `pgr4_dump_frames` and `pgr4_dump_raw` (pgr3's endian,
   4 KiB tiled-offset and FP16 fixes kept), the abort logger,
   `core/{logging,settings,memory_helpers}` and `scripts/re/funcdb.py` and
   `match_names.py`, with the licence files. The dumper gained what PGR4's
   race needs: `k_16_16_16_16_EXPAND` (as FP16), `k_2_10_10_10_AS_16_16_16_16`,
   `k_32_FLOAT` and `k_24_8_FLOAT` depth (stretched grey, range logged), and
   the destination slice (`r9`), so each cube face lands in its own file.
   **Left:** `pgr4_trace_frame` hooks `SetRenderTarget`,
   `SetDepthStencilSurface`, the draw calls, `Clear` and the tiling calls,
   and reads the device's render-target, texture and stream shadows. All of
   these are Phase 4 identifications in 6132. `Settings::Init` belongs to
   the native renderer (Phase 3).
4. **Scripted runs to reference scenes**, in seconds from boot (pgr3 quirks
   §1). **Done:** the boot movies, title, front end and a day race (St
   Petersburg, overcast). **Left:** a garage, a night race and a **rain
   race**. Dump them with `--readback_resolve=full` into `scratch/`.
5. **Done:** the icon `.rc` was already `src/pgr4.rc.in`; `pgr4_app.h` now
   follows pgr3's xenos-build layout (`OnPreSetup` with `pgr4_null_audio`,
   the abort logger in `OnPreLaunchModule`). `OnPostInitLogging` comes with
   the native renderer.

**Exit:** a scripted run reaches a race in the xenos build, and its dumps
decode every resolve format the race uses. If the xenos build cannot reach a
race, write down where it stops: the native renderer may still get further,
but the comparisons will have to use other references (RenderDoc captures of
the native build, or video of the original).

### Phase 1: Shader extraction from the patched image

1. **Done:** `pgr4_dump_image` (`tools/dump_image.cpp`) and the
   `pgr4_shaders` target (`cmake/shader_cache.cmake`, in ALL until the
   recompiled cache depends on it). The XenosRecomp half of pgr3's
   `shader_cache.cmake` comes with Phase 2.
2. **Done:** `extract_shaders.py` reads revision 0x11 (nine-dword header,
   `field1C`/`field20` zero, a well-formed CTAB at `constantTableOffset`), and
   `shaders.csv` has a `name` column from the `.updb` path.
3. **Done:** the replay takes all ten stack-built shaders.
4. **Census, started:** `shader_census.cpp` (hooking
   `XGGetMicrocodeShaderParts`) and `check_shader_census.py` are ported. The
   front end and a day race are covered (967 of 990). **Left:** a race in
   each weather type, a bike race, photo mode, replays and split screen,
   watching the 23 listed in Phase 1 findings. None has come from
   `.pak`/`.pak_hrd`/`.stream2` so far.
5. **Done:** there is no hot-reload path; the `DontShip/Shaders/...` names are
   labels (Phase 1 findings).

**Exit:** the build extracts every shader from the patched image, and the
runtime census shows 100% hits.

### Phase 2: XenosRecomp for PGR4

1. Branch `pgr4` off the fork's `pgr3` branch. First run it over the patched
   set with only `REBLUE_RECOMP` (no game define) to see which of rr6's and
   pgr3's changes assume a 0x0E container or their ABI. Then run upstream
   XenosRecomp over the same set for comparison: 0x11 is its home format, so
   its failures are the genuinely new ones.
2. Add `PGR4_RECOMP` for what is per game:
   - the vertex input location table, built from the semantics PGR4's
     declarations use (pgr3's has 21; a `#error` names any that are missing);
   - the interpolator list (pgr3 had to append `NORMAL0`);
   - spec constants (`ALPHA_TEST`, the normal-format variants, and whatever
     new ones the census shows);
   - pgr3's packed vertex formats (DEC3N/DEC4N, UHEND3N family, integer
     `SHORT4` positions, SHORT2 `BLENDINDICES` swap), kept on until shown
     unused;
   - vertex pulling for shaders that compute their fetch index.
3. Review the HLSL dump for memexport (gotham4's particles or rain may use
   it), `cond_call`/`return`, int constants, dynamic register indexing and
   point sprites. Each is an `#error` or a host task.
4. Budget for scale. pgr3 prelinked 688 variants from 309 shaders. PGR4 could
   reach 2,000-plus, so measure the build time and the size of the embedded
   cache, and decide whether prelinking stays complete or becomes lazy for
   rare variants.
5. Check that rr6's and pgr3's shaders still compile with their own defines
   (pgr3's regression rule).

**Exit:** every shader compiles for DXIL and SPIR-V with 0 failures.

### Phase 3: Build integration

1. Copy pgr3's `cmake/`, `thirdparty/CMakeLists.txt` and `CMakeLists.txt`
   layout: `PGR4_NATIVE_GPU` (OFF = xenos), `PGR4_D3D12`,
   `PGR4_VULKAN_EXE`, the shader-cache OBJECT library, and `pgr4_prelink`.
2. Add the submodules (`.gitmodules`) and vendored miniz and zstd.
3. App: under `PGR4_NATIVE_GPU`, `config.graphics = nullptr`, and exit
   cleanly from `OnPreLaunchModule` until Phase 5 has a renderer (pgr3 Phase
   3 notes why a queued quit is not enough).
4. Check regeneration: touching `shader_common.h` reruns the recompile, and
   touching `default.xexp` reruns the dump, extraction and recompile.

**Exit:** both configurations build. The native exes link the cache and exit
with a "renderer missing" log.

### Phase 4: Identify XDK 6132 D3D

pgr3's method still applies: find functions by behaviour in the codegen
output, not by signature. Because 2099 and 6132 are far apart, the anchors
matter more than the shape matching here.

1. **Anchors:**
   - kernel imports: `VdSwap`, `VdInitializeEngines`/`VdInitializeRingBuffer`,
     `VdGlobalDevice`, `VdGetCurrentDisplayGamma`,
     `MmAllocatePhysicalMemoryEx`/`MmFreePhysicalMemory`;
   - the shader container VAs and their references, which lead to
     `Create{Vertex,Pixel}Shader` or whatever replaces them;
   - the `{Get, Set, Default}` render- and sampler-state tables
     (`scripts/re/state_setters.py`). The state enums are the API's, so
     index *k* is still the same state, and the device fields each setter
     writes map out the register shadows;
   - shape matching against pgr3's 74 names (`match_names.py`). Record the
     ratio for each match, and confirm anything under 1.00 by behaviour.
2. **Find out what 6132 adds over 2099.** Each item becomes a hook, a
   documented gap, or a reason the wrap-don't-replace model needs more:
   - **The frame end.** Does the game call `D3DDevice_Present`, or resolve
     and `Swap` itself as PGR3 does?
   - **Constant paths that bypass the device shadow:** `BeginShaderConstantF4`
     (or its successor), GPU-owned constants, and constant buffers set as a
     block. pgr3 lost every world matrix until its ring writes were modelled.
   - **Command buffers.** If 6132 offers precompiled or recorded command
     buffers and gotham4 uses them, state reaches the GPU without passing the
     register shadows the renderer reads. This would be the largest change to
     the approach, so check early.
   - Surface creation parameters (EDRAM base, `HiZ`), predicated tiling,
     queries, and `DrawIndexedVerticesUP`.
3. Write `config/d3d.toml` and `config/xapi.toml`, include them from
   `pgr4_manifest.toml`, and re-run codegen with no naming warnings.
4. Write `src/gpu/d3d.h` for 6132: allocation size, dirty masks, dispatch
   tables, fetch, float and bool constants, every register shadow pgr3 reads,
   viewport, scissor, clip planes, streams, textures, render targets, depth
   and shaders. Check the resource headers (`D3DResource`, VB, IB, texture,
   surface) against live data with a destroy log and a frame trace, as pgr3
   did. Note whether gotham4 still builds its own VB/IB headers.
5. **Trace a race frame of each kind** (day, night, rain, snow; car and bike;
   a cockpit view) with `pgr4_trace_frame`, and fill in the pass table:
   targets, sizes, MSAA counts, formats, EDRAM bases, MRT, depth resolves and
   rect lists.

**Exit:** a PGR4 equivalent of pgr3's hook table (pgr3 plan, Phase 4
findings), every address named, `gpu/d3d.h` rewritten for 6132, and the pass
table filled in.

### Phase 5: Native renderer bring-up

1. Port the files listed under "What ports from pgr3", with the PGR3-specific
   passes stubbed out until PGR4's equivalents are known.
2. Bring it up in pgr3's order, checking each step against the Phase 0
   references:
   1. `CreateDevice`, `Clear`/`ClearF`, the frame end, and the ring and
      command memory turned into discard buffers;
   2. shader creation, with the hit rate logged (target: every shader);
   3. the first 2D draws (front end, Bink movies);
   4. textures, including packed, cube, volume and streamed ones;
   5. render targets and `Resolve`, including partial, depth, MRT and
      converting resolves;
   6. 3D course geometry, with ring-written constants and bool constants
      modelled **before** any scenery is judged (quirks checklist item 4);
   7. post effects, the HUD and the display curve.
3. **Work through pgr3's quirks checklist** (`engine-quirks.md`, "Bring-up
   checklist for a new title"), marking each item as it is confirmed, changed
   or absent.
4. **Model what is new in PGR4 when the trace shows it.** These are expected
   from the content but not yet seen:
   - **weather:** rain on the screen and windscreen, wet-road reflections,
     snow, fog and storm lighting. These may bring particles, memexport,
     additional render targets or refraction resolves;
   - **bikes and their riders** (new skinning or vertex formats);
   - **photo mode, replays, Gotham TV and split screen**, which are likely to
     use predicated tiling, larger targets or picture-in-picture corners
     (pgr3 quirks §3);
   - whatever render setup 6132 and gotham4 use: the resolution, MSAA count
     and HDR format (pgr3 used 1024x600, 2x, 7e3; do not assume it).
5. Keep pgr3's `WARN_FIRST` warnings for paths the renderer does not model,
   and model a path only when a warning shows PGR4 reaching it.
6. Lifetime: hook `D3DResource_Destroy` and `XPhysicalAlloc`/`XPhysicalFree`,
   and check course changes in both directions.

**Exit:** the front end, garages, a race in every city by day and night, a
race in every weather type, a bike race, replays, photo mode and the movies
match the xenos references at 1x on D3D12 and Vulkan.

### Phase 6: Hardening and performance

Mostly pgr3's Phase 6, redone for PGR4's passes:

- resolution scale and supersampling at 2x and 3x, checked pass by pass;
- `pgr4_shadow_scale`, if PGR4's shadow maps are separate square depth
  surfaces as PGR3's are;
- the pipeline precache, captured with `pgr4_pso_capture` across every city,
  weather and vehicle type;
- the per-draw cost work (batched slot binding, per-thread lookup caches, cold
  buffers, sparse texel hashes), re-measured since PGR4 may draw more;
- frame pacing, the Vulkan validation-layer pass (still open in pgr3) and
  `pgr4_perf_stats`;
- optional extras: FXAA before the HUD, 16-bit output, anisotropy, texture
  dump and replace;
- real occlusion queries, which pgr3 left open (it answers every query
  visible);
- Linux, as rr6 and pgr3 have it (written, not yet built).

---

## Risks and open questions

1. **XDK 6132 is far from 2099.** In pgr3, about 60 of rr6's 77 names matched
   at ratio 1.00. Expect far fewer here. Phase 4 depends on anchors and the
   state-setter tables more than on name matching, and it is the phase most
   likely to overrun.
2. **State that bypasses the register shadows.** Constant buffers, GPU-owned
   constants or command buffers would each need their own model, as
   `BeginShaderConstantF4` did in pgr3. Command buffers could force the
   renderer to parse PM4 packets for part of the frame. Find out in Phase 4
   step 2 before committing to Phase 5's order.
3. **Shader scale.** 980 shaders (plus about ten stack-built ones) against 309 in PGR3 means three
   times the recompile, prelink and cache work, and more chances of a new
   instruction pattern. On the other hand, rev 0x11 is upstream XenosRecomp's
   own format.
4. **The xenos baseline may not reach a race.** pgr3's whole method rests on
   comparing against xenos. If it fails early, fixing the SDK's xenos path
   for PGR4 may be cheaper than working without a reference.
5. **New render paths** (weather, bikes, photo mode, split screen) have no
   precedent in pgr3. The quirks document covers the engine as PGR3 used it,
   not everything gotham4 added.
6. **The patched image.** The title update leaves shader microcode alone,
   but it changes 878 container hashes, moves code and relinks D3D9 at a
   newer QFE. As in pgr3, codegen, the extractor and the runtime must all see
   the patched image, and a census check should fail the build if they
   drift.
7. **Scale of the port.** pgr3's `src`, `cmake`, `tools` and `scripts` come to about 27k lines,
   5k of them the precache list.
   Most of the cost was game-specific findings, not the copy. Phases 0-4 leave
   the xenos build untouched and should be finished before committing to
   Phase 5.
