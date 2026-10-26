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

## Status (2026-10-23)

| Phase | State |
|---|---|
| 0. Baseline and tooling | **Exit met; references still to take.** Codegen and the xenos build work from a fresh tree. Ported from pgr3: `pgr4_input_script`, `pgr4_null_audio`, `pgr4_dump_frames`, `pgr4_dump_raw`, the abort logger, `scripts/re/funcdb.py` and `match_names.py`. A scripted run reaches a St Petersburg street race, and its dumps decode every resolve of a race frame. `pgr4_trace_frame` is ported (Phase 4), and Phase 4's Custom Match runs dumped night, rain, snow and bike race frames. Still open: reference dumps of a garage. See Phase 0 findings. |
| 1. Shader extraction | **Exit met; coverage runs still to do.** Every build dumps the patched image (`pgr4_dump_image`) and extracts **990** shaders (`pgr4_shaders`): 980 from the byte scan and 10 built on the stack, 888 of them named. The runtime census (`pgr4_shader_census`, hooking `XGGetMicrocodeShaderParts`) matched all 970 containers a front-end-and-race run created, byte for byte, with none missing. The 23 not yet created are identified. Phase 4 found a second creation path, `D3DDevice_Create*Shader`, which the census now hooks too; the front end and a race do not reach it. Night, rain, snow, bike and burnout runs still create 967 of 990, so the 23 are not weather or tyre-smoke shaders. See Phase 1 findings. |
| 2. XenosRecomp | **Exit met.** All 990 shaders compile to DXIL and SPIR-V with `REBLUE_RECOMP PGR4_RECOMP` on branch `pgr4` of the fork in `thirdparty/XenosRecomp` (`5f68d6f`, pushed). The build runs it (`pgr4_shader_cache_gen`) and compiles the cache into `pgr4_shader_cache`. pgr3's and rr6's shaders still compile; rr6's HLSL is unchanged, and 8 of pgr3's change as intended. Prelinking is Phase 3's. See Phase 2 findings. |
| 3. Build integration | **Done.** Both configurations build. `PGR4_NATIVE_GPU=ON` builds `pgr4.exe` (D3D12) and `pgr4_vk.exe` (Vulkan) on plume with the shader cache and 2,420 prelinked variants; until Phase 5 they log that the renderer is missing and exit. Touching `shader_common.h` or `default.xexp` regenerates what depends on it. See Phase 3 findings. |
| 4. D3D identification (XDK 6132) | **Exit met; a day race with weather still to trace.** Every function pgr3's renderer hooks has a 6132 address, and `config/d3d.toml` and `xapi.toml` name 86 functions with no codegen warnings. `src/gpu/d3d.h` is rewritten for 6132 (a 0x5780-byte device, 24-byte resources, the register shadows as blocks) and checked against a live race through the ported `pgr4_trace_frame`, which also filled in the race pass table. Command buffers exist but are off in retail (`Vehicle.DisableCommandBuffers`), nothing bypasses the constant shadow that XenosRecomp doesn't cover, and the main scene uses **predicated tiling in every race frame**. Night, rain, snow, bike and in-car races are traced too (a Custom Match script reaches each): night drops the shadow maps, rain and snow also drop bloom, and the in-car view swaps the environment cube for a rear-view mirror drawn into the main targets and partially resolved. Left: a day race in rain or fog (London's menus drop timed presses). See Phase 4 findings. |
| 5. Native renderer bring-up | **In progress.** pgr3's renderer is ported and boots through the front end into a race: every shader builds, the Bink movies play, the menus match xenos, and the race scene renders with predicated tiling, the motion blur, damage-deformed cars and correctly filtered roads. Day, night, snow, bike and in-car races are checked natively, the fetch constant's LOD bias and the fetch instructions' filter overrides are modelled, and `pgr4_shadow_scale`, `pgr4_msaa`, `pgr4_fxaa` and `pgr4_anisotropy` are checked in a day race. The input script now waits on the front end's content check, so native runs reach the race at either frame rate. Left: a rain race and the rest of the exit list. See Phase 5 findings. |
| 6. Hardening and performance | **Started.** A performance audit at 3x (Phase 6 findings) found host scale copies redone for every draw: 45 of the bonnet camera's 56 ms of GPU a frame. A copy cache (`pgr4_scale_copy_cache`) brings it to under 10 ms. `pgr4_perf_gpu` times passes on the GPU. Items 2, 3, 4 and 5 are done too (a draw's CPU 2.92 to 2.73 us; the road reflection view renders at `pgr4_reflection_scale`); earlier submission is left. |
| 7. User-facing polish | **Started.** Renderer diagnostics are at the debug log level (a race logs only the adapter, the scale and one occlusion note), and F4 opens a short settings menu (built, not yet checked on screen; its resolution and reflection scales default to the console's 1x), and `pgr4_bumper_camera_height` raises the bumper view (checked in game, with a slider in the menu). See Phase 7. |

## What we know about PGR4

Everything before "Phase 0 findings" comes from reading `assets/` (no build,
no codegen). The scripts are in `scratch/` (gitignored), and each has a usage
docstring.
`assets/default_patched.xex` is a hand-made copy of `default.xex` with
`default.xexp` applied. As in pgr3, it served only for these early scans: the
build takes the patched image from `pgr4_dump_image` (Phase 1), and the two
give the same shader set.

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
  the 4 stack-built shaders (two functions, a VS and PS each). A byte scan
  cannot find a stack-built container, so none of them is in the 980.
  (Phase 1: four functions build exactly ten shaders, one per `lis`.)

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
| 8 | 128x128, one base | `k_8_8_8_8` | Perlin noise built in eight passes (mercury's `vs/psPerlin`, `psPerlin2`, `psPerlin4`; Phase 1) |
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

**Shader creation is XG-style in 6132.** The game's `sub_822B34A0` (VS) and
`sub_822B35F0` (PS) call `XGGetMicrocodeShaderParts(container, &parts)`,
allocate the header and the physical microcode themselves (from a pool, or
through `sub_8230BEB0`), fill the header with `XGSet{Vertex,Pixel}ShaderHeader`,
copy the microcode and call `XGRegister{Vertex,Pixel}Shader(header, physical)`.
The engine core's loaders (`sub_8286F7B0` VS, `sub_8286F9F0` PS) do the same
with their own allocator. All five XG functions are named in
`config/d3d.toml`. *Corrected in Phase 4:* `D3DDevice_Create{Vertex,Pixel}Shader`
are linked after all. The engine core's loaders call them instead of the XG
path when bit 2 of their flags is set, and they do not go through
`XGGetMicrocodeShaderParts`, so this census could not see shaders created
that way. The census now hooks them too. The native renderer has to hook
`XGRegister*Shader` (or the creator pairs) and `Create*Shader`.

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

### Phase 2 findings

`thirdparty/XenosRecomp` is the fork (stuken/reblue-XenosRecomp), on its
branch `pgr4` (`5f68d6f`), made from `pgr3` (`6944662`).

**First runs** (`scratch/xr-*`). With only `REBLUE_RECOMP`, 306 of 990
shaders fail DXIL; with no define (close to upstream's behaviour for 0x11),
322. Three causes, none of them about container revision 0x11, which the
fork already parses:

1. **Vertex input locations.** PGR4 declares 23 input semantics, at most 12
   per shader: `POSITION0`-`6`, `BLENDWEIGHT0`-`1`, `BLENDINDICES0`,
   `NORMAL0`-`2`, `TEXCOORD0`-`6`, `TANGENT0`, `COLOR0`-`1`. Now the
   `PGR4_RECOMP` table in `shader_common.h` (locations 0-22, so Vulkan needs
   `maxVertexInputAttributes` >= 23).
2. **Interpolators.** Some pairs pass `NORMAL0` (24 VS, 43 PS) and
   `TANGENT0`/`1` (6 VS, 22 PS). They are appended to the interpolator list
   under `PGR4_RECOMP`, so the shared registers keep their order.
3. **Vertex texture fetch** (188 vertex shaders, none in pgr3 or rr6). Two
   generic bugs, fixed for every game:
   - the microcode fetches vertex sampler *n* through fetch constant 16 + *n*,
     while the constant table names it by register *n*, so every fetch fell
     back to an undeclared `s16`-`s19`. The recompiler now maps a VS fetch
     constant >= 16 back to its sampler register;
   - a vertex shader has no derivatives, so `Sample` is invalid there. A VS
     fetch now samples at an explicit level (`tfetch{2D,3D,Cube}Level`): the
     LOD bias, in 1/16ths, as Xenia does.

   **`SetTextureLod` was dropped.** 32 VS and 103 PS set the LOD register
   and fetch with `useRegLod`; the recompiler ignored the instruction, and
   pixel shaders sampled at the computed LOD instead. It now writes
   `textureLod`, and any fetch with `useRegLod` samples at
   `textureLod + bias`. `textureLod` is declared only in shaders that use it.

**Shared engine paths.** pgr3's vertex-format and fetch work (DEC3N/DEC4N,
the UHEND3N family, integer `SHORT4` positions, the `BLENDINDICES` swap,
vertex pulling, `VFETCH`) now sits under `GOTHAM_RECOMP`, which
`PGR3_RECOMP` and `PGR4_RECOMP` both set (`shader.h`, `shader_common.h`).
Each game keeps its own vertex input table and interpolator list.

**Result:** 990 of 990 compile for DXIL and SPIR-V. Spec constants: 626 PS
`ALPHA_TEST` (`0x2`), 292 VS with both normal decodes (`0x5`), 72 with none,
the same proportions as pgr3. The full recompile takes about 5.5 s (pgr3:
2.2 s), and the generated cache source is 8.1 MB (pgr3: 4.3 MB), a 2.3 MB
object.

**The HLSL review** (step 3), shader bodies only:

- no memexport, no `call`/`return`, no point-size writes, no MRT, no `#error`;
- relative constant indexing (`a0`) in 12 VS and 2 PS, which the recompiler
  already clamps;
- integer constants and loops (`for (aL = 0; aL < i31.x; ...)`) in four PS:
  the game's `MotionBlur` and `MotionBlurSplitScreen`, mercury's
  `MotionBlur` and `psPerlin4`. **The host has to supply integer constant
  31**;
- depth output in 4 PS, cube fetches in 385 PS;
- 8 VS pull their vertices (computed fetch indices), as 10 did in pgr3.

**Regression** (step 5), comparing each shader's own HLSL with the shared
header removed, against pgr3's fork commit: rr6's 176 are unchanged. 8 of
pgr3's 309 change: pixel shaders (its road base and markings diffuse maps
among them) whose microcode sets `SetTextureLod` and now sample at that level
instead of the computed one. That is the intended fix, but pgr3 pins its own
fork commit, so it changes nothing there until pgr3 moves its submodule, and
then needs a look at the road markings.

### Phase 3 findings

- **Submodules:** `thirdparty/XenosRecomp` (`pgr4`, `5f68d6f`),
  `thirdparty/plume` (stuken `rr6`, `2f9fb4c`, with its four `contrib/`
  submodules) and `thirdparty/rexglue-sdk` (stuken `main`, `905694f`), the
  same plume and SDK commits pgr3 pins. Each was cloned with pgr3's copy as a
  reference and then made independent of it. As in pgr3 the SDK checkout is a
  pin: the build uses an installed SDK (`CMAKE_PREFIX_PATH`). miniz and zstd
  are vendored from pgr3.
- **Build files:** `cmake/native_gpu.cmake` and `cmake/shaders.cmake` are
  pgr3's, renamed. The renderer's host HLSL list is left empty until its port
  in Phase 5. The top-level `CMakeLists.txt` has `PGR4_NATIVE_GPU` (OFF keeps
  the xenos plugin) and splits sources as pgr3 does: every build shares the
  test tooling, the xenos build adds the census and resolve dump, and the
  native build globs `src/gpu`, `src/core` and `src/platform`.
- **Prelink:** `pgr4_prelink` DXC-links every subset of each shader's spec
  constants: **2,420 variants** (626 PS x 2 alpha-test states, 292 VS x 4
  normal-format states), 28.7 MB of DXIL deflated to 8.2 MB, with no
  failures. pgr3 prelinks 688 (548 at its own Phase 3, before the DEC3N
  variants doubled its vertex shaders'). The whole native build from a fresh
  directory took
  67 s, so prelinking stays complete; Phase 2's question about lazy
  prelinking is closed.
- **App:** under `PGR4_NATIVE_GPU`, `OnPreSetup` sets `config.graphics =
  nullptr`, and `OnPreLaunchModule` logs "the native renderer is not
  implemented yet", flushes the log and exits with code 1, as pgr3 did in its
  Phase 3. `src/gpu/present_count_stub.cpp` stands in for the renderer's
  present counter, which `pgr4_input_script` reads. Both exes load the
  patched image and exit this way.
- **Regeneration checked:** touching `shader_common.h` relinks XenosRecomp
  (it compiles the header in) and reruns the recompile; touching
  `default.xexp` reruns the image dump, extraction and recompile. Both
  rebuild `pgr4_shader_cache` and relink the exe.

### Phase 4 findings

**Method.** Shape matching against pgr3's names (`match_names.py`) gives
1.00 only for leaf code: `D3D_AllocDevice`, `D3DDevice_Release`, the lock
helpers, `XGGetBlockDimensions`, `D3D__FlushCachedRange` and the four XAPI
allocators. Every function that touches the device or a resource header was
rewritten between 2099 and 6132, so the rest were found by behaviour with
`funcdb.py`:

- **The state tables**, `{Get, Set, Default}` per entry, are in `.data` at
  `0x82A640D0` (101 render states) and `0x82A64590` (20 sampler states);
  `D3D__InitializeStateTables` (`sub_826DA5B0`) is their only reader. The
  render-state enum is not 2099's: 6132 has 101 entries to 2099's 97, and
  from `WRAP0` on pgr3's index is one higher. Index *k* is `D3DRS` value
  *4k*, the enum UnleashedRecomp uses (10 `ZENABLE`, 14 `CULLMODE`, 50-56
  `SCISSORTESTENABLE`..`COLORWRITEENABLE3`, 60-75 `WRAP0-15`, 76
  `VIEWPORTENABLE`, 81 `HALFPIXELOFFSET`, 83 `PRIMITIVERESETINDEX`, 86-89 the
  guard and discard bands, 96/97 `HIZENABLE`/`HIZWRITEENABLE`, 98
  `LASTPIXEL`). The defaults agree, and each setter writes the register its
  state names (`MULTISAMPLEMASK` writes `PA_SC_AA_MASK`, `LASTPIXEL`
  `PA_SC_LINE_CNTL`, the bands `PA_CL_GB_*_ADJ`). Entries 0-9 are a getter
  returning 0 and an empty setter. `scripts/re/state_setters.py` now pairs
  setters by best shape instead of by index; its ratios across the two XDKs
  (0.2-0.7) are only good for spotting which device field a store moved to.
- **The draw-time flush**, inlined into `DrawVertices`, `DrawIndexedVertices`
  and `BeginVertices`, writes each Xenos register group from one contiguous
  device block, chosen by bits of the dirty masks. That maps every register
  shadow at once (the table is in `src/gpu/d3d.h`).
- **PM4 opcodes** each function writes (`scratch/p4/pm4_opcodes.py`): no
  code outside D3D writes packets.
- `.pdata` (`0x8226AB80`..) gives exact function extents.

**The hook table.** Everything pgr3's renderer and tooling hook or call has a
PGR4 address, and `config/d3d.toml` and `config/xapi.toml` name 86
functions (81 and 5) with no codegen warnings:

| pgr3 (2099) | PGR4 (6132) |
|---|---|
| `Direct3D_CreateDevice` | `0x826CD270`; allocates **`0x5780`** (2099: `0x5080`); type 2 makes a command-buffer device |
| `D3DDevice_Swap` / `Present` | `0x826CB148`; the game resolves and swaps itself. `Present` (`0x826CB6E0`) exists, but only mercury calls it |
| `Clear` (stencil `r9`) / `ClearF` | `0x826D1A08` / `0x826D18E0`, the same arguments. `ClearF` is now 73 instructions around `sub_826D1250` |
| `Resolve` | `0x826CBEE0` |
| `DrawVertices`, `DrawIndexedVertices`, `DrawVerticesUP` | `0x826CFF58`, `0x826D0340`, `0x826CFF10` |
| `BeginVertices` / `EndVertices` | `0x826CFA60` / `0x826CFF00` |
| `Create{Vertex,Pixel}Shader` | `0x826CA860` / `0x826CA710`, mercury only; the game uses `XGRegister*Shader` (Phase 1) |
| `CreateVertexDeclaration` | `0x826CA490`, **no device argument** in 6132 |
| `Set{Vertex,Pixel}Shader`, `SetVertexDeclaration` | `0x826CA130`, `0x826C9E30`, `0x826CA348`; `SetFVF` `0x826CA950` |
| `Set{Vertex,Pixel}ShaderConstantF` | `0x826C9908` / `0x826C99F0`, the `...FN` cores: the game's inline wrapper computes the dirty mask and passes it in `r7` |
| `SetPixelShaderConstantB` | `0x826C9B78` (and the VS, `I` and getter variants beside it) |
| `Set{RenderTarget,DepthStencilSurface}` | `0x826C64B0`, `0x826C6818` |
| `D3DResource_Destroy`, `D3DSurface_GetDesc` | `0x826CF0C8`, `0x826C8C40` |
| `D3D__MakeSpace`, `D3D__AllocCommandMemory` | `0x826CE8C0`, `0x826CDA98` (device, dwords, align) |
| `XPhysicalAlloc` / `XPhysicalFree` | `0x826A8B30` / `0x826A8BC0` |
| `XInputGetState` | `0x826A8F00` |
| `D3DQuery_Issue` / `GetData`, `CreateQuery` | `0x826D2F00` / `0x826D3188`, `0x826D3D48`; a query is a self-contained 156-byte object |
| `BeginTiling` / `EndTiling` | `0x826D1C38` / `0x826D20D0`, one game and one mercury caller each |
| `BeginShaderConstantF4` | **not linked**, and nothing writes `SET_CONSTANT` outside D3D |
| `VdGlobalDevice` import | `0x82000898` (resolves to the same kernel variable as pgr3's `0x820005A0`) |

**What 6132 adds** (Phase 4 step 2), and what each means for Phase 5:

- **Command buffers: used, but switched off in retail.** The vehicle
  renderer creates a second device with `Direct3D_CreateDevice(0, 2, ...)`
  (`D3DDEVTYPE_COMMAND_BUFFER`), records each car's draws into a command
  buffer once (`sub_82423218` begins, `sub_82423648` ends) and replays it with
  `D3DDevice_RunCommandBuffer` (`0x826D6610`), whose draws never reach the
  draw hooks. That happens only while the game tweakable
  `Vehicle.DisableCommandBuffers` (value byte at `0x82AAABE4`) is 0, and its
  default is 1, so vehicles draw through the device. The renderer should keep
  the byte at 1 and warn if `RunCommandBuffer` is ever called.
- **GPU-loaded constants.** `SetPixelShader`/`SetVertexShader` emit
  `LOAD_ALU_CONSTANT` packets that load a shader's literal constants from its
  own microcode (`sub_826DEBD8`) and clear those registers' dirty bits, so the
  device's float-constant shadow never holds them. XenosRecomp already bakes
  revision-0x11 float and integer literals into the HLSL, so nothing is lost.
- **Z pass and predication.** `D3DDevice_BeginZPass`/`EndZPass` (by
  behaviour, `0x826D2590`/`0x826D28D8`, one game caller, `sub_824CE250`) and
  `D3DDevice_SetPredication` (`0x826D1AC8`). The draws in a Z pass still go
  through the draw hooks once; the replay is the guest ring's, which the
  native renderer discards.
- **No game-written PM4**, and `BeginShaderConstantF4` is gone, so pgr3's
  ring-written constants (`hooks/constants.cpp`) have nothing to model here.

**The device and resources** (`src/gpu/d3d.h`, every offset with a static
assert):

- The header is six 64-bit dirty masks (`+0x00`..`+0x2F`), the ring pointer,
  end and limit at `+0x30`/`+0x34`/`+0x38` and the reference count at `+0x3C`
  (2099: ring at `+0x00`, masks from `+0x10`). The state-call tables follow
  at `+0x40`, `+0x1D4`, `+0x224`, `+0x3B8`.
- Fetch constants (`+0x480`), float constants (`+0x780`, `+0x1780`) and the
  bool and loop files (`+0x2780`, `+0x27A0`) are where 2099 had them. The
  clip planes are at `+0x2820`.
- The register shadows are blocks in register order: `0x2000` at `+0x2880`,
  `0x2100` at `+0x28CC`, `0x2180` at `+0x2920`, `0x2200` at `+0x2934`
  (`RB_DEPTHCONTROL`; 2099 `+0x2D74`, so the `0x21xx`/`0x22xx` shadows moved
  by -0x440), `0x2280` at `+0x2964`, `0x2300` at `+0x29B8`, `0x2380` at
  `+0x2A50`. The window scissor is not on a mask: `sub_826C33C0` writes it
  into the ring at once.
- The bindings: index buffer `+0x308C`, render targets `+0x3090`, depth
  `+0x30A0`, stream buffers `+0x30A4` (no offset field; it is folded into
  vertex fetch constant 95 - stream), strides `+0x30E8`, textures `+0x30F8`,
  viewport `+0x3160` **as floats**, scissor `+0x317C`, pixel and vertex
  shaders `+0x318C`/`+0x3190`, declaration `+0x2E24`, blend staging
  `+0x2E40`/`+0x2E44`, tile count `+0x31C4`, behaviour flags `+0x5708`.
- **`D3DResource` is 24 bytes** (2099: 12): `Common` with the
  `D3DRESOURCETYPE` in bits 0-3 (1 VB, 2 IB, 3 texture, 4 surface,
  5 declaration, 6 VS, 7 PS, 8 constant buffer, 9 command buffer), then the
  reference count, fence, read fence, identifier and base flush. So a
  texture is 52 bytes with its fetch constant at `+0x1C`, a surface 48 bytes
  (`RB_SURFACE_INFO` `+0x18`, colour/depth info `+0x1C`, size `+0x24`,
  `D3DFORMAT` `+0x28`, EDRAM size `+0x2C`), and a vertex or index buffer 32
  bytes with its two words at `+0x18`/`+0x1C`. Shaders and declarations are
  resources too.

**Checked live** (`scratch/p4/run1`): `pgr4_trace_frame` is ported into
`xenos_resolve_dump.cpp` and decodes every header through `gpu/d3d.h`. A
scripted run to the St Petersburg race (Phase 0's script, frames 2100-2102,
`scratch/p4/trace_summary.py` and `pass_list.py`) shows:

- **The headers agree.** Surfaces decode to the sizes, formats and EDRAM
  bases the resolves imply (a 1280x720 colour surface is 3,686,400 bytes,
  720 tiles, and its depth sits at base 720). Vertex buffers carry `Common`
  `0x00200001`: type 1 in cached memory (bit 21, which
  `D3DDevice_CreateIndexBuffer` sets for usage 4), without the D3D-created
  bit, so **gotham4 builds its own buffer headers**, as gotham3 did. Their
  `Fetch0` holds the virtual address; `SetStreamSource` writes the physical
  one into the device. Index buffers carry `0x20200002`. Surfaces D3D made
  carry `0x00100004` (`0x80100004` with EDRAM it allocated); the game's
  aliases carry `0x00000004`.
- **The two main-scene surfaces trade roles every frame.** One frame
  `0x4098E180` is the colour target and `0x41B4D520` the depth, the next the
  reverse; the game rewrites their headers in place (format, base, MSAA and
  size all change). The renderer must not cache a surface's header by its
  address.
- **What does not run:** `D3DDevice_RunCommandBuffer` (never called), the Z
  pass and `D3DDevice_Create{Vertex,Pixel}Shader` (not in the front end or
  this race). The vehicle renderer does create its command-buffer device
  (`Direct3D_CreateDevice` type 2, about 20 s after the main one): creation
  comes before the option check.

**A race frame** (St Petersburg, day, overcast; 43 resolves in each of the
three frames. Phase 0's frame had 42, without the 256x256 second shadow):

| Pass | Target | MSAA | EDRAM format | Draws | Resolves to |
|---|---|---|---|---|---|
| minimap | 200x200, D3D's own EDRAM (base 720) | 1x | 8888 | 8, after a clear | 200x200 `k_8_8_8_8` |
| Perlin noise | 128x128 alias | 1x | 8888 | 1 per pass | eight resolves into one texture |
| environment cube | 256x256, with 256x256 depth | **4x** | 7e3 | 7-54 per face | six faces of a `k_16_16_16_16_EXPAND` cube (flags `0x300`, clearing) |
| car shadow | depth only, 512x512 | 1x | `D24FS8` | 160 | `k_24_8_FLOAT` (flags `0x4`) |
| second shadow | depth only, 256x256 | 1x | `D24FS8` | 120 | `k_24_8_FLOAT` |
| quarter-resolution view | 320x180, with depth, cleared to Z 0 | **4x** | 2_10_10_10 | 664 | `k_2_10_10_10_AS_16_16_16_16`, then blurred twice (1 draw each): the scene from a camera behind the car, the lower half left at the clear colour |
| **main scene** | 1280x720 as **two predicated tiles**, 1280x480 and 1280x240 | **2x** | 7e3, `D24FS8` | about 3,330 | depth `k_24_8_FLOAT` 1280x720 (flags `0x14`); `EndTiling` resolves colour per tile to `k_16_16_16_16_FLOAT` |
| after the scene | 1280x720 alias, 1x depth | 1x | 7e3 as 16_16_16_16 | 93 | the same FP16 scene texture |
| luminance | 64x64, 16x16, 4x4, then 1x1 twice | 1x | FP16, then 32_FLOAT | 1 each | FP16, then `k_32_FLOAT` |
| bloom | 320x180, 160x90, 80x45, 40x22 (three each), then 320x180 | 1x | 8888 | 1 each | `k_8_8_8_8` |
| final | 1280x720 front buffer | 1x | 7e3 | 35 (tone map, HUD) | `k_2_10_10_10_AS_16_16_16_16` (flags `0x300`) |

So against PGR3's race: **predicated tiling in every race frame** (PGR3 used
it only outside races), 2x MSAA on the main scene and 4x on the cube and
the quarter-resolution view, 7e3 EDRAM colour as in PGR3, float depth with
reversed Z, and a second shadow map. MRT does not appear (render target 1 is
only ever set to null).

**Other race kinds** (`scripts/runs/custom_race.sh`, results in
`scratch/runs/`). The front end's **Custom Match** (Single Player, three
`right`s from Arcade Mode) sets city, weather and vehicle directly, so one
script reaches every kind: Street Race, then a weather from the grid (row 1
clear, light rain, heavy rain, cloudy; row 2 snow, fog, heavy fog, storm;
row 3 icy, showers), the lobby, optionally a vehicle change, and Start. Its
default city is Tokyo at night. Each run traced and dumped one race frame
with the census on:

| Race (Tokyo, Enzo unless noted) | Resolves | Against the day race |
|---|---|---|
| night, clear | 41 | no shadow passes (neither the 512x512 nor the 256x256 depth map); everything else the same, bloom included |
| night, heavy rain | 28 | as clear night, and **no bloom chain** (13 resolves fewer). The rain adds no target or resolve: it is drawn inside the existing passes |
| night, snow | 28 | the same as heavy rain |
| night, clear, **Ducati Monster S4RS** | 41 | the same as the clear night car race |
| night, clear, **in-car view** | 36 | no environment cube; a rear-view mirror pass (below) |

Every kind keeps the two-tile 2x MSAA main scene and the 4x
quarter-resolution view, every outside view the 4x environment cube, and
none reaches `RunCommandBuffer`, the Z pass or `Create*Shader`. Weather and
bikes therefore add no render target, format or pass to model; they only
switch passes off.

**The in-car view** is `LB` twice, at least 4 s after the race starts (a
single press, or one earlier, does nothing; `up` and `left` change the
music track). Its frame has 36 resolves: the environment cube is gone, and
a **rear-view mirror** pass is added. The mirror draws 368 draws into the
main scene's own 2x MSAA surfaces, before and outside the tiling, and is
resolved as a 424x96 rectangle into an FP16 texture: a partial resolve
from the shared EDRAM targets, which the renderer has to support.

One thing is still open. **Day weather:** Tokyo is
night only, so a day race in rain or fog needs another city. London is on
the grid (`CITY="up up left"`) and has day weather (Clear, Overcast, Light
Rain along its top row), but its menus drop presses while their previews
load, and three timed runs each lost a different press. A day weather race
needs a script that waits on what the screen shows, or one manual run.

### Phase 5 findings

The port is pgr3's renderer with PGR4's names, its 6132 device layout and
these changes. Each was found by comparing a native frame with xenos.

- **Device reset.** After `CreateDevice`, the game calls `D3DDevice_Reset`
  (`sub_826CD008`), which reallocates the ring through
  `SetRingBufferParameters` and then waits on a GPU that never reads it. The
  native device ignores both.
- **Declarations.** `BLENDINDICES` comes as `INT1` (`0x2C83A1`), which maps to
  `R32_UINT`.
- **Predicated tiling.** The guest surfaces bound at `BeginTiling` are one
  tile's size (1280x480). The host binds surfaces of the tiles' extent
  (1280x720) in their place until `EndTiling`. It also sets the device's
  tiling fields (`tilingFlags` 0x20, the tiled targets, the extent at
  +0x3374) so that `sub_826C6100` clamps viewports to the extent, as it does
  while the guest tiles. `EndTiling` is one resolve of the whole surface.
- **Rect list winding.** Full-screen passes are rect lists, and Xenos culls
  them like any triangle. The motion blur composite (`ps_821AE5A0`, the first
  draw of the after-scene pass) draws a clockwise triple under
  `D3DCULL_CCW`. The host's rect expansion reversed the winding, so the
  composite was culled. Its target was then never redrawn, and each frame's
  glass, shadows, rain and particles piled up on it. That was the smearing
  in the menus, and the "huge cars" and black course in races. The
  expansion now keeps the guest's winding.
- **Vertex textures.** The car and bike shaders' damage variants (for example
  `vsCarPaint7` against the undamaged `vsCarPaint6`) displace each vertex by
  a crumple map, a control mesh and a scale map sampled in the vertex shader
  (`g_CrumpleMap`, `g_ControlMeshTex`, `g_ScaleMap` on samplers 0-2), and
  the palette variants read `g_VertexColourPalette` on sampler 3. Vertex
  sampler n is the device's sampler slot 16+n (slot 19 holds the 1x267
  palette). XenosRecomp gave them the pixel samplers' descriptor slots, so
  the cars were displaced by the pixel shader's textures into spikes. PGR4's
  vertex shaders now read their indices from `SharedConstants` bytes
  736-799, and the host binds slots 16-19 there. The control mesh is an 8x6
  k_8_8_8_8 linear texture with packed mips, which the game fills through
  LockRect with 0x7F (no displacement) per lattice node. Packed mips put its
  base level in the mip tail, 16 texels (0x40 bytes) past the fetch base,
  where LockRect points; the base itself holds zeros. The mirror applied the
  packed-mip offset to tiled textures only, so it read zeros, a -0.5
  displacement that dented every car. Linear textures now take it too.
- **FVF declarations.** Mercury's 2D path (`sub_8285B278`) sets its vertex
  format with `D3DDevice_SetFVF`, which builds a declaration inside the
  device and binds it without `SetVertexDeclaration`. The Bink movie planes
  (YUV, three k_8 textures) draw through it as an XYZ | TEX1 quad, so the
  host drew them with the previous declaration and the movies stayed black.
  The SetFVF hook now builds the matching declaration (one per FVF) and binds
  it.
- **Cube mips.** Quebec's church brick reflected the sky and clouds where
  xenos shows them on the windows only (`reflect.rdc`, one
  `psSceneDiffuse` draw for the whole facade). The building shaders scale
  the environment cube (`cubeMap`, slot 7) by Fresnel times the material
  map's red channel, which is mid-grey on brick too, and sample it at
  level (1 - green) * `g_buildingSettings1.z` (7), green being the glass
  mask. So brick reads the cube's blurred low mips, close to a flat tint,
  and glass the sharp base. Cube mirrors uploaded the base alone, so brick
  reflected as sharply as glass. They now take the chain at `mip_address`
  (every face per level, a face `array_slice_stride_bytes` apart): the
  race's cubes have 7 to 9 levels.
- **Volume swizzle.** The night road shaders sample a 64x64x32 k_8_8_8_8
  headlight cookie (`headlightMap`, sampler 15) as a volume, fetched ZYXW,
  for `.x`. Volume mirrors did not take the fetch swizzle, so the road read
  another channel and was covered in white speckle at night. Volumes now
  take the same component mapping as 2D textures.
- **Fetch LOD modes.** A pixel shader fetch that sets both the computed and
  the register LOD takes the register LOD as a bias on the computed one
  (Xenia folds it into the gradients), not as the level. XenosRecomp
  sampled the register LOD as an explicit level. PGR4's road shaders fetch
  their base maps so with the register LOD at 0 (135 such fetches in the
  set), so the road sampled level 0 and shimmered. PGR4's build now emits
  SampleBias for them. A fetch with neither LOD source samples at an
  explicit level (the instruction's bias alone), where XenosRecomp let the
  hardware compute one: 91 pixel shaders, mostly the metallic car paint
  looking its colour up in a 3x512 table whose sampler minifies with point
  filtering.
- **Aliased resolve destinations.** The bonnet camera (LB three times)
  reflects the previous frame's scene: its paint (`psCarPaintMetallicBonnet*`,
  sampler 12) reads a k_16_16_16_16_EXPAND header pointed at the memory of
  the scene resolve (0x400CD27C). That header (0x40E5D08C) had taken the
  front end's depth resolve, and the host bound that stale target by its
  address, so the bonnet was black. A header now samples its own resolve
  target only while it still describes it (the same format, or the base of
  its latest resolve), and otherwise the target whose latest resolve wrote
  the memory it points at.
- **6132's 7e3 format word.** PGR4's 7e3 EDRAM surfaces report D3D format
  0x1A2201BF (GPUTEXTUREFORMAT 63, as PGR3's 0x1A22ABBF), which pgr3's
  table carried as an FP16 alternate, so the host never gave PGR4's 7e3
  clears alpha 1. The tone map writes RGB only into the final target, and
  the loading screen before a race blends the last menu frame resolved from
  it (0x400CD380) by its alpha, so it showed black where xenos shows the
  menu's car. Both words now count as 7e3.
- **NaN on 7e3 targets.** Xenos' 7e3 EDRAM formats cannot hold NaN,
  infinity or values above 31.875, so what a pixel shader writes is clamped
  on the way in; the host's FP16 stand-in keeps a NaN. At a resolution scale
  above 1 they showed as black blocks: a single-frame smudge when the
  burnout smoke produced one (spread by the motion blur and bloom), and a
  sharp rectangle on the road when the 320x180 reflection view held a patch
  of them, which the B-spline upscale to the scene's scale filled in solid
  (found from a RenderDoc capture). PGR4's pixel shaders now end with
  `clamp7e3(oC0)` under `SPEC_CONSTANT_CLAMP_7E3`, which the host sets while
  the bound target's GPUTEXTUREFORMAT is 63 (the scene) or 54 (the
  reflection view, 0x182800B6).
- **Motion blur.** `ps_821AE5A0` rebuilds each pixel's position from the
  scene depth (`g_vDepthScale`, c158), reprojects it with the previous
  frame's matrix (`g_mMatrix`, c154-c157) and samples along the difference,
  only where the scene's alpha is 1. So the host clears the 7e3 scene's
  alpha to 1 at `BeginTiling`, as the resolve's clear already did.
- **Fetch constant LOD bias.** The fetch constant's `lod_bias` (10 bits
  signed, in 1/32nds) went to the host sampler's `MipLODBias`, which D3D12
  does not apply to `SampleLevel` and Vulkan does, so the backends
  differed on explicit-level fetches. PGR4's shaders now add it in the
  shader, as Xenia does: each sampler reads `{name}_LodBias` from
  `SharedConstants` (bytes 800-863 for the pixel slots, 864-879 for the
  vertex slots) on top of the instruction's bias, every fetch that is not
  an explicit level is a `SampleBias`, and the host sampler's
  `MipLODBias` is 0. In the front end and races only slot 13 carries a
  bias (-1, the car paint's 3x512 table and a 512x512 texture), so the
  picture barely changes.
- **Fetch filter overrides.** A fetch instruction can override its
  fetch constant's filters (`magFilter`, `minFilter`, `mipFilter`; 3 keeps
  the fetch constant's) and anisotropy (`anisoFilter`; 7 keeps it).
  XenosRecomp ignored them. PGR4's pixel shaders use four combinations:
  anisotropy off (276 fetches: the light, material, bump and specular maps
  of `psSceneDiffuse*`, `psSceneRoadSurfaceGotham4*` and the puddles),
  point with a point mip (14) or the constant's mip (10) (`psCrowdSprite*`,
  `psCrowdGeneric*`, `psShadowGaussBlurHorizontal`), and linear (10:
  `psShadowGaussBlurVertical`, `psSkinnedModel_Shadow_*_Day`). XenosRecomp
  maps each to a sampler variant (`SAMPLER_VARIANT_NO_ANISO`, `POINT`,
  `POINT_MIP_POINT`, `LINEAR`) and reads that variant's sampler index from
  `SharedConstants` bytes 880-1135 (16 slots per variant). The host decodes
  the fetch constant with the override applied for each pixel slot. Any
  other combination, or one in a vertex shader, fails the compile. So
  `pgr4_anisotropy` no longer forces anisotropy onto fetches that turn it
  off.
- **Impact effect.** In crash frames the final pass is `ps_821ADE28`
  (`41026B823D173563`) in place of the tone map `ps_821ADB68`, driven by
  `g_vGrading2` (c172). `.y` ripples the screen UVs, and `.x` blends toward
  a grain, `frac(n² / (7907 + 109·x))` with `n = 128u + 92160v`, which at
  720p aliases into a coloured fish-scale moiré. Xenos draws the same
  (c172.x 0.37 on the first crash frame, fading over about 1.5 s), so the
  "static" is the game's effect, not a host fault.
- **Post-processing cvars.** `pgr4_fxaa` was keyed to PGR3's composite
  hash and did nothing; it now runs after PGR4's tone map
  (`DFF1E0F0383C494B`). `pgr4_shadow_scale` defaults to 1, the console's
  shadow sizes. Checked in the St Petersburg day race against the
  defaults: `pgr4_shadow_scale` 2 and 4 resolve the car shadow map at
  1024 and 2048 (and the three 256x256 maps at 512 and 1024), with
  sharper car outlines in the map; `pgr4_msaa=false` steps the edges (edge
  contrast 91 to 115); `pgr4_fxaa=true` softens them (91 to 73) and the
  textures a little; `pgr4_anisotropy=16` sharpens the road (detail 5.3 to
  9.8 near, 11.4 to 12.2 far).

**Race kinds.** `NATIVE=1 scripts/runs/custom_race.sh` drives the native
build through the same Custom Match script as Phase 4 (and
`scratch/runs/retry_race.sh` reruns it until the traced frame is a race
frame). Night, snow, bike and in-car races render as on xenos. The native
frame rate varies 2x between runs while the boot screens advance by time,
so timed presses used to land on the wrong screen: a slow boot let the
profile dialog take the press meant for Single Player. The script now
presses `start` every 4 s through the boot screens until
`wait:content#2`, the front end's second `XContentCreateEnumerator`
(`0x826B1658`, about 1 s before the main menu), drops the spare presses,
and times every later press from there. Since then the D3D12 and Vulkan
night races and six day races each reached the race on the first try. The
heavy rain race has not been run with it yet.

**Drivers and riders: fixed.** The cars had no drivers and the bikes no
riders, and on the main menu the driver showed as stray geometry under the
car. The skinned models' vertex shaders (`vsSkinnedModel_*`) skin through a
bone palette fetched as a vertex texture, `MatrixPaletteSampler`. That
palette is a 156x1 linear `k_16_16_16_16_FLOAT` texture: 52 3x4 matrices,
3 texels a bone. There were two faults:
- `GetOrCreateNativeMirror` rejected the format (the ~360 rejected
  native textures a race logged), so the palette sampled the null texture.
  It now mirrors as R16G16B16A16_FLOAT.
- The fetches use unnormalized coordinates (`3 * index + 0.5 .. 2.5`, the
  instruction's `texCoordDenorm` bit), which XenosRecomp ignored. So every
  vertex read the clamped edge texel and the mesh collapsed to one point
  (RenderDoc: all post-VS positions equal). The recompiler now wraps such
  coordinates in `denormTexCoord2D`/`3D`, dividing by the texture's size as
  Xenia does (PGR4_RECOMP only, not for cubes).

An opponent's rider then drew as a black silhouette (`black_driver.rdc`, a
bike on the grid at 2x). Its draws enable shadow receiving (PS boolean b8)
with a "no shadow" matrix in c56-c59, whose w row is 0. The pixel shader
projects by `1 / TEXCOORD5.w`, clamped to FLT_MAX as Xenos does. Scaled by
the shadow map's size in `getWeights2D`, that overflows, and `frac(inf)` is
NaN. The NaN weights reached the output, which the 7e3 clamp turned to
black. The texture unit's weights are fixed point, never NaN, so
`getWeights2D` now returns 0 for a NaN weight. Checked by replacing the
rider's two pixel shaders (`ps_821A37A0`, `ps_821A47F8`) in the capture:
the old helper reproduces the black, the new one draws the suit and helmet.

The input script also takes `capture@<s>s`, which triggers a RenderDoc
capture when the game runs under RenderDoc.

**Rider shading: fixed.** On the vehicle select screen (Arcade Mode, down
twice to the Buell; `scratch/runs/menu_bike.sh`) the rider's suit had
hard-edged shading bands and the helmet a patchy chrome pattern where xenos
shows smooth gradients and a red helmet. The skinned vertex declaration
mixes the tangent-basis packings: NORMAL is 11:11:10 (`kDec3NAlt`), TANGENT
10:10:10:2 (`kDec4N`). The decode was chosen per pipeline by two spec
constant bits, and with both set the DEC3N branch won, so every normal was
decoded with the wrong bit layout. That bent the lighting (the bands) and
the reflection vector into the environment cube (the helmet's pattern was
the cube's sky and skyline, scrambled). The recompiler now emits
`tfetchPackedBasis`, which takes the packing per element from two shared
constant masks the declaration fills (`dec3nBasis` and `r11g11b10Basis` in
c30.zw, bit 8 * semantic + index), and the two spec bits are no longer
set. Ruled out on the way: the helmet's decal sheet (identical in both
builds, clamp to transparent border), the environment cube's six faces
(identical), the normal maps.

Debug aids from this: `pgr4_debug_null_slots` (a mask of host sampler slots
bound to the null texture, to find which input a fault comes from), cube
resolve targets dumped per face (`_f<n>`), the draw trace's per-slot
`clamp=` and `border=`, and the xenos build's `pgr4_dump_draw_textures=<n>`,
which writes the first four textures of every indexed draw with that index
count from guest memory.

**Open.**
- A rain race (heavy rain natively, and a day race in rain), and the rest
  of the exit list below (every city by day and night, every weather,
  replays, photo mode).
- `pgr4_fxaa` skips crash frames, whose final pass is `ps_821ADE28`
  rather than the tone map.
- ~~Regression: no flares around the head and tail lights~~ (fixed): the
  D3D12 occlusion results were never written on this machine's driver
  (Phase 6 findings, light glows), so the glows read garbage in one frame
  slot and 0 in the other.

`pgr4_dump_resolve_sources` (with `pgr4_dump_frames`) writes each resolve's
source as the resolve found it, as `pgr4_frame_N_sK_<dest>.bmp`. It shows
which pass first goes wrong. `pgr4_trace_frame` now prints every non-zero
PS constant of a traced `DrawUP`, and the xenos trace prints the PS
constants from c150 for `BeginVertices` and the VS constants from c200 for
`Draw`. The input script takes `dump@<s>s` and `trace@<s>s` entries
(fractional seconds allowed), timed like presses, so captures land on the
same scene at any frame rate; both builds honour them. A trace fills the
5 MB logs quickly, and only 20 rotated files are kept, so trace only the
frames needed.

### Phase 6 findings

**Performance audit at 3x** (2026-10-24). The race does not hold 60 fps at
`pgr4_resolution_scale=3`, worst in the bonnet camera. Measured on an AMD
Radeon 8060S (an iGPU) with `scratch/runs/perf_race.sh`. It runs the St
Petersburg day race, which starts in the bonnet camera with this profile,
then switches to the chase camera with LB three times.
`scratch/runs/perf_summary.py` splits the results by phase. The new
`pgr4_perf_gpu` (`gpu/gpu_timing.cpp`) adds GPU time per pass to
`pgr4_perf_stats`. It timestamps every framebuffer bind and host copy,
labels each interval by its target, and also reports the GPU's idle time
between lists and the video memory budget. A run whose display sleeps
throttles presents: the vblank clock stops with `0xC01E0006`, and frame
times then mean nothing. GPU busy time stays comparable.

GPU per frame at 3x, before any change:

| | Bonnet camera | Chase camera |
|---|---|---|
| GPU busy | 56 ms | 19 ms |
| `GuestScaleUpscale` 960x540 (the 320x180 reflection view, B-spline) | 1,476 a frame, 19.2 ms | 199, 2.9 ms |
| `GuestScaleCopy` 1280x720 (a 3840x2160 scene texture, box-filtered) | 57, 20.5 ms | 1, 0.4 ms |
| `GuestScaleCopy` of the shadow maps and the mirror | 153, 5.4 ms | 161, 6.5 ms |
| the main scene's own draws | 5.3 ms | 3.3 ms |

Video memory is not the limit (3.5 GB used of a 113 GB budget).

1. **Guest scale copies are redone for every draw (the bonnet camera's
   cost).** `BuildGuestScaleCopiesLocked` runs at every framebuffer bind.
   It copies each bound texture whose host scale differs from the target's
   at the target's scale, whether or not the texture changed since the last
   copy. PGR4 has many such pairs, because `Output::SurfaceScale` keeps every
   320x180 surface at 1x. That rule is RR6's glow rule, and in PGR4 it also
   catches the 4x MSAA quarter-resolution reflection view and its blurs.
   So every 3x scene draw with the reflection view bound upscales it again,
   and every reflection-view draw with a 3x scene texture bound copies
   3840x2160 down to 1280x720 again. The bonnet camera is the worst case: its
   scene draws and its reflection view keep those textures bound throughout.
   **Prototyped** (`pgr4_scale_copy_cache`, on by default): each
   `GuestTexture` carries a `contentVersion` that every host write bumps
   (`NoteTextureWrite`: a draw-target bind, a resolve, a copy, a clear, FXAA).
   A copy is redone only when its source's version has moved. GPU busy per
   frame at 3x (`perf_3x_base` against `perf_3x_cache`): bonnet camera 53 to
   9.6 ms, chase camera 18 to 9.8 ms, menus 22 to 8.7 ms. One 1280x720 and
   one 512x512 copy are left a frame. The front-buffer dumps of the two runs
   show the same scenes (bonnet reflection, mirror, shadows), but they are
   not pixel-compared. Both runs had the display asleep, so 60 fps at 3x is
   still to confirm with the display on.
2. **Slots the shader does not sample.** A null `SetTexture` leaves the
   slot's last texture bound, so stale slots still drive scale copies,
   mirror lookups, texel hashes and descriptor work. Recording each shader's
   sampler registers from its CTAB at creation, as a mask on `GuestShader`,
   would limit `BindGuestTextures`, `BuildGuestScaleCopiesLocked` and the
   shared constants' texture loop to the slots the draw reads. The chase
   camera's 161 shadow-map and mirror copies are most likely stale slots.
   **Done** (`pgr4_sampled_slots_only`, on): the CTAB alone was not enough,
   because three pixel shaders (`ps_8223B158`, `ps_8223B468`, `ps_8223B620`)
   fetch through samplers it does not name. So XenosRecomp records every
   texture fetch's slot (bit n for pixel sampler n, 16 + n for vertex
   sampler n) and, under `PGR4_RECOMP`, emits the mask as
   `ShaderCacheEntry::samplerSlotMask`. 190 shaders fetch nothing and none
   more than 14 slots; checked against the HLSL dump, no pixel shader reads a
   slot outside its mask. Binding, scale copies, the resolve-source barriers
   and the shared constants skip the other slots, which keep their last
   entries.
   Regression, fixed: `BindGuestTextures` skipped a slot whose texture and
   fetch words were unchanged within the current epoch (present frame and
   mirror generation), but kept one epoch for all slots and advanced it on
   the frame's first draw. A slot that draw did not sample stayed marked
   current, so it was never looked up again. A paint change on the car
   select screen (RB) rewrites the car's paint ramp, slot 11 of
   `psCarPaintMetallic1` (a linear 3x512 `k_8_8_8_8`), and the car stayed
   on its old (black) mirror. The epoch is now kept per slot.
3. **`Output::SurfaceScale` for PGR4 (done: `pgr4_reflection_scale`).**
   Keeping 320x180 at 1x suits RR6's glow, whose blur taps are sized for
   320 texels. PGR4's 320x180 surfaces are two chains. The bloom (8888, 1x)
   stays as it was. The other is the road reflection view: a 4x MSAA 7e3
   surface with its own depth, drawn every other frame from a camera
   mirrored below the road, resolved, blurred twice and resolved again into
   `0x412CD244`. It draws the simple road (`psSceneRoadSurfaceGotham4_Simple`),
   buildings, trees, cars and `psSceneMslSpec`, which reads the view's own
   previous resolve (b157 stays set). Twelve shaders read it
   (`reflectionMap`, slot 13, one bilinear fetch at perturbed coordinates,
   gated by b157): the road surfaces, `psSceneMslSpec*` and
   `psScenePuddle*`. The bonnet's reflection is not it: the bonnet
   is identical either way.
   That chain (the 7e3 surface, its depth and its float resolves) now
   renders at `pgr4_reflection_scale`, 1 to 3 times 320x180 (default 1
   since Phase 7, before that 3),
   never above the resolution scale, so 1x runs are unchanged. GPU at 3x,
   chase camera: 9.7 ms a frame before, 8.9 ms after. The three passes
   cost 0.64 ms at 960x540 against 0.75 ms at 320x180, which had paid for a
   per-draw scale copy of the reflection, and the upscales go. Countdown
   frames at 3x (St Petersburg fog in chase and bonnet camera, Tokyo rain)
   differ only in the crowd and the rain; the game's double blur leaves
   little for the extra resolution to show. Stair steps on a kerb at speed
   come from the game's motion blur and show at 1x too.
4. **CPU per draw: about 3.1-3.4 us.** That is 7-10 ms a frame at the
   2,000-3,000 draws of these race frames, and 16.4 ms at the 5,300 of the
   first race frames of an earlier run (geometry 5.7, framebuffer 2.7, flush
   5.4). pgr3 reached 2.4-2.6 us. Once the GPU is fixed this is the limit, because the
   guest's own frame runs on the same thread.
   - The VS and PS float constants upload in full every draw: 4 KB each,
     byte-swapped and NaN-flushed, 42 MB a frame, which the GPU then reads
     from the upload heap (1.2-1.5 ms of CPU). Reusing the last allocation
     when the guest block is unchanged (a memcmp against a host copy), or
     uploading only the registers the shader's CTAB declares, would cut most
     of it.
   - `BindGuestGeometry` mirrors and verifies all 16 stream slots the device
     holds, used or not (about 3,500 buffer checks a frame, 1.1 ms of
     hashing). Limit it to the streams the bound declaration names.
     `BindGuestTextures` likewise visits all 20 slots (see 2).
   - `CountRenderTarget` takes a mutex and a hash-map insert on every draw,
     only for a `pgr4_devmode` log. Gate it.

   **Done:** a float block identical to the one bound on the list keeps its
   binding (`pgr4_constant_reuse`, a memcmp of the guest bytes); only the
   declared streams are mirrored (`pgr4_declared_streams_only`; slot 15,
   which feeds the omitted elements, still follows the device), and
   `SanitizePipelineState` drops the other slots' strides from the PSO key;
   `CountRenderTarget` runs only under `pgr4_devmode`. With the sampled-slot
   mask, the St Petersburg race at 3x after the camera change
   (`perf_3x_audit` against `perf_3x_audit_off`, the new switches off) goes
   from 2.92 to 2.73 us a draw: flush 1.07 to 0.94 (constants 0.30 to 0.25,
   shared 0.28 to 0.21), geometry 1.35 to 1.30. Geometry is now the largest
   part, and its buffer hash (0.85 ms a frame) is one per distinct buffer
   drawn, which the cold-buffer scheme already keeps sparse. Declared
   streams barely changed the count: the game rarely leaves stale streams
   bound. Both runs throttled presents, so frame times say nothing.
5. **MSAA resolves go through a scratch texture.** A multisampled colour
   source is first resolved into a scratch texture of its own size
   (`ResolvedSingleSampleLocked`) and then copied into the destination.
   That is 3 x (resolve + copy) of 3840x2160 RGBA16F a frame at 3x (about
   2 ms). When the destination matches the source's size and format,
   resolve straight into it. **Done:** `CopySurfaceToTextureLocked` does
   so for a whole-surface resolve into a single-mip 2D destination of the
   same size and format. The three scene resolves at 3x take 1.5 ms a frame
   in the race's first camera (2.1 ms in `perf_3x_cache`), 1.1 ms in the
   menus (1.4). Partial resolves (the mirror) and FXAA keep the scratch
   texture.
6. **One command list a frame, submitted at Present.** The GPU starts a frame
   only once the CPU has recorded all of it. Two frames in flight keep
   throughput at max(CPU, GPU), but latency is their sum. Submitting at pass
   boundaries (after `EndTiling`, say) would let the GPU start sooner. Low
   priority.

**Pipeline precache** (2026-10-25). `cache/pipeline_state_cache.h` lists
736 pipelines: 702 from a manual `pgr4_pso_capture` session across the
front end and the race options at 3x, and 34 from the scripted St
Petersburg Arcade race at 1x (`scratch/runs/precache_capture2`). The list
was empty before, so on a machine with no driver cache every pipeline
compiled on the drawing thread as it was first drawn, at 10 ms or more
each: a long stall entering a race, then a hitch whenever new content
appeared. With vsync on at a matching refresh, each hitch also drains the
present queue. `PaceFrame` then runs at 0.9 of the frame period until the
queue refills, about 30 frames at 60 Hz, so the pacing stays uneven while
hitches keep coming. That is not yet confirmed on a 60 Hz display (this
machine's display sleeps and ticks at 32 Hz), and pacing on the swap
chain's frame latency object is still to do. With the 702-entry list, that
race compiled 687 pipelines on the background threads by its loading
screen and 33 on the drawing thread. 20 of those were shader combinations
the manual session never reached, and the rest differed from a listed
entry only in game state (alpha source blend, cull mode, colour write
mask), not in anything machine- or scale-specific. More sessions add to the
list the same way: run with `pgr4_pso_capture`, then copy
`<build>/cache/pipeline_state_cache.h` over the tree's copy.

**Light glows drawn through geometry (fixed: real occlusion queries).** In
`tail_lights.rdc`, a race at 3x, about 40 additive quads with no depth test
follow the scene resolve. They land on the player's tail lights and on cars
behind the left barrier. They are the light glows, and they rely on
occlusion queries to hide:

- `sub_8233F890` loops over the lights (208-byte entries). For each it
  issues a query (`D3DQuery_Issue` BEGIN 2 / END 1) around a small
  depth-tested quad with colour writes off.
- `sub_8233F708` reads it a frame later (`GetData`, 4 bytes). It fades the
  light's visibility (entry +132) up while ZPass is non-zero and down while
  it is zero.
- The glows draw scaled by that visibility.

The stub answered every query with 2^20 samples, so every glow showed.
`gpu/occlusion.cpp` now counts the draws between BEGIN and END with host
occlusion queries:

- one host query per host draw, so no query spans a pass;
- resolved at the end of the list and read when the slot comes round;
- `GetData` answers S_FALSE until then, which the game already waits on.

The 6132 `GetData` fills {ZPass, ZFail, StencilFail, Total}. The host counts
only passes, so Total reads as ZPass. The one ratio reader (`sub_826A12A8`,
1 - ZPass/Total, a per-view probe) sees all or nothing. Counts are host
samples, so they grow with the scale squared. Both readers only compare
against zero or against Total, so that is harmless.
`pgr4_occlusion_queries` (on) turns the new path off. The rr6 sun counter's
UAV (`occlusion_counting`, root descriptor 3) is still unused; its shader is
now the counting shader below.

**The driver never writes the occlusion results (fixed: pixel shader
counts).** The first version of this read garbage in one frame slot (a
stale `0xC0011000FFFF1000` pattern) and 0 in the other, every frame. So
every glow faded out, and the flares around the head and tail lights went
with them. The calls are right: RenderDoc shows each query draw between its
`BeginQuery` and `EndQuery`, the indices, and one `ResolveQueryData` at the
end of the list, and the debug layer is silent. But a standalone test
(`scratch/occtest`) shows the Radeon 8060S driver (32.0.32015.7029) resolves
occlusion and binary occlusion queries without writing anything:

- into a readback buffer the fill pattern survives, at 1x and 4x MSAA, with
  D32, D24S8 and D32S8 depth, and with a heap of 1, 4 or 1024 queries;
- timestamps and pipeline statistics resolve into the same buffer fine.

Each counted draw now carries a pipeline statistics query too. The
occlusion results are filled with `~0` before the GPU resolves them, and one
still holding it reads the draw's pixel shader invocations instead (logged
once). The test shows that count works as a stand-in:

- early depth rejects failing pixels before the shader, with `discard` in
  the test's shader too, so the count is the pixels that pass (not so for
  the game's light probes; see below);
- the driver skips the shader of a draw with every colour write off, so a
  counted draw like that (the probe) writes through a ZERO/ONE blend. The
  test shows that blend keeps the target exactly, +Inf, NaN and -Inf outputs
  included. Drivers that do write the results are untouched.

With it the player's tail lights glow again in the chase camera at night.
The headlights, behind the car, read 0. The night probe reads the full
screen (921600 at 1x), and the grid's lights in the St Petersburg day race
come and go as the cars move. The tail-light query quad is only 2 pixels.
In one captured frame it sat about 2 mm behind the lamp's depth, and it
passes in some frames and not others.

**Glows still showing through geometry (fixed: a counting copy of each
query draw).** In the Pushkin's Places intro (St Petersburg, fog), the
cockpit shot showed a car's tail-light glow on the A-pillar, and in races
CPU cars' glows showed through barriers. The answers were wrong, not the
path: nearly every light query read 2 to 8 pixels. A capture of the cockpit
frame shows all 14 light probes (3x2-pixel quads, GreaterEqual, additive,
RGB writes) pass 0 pixels against the depth buffer, the A-pillar's two
included. The probe pixel shader discards (alpha test), and on this driver
that puts the draw on late depth: the shader runs for every covered pixel,
so `PSInvocations` counted the whole quad. The standalone test's `discard`
shader kept early depth, so it did not show this. The four view probes
(`sub_826A12A8`, no discard) counted right.

So the counted draw is no longer the guest's. The guest draw records as
usual, outside any host query. Then `DrawCounted` (hooks/draw.cpp) records
the same host draws again through `Video::BindOcclusionCountPipelineLocked`:
the guest's pipeline with `occlusion_count_ps` (`[earlydepthstencil]`,
returns 0) in place of its pixel shader, depth and stencil writes off, and
a ZERO/ONE blend over a full colour write so the driver runs the shader.
The host queries go around that copy only. The copy's invocations are the
pixels that pass depth and stencil, whatever the guest shader discards, and
it changes nothing on screen. The cockpit glow is gone; the grid's visible
tail lights still read 1 to 7.

The two light query sets alternate frames: the main view's (issued from
0x82341FD0, viewport 0) and the road reflection's (0x82341D20, viewport 1,
a 320x180 4xMSAA pass from a camera mirrored below the road, the road
itself not drawn). Each is answered the next frame and read the frame
after.

D3D12 only. On Vulkan no query ever becomes available on this machine's
driver (Radeon 8060S, Vulkan 1.4.359), not even an empty
`vkCmdBeginQuery`/`vkCmdEndQuery` pair outside any render pass. That holds:

- after the fence and after `vkDeviceWaitIdle`;
- with a barrier after `vkCmdResetQueryPool`, and with a host
  `vkResetQueryPool`;
- for plume's own timestamps too (`vkGetQueryPoolResults failed with error
  code 0x1` on stderr every frame).

So `pgr4_perf_gpu` on Vulkan reports stale numbers, and the Vulkan build
still answers every occlusion query visible. Open.

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
3. **Done:** `pgr4_input_script`,
   `pgr4_null_audio`, `pgr4_dump_frames` and `pgr4_dump_raw` (pgr3's endian,
   4 KiB tiled-offset and FP16 fixes kept), the abort logger,
   `core/{logging,settings,memory_helpers}` and `scripts/re/funcdb.py` and
   `match_names.py`, with the licence files. The dumper gained what PGR4's
   race needs: `k_16_16_16_16_EXPAND` (as FP16), `k_2_10_10_10_AS_16_16_16_16`,
   `k_32_FLOAT` and `k_24_8_FLOAT` depth (stretched grey, range logged), and
   the destination slice (`r9`), so each cube face lands in its own file.
   `pgr4_trace_frame` came with Phase 4's names (Phase 4 findings).
   `Settings::Init` belongs to the native renderer (Phase 5), as in pgr3.
4. **Scripted runs to reference scenes**, in seconds from boot (pgr3 quirks
   §1). **Done:** the boot movies, title, front end and a day race (St
   Petersburg, overcast), and from Phase 4 a night race, a rain race, a snow
   race and a bike race (`scripts/runs/custom_race.sh`). **Left:** a garage.
   Dump them with `--readback_resolve=full` into `scratch/`.
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
   `pgr4_shaders` target (`cmake/shader_cache.cmake`). Since Phase 2 the
   recompiled cache depends on it, so every build extracts.
2. **Done:** `extract_shaders.py` reads revision 0x11 (nine-dword header,
   `field1C`/`field20` zero, a well-formed CTAB at `constantTableOffset`), and
   `shaders.csv` has a `name` column from the `.updb` path.
3. **Done:** the replay takes all ten stack-built shaders.
4. **Census, started:** `shader_census.cpp` (hooking
   `XGGetMicrocodeShaderParts`, and since Phase 4 `Create*Shader` too) and
   `check_shader_census.py` are ported. The front end, a day race and
   Phase 4's night, rain, snow, bike and burnout races are covered, still
   967 of 990. **Left:** photo mode, replays and split screen, watching the
   23 listed in Phase 1 findings. None has come from
   `.pak`/`.pak_hrd`/`.stream2` so far.
5. **Done:** there is no hot-reload path; the `DontShip/Shaders/...` names are
   labels (Phase 1 findings).

**Exit:** the build extracts every shader from the patched image, and the
runtime census shows 100% hits.

### Phase 2: XenosRecomp for PGR4

Steps 1-5 are done (Phase 2 findings).

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
4. Budget for scale. pgr3 prelinks 688 variants from 309 shaders. PGR4 could
   reach 2,000-plus, so measure the build time and the size of the embedded
   cache, and decide whether prelinking stays complete or becomes lazy for
   rare variants. (Measured in Phase 3: 2,420, and it stays complete.)
5. Check that rr6's and pgr3's shaders still compile with their own defines
   (pgr3's regression rule).

**Exit:** every shader compiles for DXIL and SPIR-V with 0 failures.

### Phase 3: Build integration

All done (Phase 3 findings).

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

Steps 1-3 are done and step 4's header is written (Phase 4 findings). Step
1's assumption that the state enums match did not hold: 6132's render-state
enum is the later one.

1. **Anchors:**
   - kernel imports: `VdSwap`, `VdInitializeEngines`/`VdInitializeRingBuffer`,
     `VdGlobalDevice`, `VdGetCurrentDisplayGamma`,
     `MmAllocatePhysicalMemoryEx`/`MmFreePhysicalMemory`;
   - the shader container VAs and their references, which lead to
     `Create{Vertex,Pixel}Shader` or whatever replaces them (Phase 1: the XG
     functions, `XGRegister{Vertex,Pixel}Shader` and the game's creators);
   - the `{Get, Set, Default}` render- and sampler-state tables
     (`scripts/re/state_setters.py`). The state enums are the API's, so
     index *k* is still the same state, and the device fields each setter
     writes map out the register shadows;
   - shape matching against pgr3's 74 names (`match_names.py`). Record the
     ratio for each match, and confirm anything under 1.00 by behaviour.
2. **Find out what 6132 adds over 2099.** Each item becomes a hook, a
   documented gap, or a reason the wrap-don't-replace model needs more:
   - **The frame end.** Does the game call `D3DDevice_Present`, or resolve
     and `Swap` itself as PGR3 does? (Phase 0: it resolves and calls `Swap`
     itself, `sub_822B3B08`; `Swap` has three other callers to place.)
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
   `pgr4_manifest.toml`, and re-run codegen with no naming warnings. (Done:
   86 names.)
4. Write `src/gpu/d3d.h` for 6132: allocation size, dirty masks, dispatch
   tables, fetch, float and bool constants, every register shadow pgr3 reads,
   viewport, scissor, clip planes, streams, textures, render targets, depth
   and shaders. Check the resource headers (`D3DResource`, VB, IB, texture,
   surface) against live data with a destroy log and a frame trace, as pgr3
   did. Note whether gotham4 still builds its own VB/IB headers. (Done
   through the frame trace: the headers decode, and gotham4 builds its own
   buffer headers. A destroy log is still to do, with Phase 5's lifetime
   work.)
5. **Trace a race frame of each kind** (day, night, rain, snow; car and bike;
   a cockpit view) with `pgr4_trace_frame`, and fill in the pass table:
   targets, sizes, MSAA counts, formats, EDRAM bases, MRT, depth resolves and
   rect lists. (Done for day, night, rain, snow, a bike and the in-car
   view; a day race in rain or fog is still to reach.)

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
     snow, fog and storm lighting. Phase 4's traces show rain and snow add no
     render target, resolve or memexport (no shader uses it); they draw inside
     the existing passes and switch bloom off, so what is left is shader and
     blend correctness, checked against the Phase 4 dumps;
   - **bikes and their riders** (new skinning or vertex formats; the bike
     race has the same passes as a car race);
   - **photo mode, replays, Gotham TV and split screen**, which are likely to
     use predicated tiling, larger targets or picture-in-picture corners
     (pgr3 quirks §3);
   - whatever render setup 6132 and gotham4 use (pgr3 used 1024x600, 2x,
     7e3). Known from Phases 0 and 4: 1280x720, 2x MSAA in 7e3 drawn as two
     **predicated tiles** in every race frame (so `BeginTiling`/`EndTiling`
     have to be modelled from the start, unlike in pgr3), 4x MSAA for the
     environment cube and a quarter-resolution view, an FP16 scene, an FP32
     adaptation value, a 10-bit front buffer and reversed-Z float depth. The
     main scene's colour and depth surfaces trade headers every frame.
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
- the pipeline precache (736 pipelines so far, see Phase 6 findings),
  captured with `pgr4_pso_capture` across every city,
  weather and vehicle type;
- the per-draw cost work (batched slot binding, per-thread lookup caches, cold
  buffers, sparse texel hashes), re-measured since PGR4 may draw more;
- frame pacing, the Vulkan validation-layer pass (still open in pgr3) and
  `pgr4_perf_stats`;
- optional extras: FXAA before the HUD, 16-bit output, anisotropy, texture
  dump and replace;
- real occlusion queries, which pgr3 left open (it answers every query
  visible): done (see Phase 6 findings, light glows);
- Linux, as rr6 and pgr3 have it (written, not yet built).

### Phase 7: User-facing polish

Three items for players rather than for bring-up. Only the menu's console
defaults change what a frame looks like at the defaults.

1. **Renderer diagnostics at the debug level.** The SDK's default
   `log_level` is `info`, and the renderer logs much of its bring-up
   detail at `PGR4_INFO` (65 sites, plus 61 `PGR4_WARN` and 28
   `PGR4_WARN_FIRST`). A player's log should hold what they or a bug
   report need: the adapter and its capabilities, the output and resolution
   scale, the precache summary, frame pacing, and real faults. Everything
   else moves to `PGR4_DEBUG`, which `--log_level=debug` shows again.
   - **To `PGR4_DEBUG`:** one-off bring-up facts with no diagnostic switch
     behind them. These include `D3DDevice_Swap: first frame`,
     `Direct3D_CreateDevice: guest device at`, `D3DDevice_Reset ...
     (ignored)` (`hooks/device.cpp`), `first resolve into`
     (`hooks/draw.cpp:1124`), `D3DSurface_GetDesc` (`hooks/state.cpp:100`),
     `depth surface grows`, `[output-res]` (`device.cpp`, `output.cpp`) and
     the occlusion summary (`occlusion.cpp:254`). Also the warnings that mark
     paths the game takes normally rather than faults: check each
     `PGR4_WARN_FIRST` against a clean day-race log and demote the ones it
     prints.
   - **Stay at `PGR4_INFO`:** output that a diagnostic cvar turns on, because
     setting the cvar is the opt-in. That covers `[trace]` (`pgr4_trace_frame`),
     `[perf]` (`pgr4_perf_stats`), `pgr4_dump_frames`, `pgr4_input_script`
     and the `present N` markers those tools use. Making them need
     `--log_level=debug` as well would break `scripts/runs/*` for nothing.
   - **`PGR4_DEV_INFO`/`PGR4_DEV_WARN`:** these stay as they are. They
     already need `pgr4_devmode`.
   - **Check:** a native run to the St Petersburg race at the defaults. Its
     `pgr4.log` should come to tens of lines before the race and stay quiet
     during it. The same run with `--log_level=debug` should log everything
     it logged before. Do the same for a xenos-build run, since
     `xenos_resolve_dump.cpp` and `shader_census.cpp` share the macros.
   - **Done** (2026-10-25). Twelve sites moved to `PGR4_DEBUG`, as listed
     above, together with the shader creation summaries (`hooks/shader.cpp`,
     19 lines a run). `ConvertGuestFormat`'s fallback decode is now
     `PGR4_DEBUG_FIRST` (new in `core/logging.h`): the reflection view's
     `0x182800B6` takes that path on purpose. The occlusion driver note is
     `PGR4_INFO`, because the workaround handles it. The other
     `PGR4_WARN_FIRST`s stay: none of them fired in the race. A native race
     at the defaults (`scratch/runs/log_default`) logs 46 `[pgr4]` lines,
     all of them the adapter line, the resolution scale, the occlusion note
     and the input script's own markers. With `--log_level=debug`
     (`scratch/runs/log_debug`) the demoted lines are back. The xenos build
     compiles; its log is not yet checked.
2. **A short F4 settings menu.** F4 is the SDK's generic `SettingsDialog`
   (`rex_app.cpp:420`, `bind_settings`). It lists every registered cvar by
   category: the SDK's own plus about 55 `pgr4_*`, most of them
   `pgr4_debug_*`, trace, dump and switches kept from the Phase 6 audit. The
   player's menu should show only frame rate, resolution and image quality.
   - **How:** in `OnCreateDialogs` (`pgr4_app.h`, native build), call
     `rex::ui::UnregisterBind("bind_settings")` and register our own F4 bind
     that toggles a `pgr4::gpu::SettingsOverlay` (an `ImGuiDialog`, in
     `src/gpu/settings_overlay.cpp`). It keeps the SDK dialog's
     save-to-config behaviour (the same `config_path`), and a "Show all
     settings" button opens the SDK's full dialog for developers. The SDK
     stays unchanged. If `UnregisterBind` leaves the old cvar entry
     registered, register the new bind under another name, such as
     `bind_pgr4_settings` with default key F4, and set the old bind's key to
     none.
   - **What it lists**, grouped and labelled in plain words, with the cvar
     name as a tooltip:
     - *Display:* the SDK's `fullscreen` (`UI/Window`, `window.cpp:37`). It
       is hot-reload: `ReXApp`'s change callback (`rex_app.cpp:363`) calls
       `Window::SetFullscreen`, and the size change reaches the renderer
       through `OnWindowPixelSizeChanged` and `Video::RequestResize`. The
       toggle writes the cvar and leaves the rest to that path. `monitor`
       and the start-up window size need a restart, so they stay in the full
       dialog;
     - *Frame rate:* `pgr4_fps_limit` and `pgr4_vsync`;
     - *Resolution:* `pgr4_resolution_scale` (Auto, 1x, 2x, 3x) and
       `pgr4_supersampling` (marked "restart");
     - *Image quality:* `pgr4_msaa`, `pgr4_fxaa`, `pgr4_anisotropy`
       (Game default, 2, 4, 8, 16), `pgr4_shadow_scale` (1, 2, 4),
       `pgr4_reflection_scale` (1 to 3), `pgr4_output_16bit` and
       `pgr4_display_correction`;
     - a link to the keybinds page.
     Kept out: every `pgr4_debug_*`, trace, dump, census, devmode, d3d12 and
     DRED switch, and the Phase 6 performance switches
     (`pgr4_scale_copy_cache`, `pgr4_sampled_slots_only`,
     `pgr4_constant_reuse`, `pgr4_declared_streams_only`,
     `pgr4_geometry_gpu_upload`, `pgr4_pso_*`). Those switches exist for A/B
     runs, not for players.
   - **Live or restart:** mark each entry from its cvar's lifecycle
     (`kInitOnly` is greyed out with "restart"). For each hot-reload entry,
     confirm that a change made while the game runs actually reaches
     `gpu::Settings` and the frame.
   - **Check:** F4 opens the short menu in the front end and in a race. Each
     live control changes the picture. Fullscreen toggles both ways during a
     race, with no stretched or stale back buffer and the menu still drawn
     at the new size. Saving writes `pgr4.toml`, and "All settings..."
     still reaches every cvar. The Vulkan exe too.
   - **Built** (2026-10-25), not yet checked on screen:
     `gpu/settings_overlay.{h,cpp}`, installed from
     `Pgr4App::OnCreateDialogs`.
     - **The bind.** A bind's name can't be registered twice, because its
       key cvar is registered once. So the menu has its own bind,
       `bind_pgr4_settings`, on the SDK bind's key. The SDK bind is
       unregistered and its key emptied, so the keybinds page shows no
       conflict.
     - **The window.** It auto-sizes. Shadow and reflection detail are
       combos, and supersampling is a checkbox over 1 and 2. The window has
       Save and "All settings...", which opens the SDK dialog.
     - **Restart-only options.** `pgr4_resolution_scale`,
       `pgr4_supersampling`, `pgr4_msaa`, `pgr4_shadow_scale`,
       `pgr4_reflection_scale` and `pgr4_output_16bit` are now declared
       `kRequiresRestart`, so both menus mark them. The rest of the renderer
       already reads each of them once.
     - **MSAA.** `pgr4_msaa` was read at each host surface's creation, which
       would have mixed sample counts after a change made mid-run. It is now
       read once.
     - **Live options.** Fullscreen, the frame rate limit, vsync, FXAA,
       anisotropy and display correction are read per frame or per sampler,
       so they apply live.
     - **Console defaults** (2026-10-25). The menu's resolution and image
       quality options now default to the console's values.
       `pgr4_resolution_scale` goes from 0 (auto, from the output height) to
       1 (720p), and `pgr4_reflection_scale` from 3 to 1 (320x180). The
       other entries already matched the console: vsync, MSAA and display
       correction on, FXAA and supersampling off, anisotropy `default`,
       shadow 1x and bumper height 0. Two stay above the console on
       purpose: `pgr4_fps_limit` 60 (the console ran at 30) and
       `pgr4_output_16bit` on (the console's back buffer is 8 bits). A
       `pgr4.toml` that sets either scale keeps its value.
3. **The bumper camera's height.** The bumper view sits too low in game.
   Find what sets it, then add an offset cvar.
   - **What is known:** camera placement is data, not code constants. The
     image has no `Bumper` string. It has the mount names
     (`Bonnet`, `Boot`, `RearViewMirror`, the wheels and mirrors, from
     `0x82194C4C`) and tweakables `Vehicle.Cameras.CameraIndex`,
     `OverrideCamera`, `UpdateFromSorcery` and `DisplayDebug` (`0x82194E18`
     on). Each car's audio data (`Game/Audio/<car>.dat`) carries
     `BumperCameraXOffset`/`Y`/`Z`, the listener position for that view, so
     each car's visual camera offsets are most likely in its `*_game.dat`
     inside `Game/vehicledata.pak` (a `KAP` archive) or in the car's own
     pak. `GAME:\Camera\Cams_*.bin` are the course's TV and replay cameras,
     not the player's.
   - **Find the code:**
     1. Which press of `LB` reaches the bumper view (Phase 4: twice is the
        in-car view; the plan has also called three presses the bonnet
        camera). Name the view index each press selects.
     2. From the tweakables' registration, find the vehicle camera class.
        `CameraIndex` is read where the current view is chosen.
        `DisplayDebug` probably prints each view's offsets, which would name
        the fields.
     3. Find where that class reads a view's offset (a per-car table of
        vec3s, one per view, read from the car data), and where it composes
        the eye position from the car's matrix.
     4. Confirm live: with `pgr4_trace_frame` in the bumper view, the eye
        position the game passes to the scene's VS constants (`g_cameraPos`
        is a named constant) should move by the same amount when the
        offset's Y is patched in memory.
   - **The change:** a mid-asm hook or a function override at the point where
     the bumper view's offset is read. It adds `pgr4_bumper_camera_height`
     (metres, default 0) to Y for the bumper view only. It is not a rewrite
     of the car data, so every car moves by the same amount and 0 is the
     original game. It belongs to the game code, not the renderer, so it goes
     in `config/` and a new `src/game/camera.cpp` that builds in both
     configurations, and it is listed in the F4 menu under a *Camera* group.
   - **Watch for:** the bumper view's near plane and its bonnet or bumper
     mesh culling, which may assume the original height (the car's own nose
     clipping into view). Also the rear-view mirror pass, if this view has
     one, and replays, which may reuse the same offsets.
   - **Check:** two cars of different heights (a supercar and a bike, if
     bikes have the view) at 0 and at a few positive values. They should
     match xenos at 0.
   - **Found** (2026-10-25). The first guess was wrong.
     - **Not the player camera:** the `Vehicle.Cameras.*` set is the car's
       Sorcery camera mounts. It is 11 64-byte entries at vehicle `+11600`
       (`sub_8243C5F8` indexes them, `sub_8243D658` loads them from the
       vehicle database), which `DisplayDebug` draws. The player camera does
       not read it.
     - **The player camera:** `Cam::CameraImp` (`CameraChase.cpp`). The
       image has `Camera.Bumper.CarForward`, `CarSide`, `BikeForward` and
       `BikeSide` beside `Camera.Chase.*` and `Camera.Dash.*`, and their
       only reader is `sub_827DAC20`, now named `Camera_PlaceBumper`
       (`config/game.toml`).
     - **Where the eye goes:** it copies the camera's mount position (`+304`)
       and orientation (`+320`) into its eye (`+16`, `+32`). It adds `f1`
       times the up axis of the orientation's matrix, then the bumper
       tweaks along the forward and side axes, which differ for cars and
       bikes (`+340`).
     - **The modes:** the per-frame update `sub_827D4688` switches on the
       mode at `+496`. 256 is chase (`sub_827DC9F0`) and 257 the bumper
       view, which passes `f1` = `+1492`. `SetMode` (`sub_827D5AE8`)
       zeroes `+1488..+1507` and fills them only for chase (1.51 at
       `+1492`), so **the bumper view's height is 0**: the eye sits on the
       mount itself.
     - **Other users of the placement:** mode 258, mode 259 (the dash view,
       which reads `Camera.Dash.HeadAmount`, and for bikes falls back to the
       bumper placement) and mode 768 call it with a constant 0.5.
   - **Done:** `src/game/camera.cpp` hooks `Camera_PlaceBumper` and adds
     `pgr4_bumper_camera_height` (0 to 1 m, default 0) to `f1` only while
     the camera's mode is 257. The value is read every frame, so the F4
     menu's *Camera* slider applies live. A debug-level line logs each
     camera mode that reaches the placement.
   - **Checked** (2026-10-25). In the Tokyo race the second LB press gives
     mode 257, and it was confirmed by hand in game.
     - **The eye:** a temporary probe in the hook, since removed, showed the
       eye at the mount's height at 0 and 1.0 m above it at 1.0 while
       driving.
     - **The nose:** the red nose at the bottom of the view is fixed to the
       camera, so it does not move.
     - **Standing still:** with the car stopped on the grid, the eye rose
       only about 0.24 m at 1.0, because the mount (`+304`) came out 0.65 m
       lower. Something feeds the eye back into the mount. Its writer is not
       yet found; look there if the slider's travel feels short.

---

## Risks and open questions

1. **XDK 6132 is far from 2099.** In pgr3, about 60 of rr6's 77 names matched
   at ratio 1.00. Expect far fewer here. Phase 4 depends on anchors and the
   state-setter tables more than on name matching, and it is the phase most
   likely to overrun. **Settled in Phase 4:** only leaf code matched at 1.00,
   and the rest was found by behaviour in one pass.
2. **State that bypasses the register shadows.** Constant buffers, GPU-owned
   constants or command buffers would each need their own model, as
   `BeginShaderConstantF4` did in pgr3. Command buffers could force the
   renderer to parse PM4 packets for part of the frame. Find out in Phase 4
   step 2 before committing to Phase 5's order. **Settled in Phase 4:** the
   vehicle renderer's command buffers are off unless
   `Vehicle.DisableCommandBuffers` is cleared (its default is 1), the shader
   literals that `LOAD_ALU_CONSTANT` loads are baked in by XenosRecomp, and
   nothing else writes packets. The renderer should pin the option at 1 and
   warn on `RunCommandBuffer`.
3. **Shader scale.** 990 shaders against 309 in PGR3 means three times the
   recompile, prelink and cache work, and more chances of a new instruction
   pattern. **Settled in Phases 2 and 3:** all 990 compile, the recompile
   takes about 5.5 s and the 2,420-variant prelink fits in a 67 s native
   build. The new patterns (vertex texture fetch, `SetTextureLod`) are
   handled.
4. **The xenos baseline may not reach a race.** pgr3's whole method rests on
   comparing against xenos. **Settled in Phase 0:** a scripted run reaches a
   race, and its resolves decode. Night, rain and snow races are not yet
   checked.
5. **New render paths** (weather, bikes, photo mode, split screen) have no
   precedent in pgr3. The quirks document covers the engine as PGR3 used it,
   not everything gotham4 added.
6. **The patched image.** The title update leaves shader microcode alone,
   but it changes 878 container hashes, moves code and relinks D3D9 at a
   newer QFE. As in pgr3, codegen, the extractor and the runtime must all see
   the patched image (they do: Phases 0 and 1). A census check that fails the
   build if they drift is still to do.
7. **Scale of the port.** pgr3's `src`, `cmake`, `tools` and `scripts` come to about 27k lines,
   5k of them the precache list.
   Most of the cost was game-specific findings, not the copy. Phases 0-4 leave
   the xenos build untouched and should be finished before committing to
   Phase 5.
