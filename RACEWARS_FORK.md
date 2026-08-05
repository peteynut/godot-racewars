# RaceWars engine fork

A thin, rebasable fork of Godot. Current series: branch
**`racewars-upscalers-47`** on the **`4.7.1-stable`** tag (rebased 2026-08-05;
the original `racewars-upscalers` branch off `4.6.3-stable` is kept as-is,
plus `racewars-upscalers-4.6-backup`). The 4.7 rebase renumbered the mode
contract - upstream took Scaling3DMode value 5 for `NEAREST`, so **DLSS = 6,
FSR3 = 7, MAX = 8** (mirrored in the game's `UpscalerCatalog.cs`) - and the
series gained a **MetalFX temporal correctness fix** (jitterOffset was passed
in UV units instead of pixels, and motionVectorScaleY needed negating for
Metal's y-down convention; see the game repo's `docs/metalfx-jitter/REPORT.md`
for the isolation matrix - the fix makes MetalFX temporal beat FSR2 and is
drafted for upstreaming). Its only purpose is to add the DLSS / DLAA and
FSR 3/4 upscalers that stock Godot cannot expose (no custom-upscaler hook — see
`godot-proposals#13718` and the game repo's `UPSCALING.md`, which is the design
reference for this work).

Everything here is meant to migrate to upstream's plugin API (`godot#114670`)
and retire the moment that lands.

## Scope of the fork

Only the **Windows D3D12 export templates** are ever built from this fork. The
macOS editor, macOS templates and the C# tooling stay 100% stock — Godot's
per-preset `custom_template` is a file-exists check with no version compare, so a
stock editor exports happily with fork-built templates.

## Phase status

| Phase | What | State |
|---|---|---|
| **1** | Enum / feature / viewport plumbing, SDK-free; compiles on every platform; templates CI | **done** |
| **2** | `dlss_ngx` + `ffx_upscaler` effect wrappers (NGX + ffx-api), D3D12 `has_feature` probe, dispatch | **this branch** — compiles (macOS + Windows CI); runtime-unverified |
| 3 | Hardware verification (any GPU → FSR 3.1.5; RTX 20xx+ → DLSS/DLAA; RX 7000/9000 → FSR 4.1) | not started |
| 4 | `export.sh` DLL shipping + licensing checklist | not started |

Phase 2 is compiled into the Windows templates behind `dlss=yes
ffx_upscaler=yes` (both default **off**, MSVC + D3D12 + x86_64 only). On any
build without the flags nothing changes; with the flags, availability is still
runtime-gated (NGX probe / signed-DLL presence), so machines without support
keep degrading to FSR 2 exactly as in Phase 1.

## Phase 2: how the wrappers work

Both are modeled on MetalFX (`renderer_rd/effects/metal_fx.mm`) — native
texture handles via `get_driver_resource`, SDK work recorded into the live
frame via `RD::driver_callback_add`, whose declared `CallbackResource` usages
make the render graph transition inputs to shader-resource and the output to
UAV before the callback runs.

- `effects/dlss_ngx.{h,cpp}` (`DLSS_D3D12_ENABLED`): direct NGX. Probe =
  `GetFeatureRequirements` → `Init_with_ProjectID` → `SuperSampling.Available`
  (cached; this is what `has_feature(SUPPORTS_DLSS)` returns). The NGX feature
  is created lazily inside the first driver callback (creation records GPU
  work). Quality bucket snaps from the render scale; scale 1.0 = DLAA. Links
  `nvsdk_ngx_s.lib` from a `DLSS_SDK` checkout at build time (Bevy pattern —
  the proprietary SDK is never committed).
- `effects/ffx_upscaler.{h,cpp}` (`FFX_UPSCALER_D3D12_ENABLED`): ffx-api.
  `LoadLibrary(amd_fidelityfx_loader_dx12.dll)` + 5 `GetProcAddress` entries;
  probe = upscale-provider version query on the device. Context creation
  chains upscale desc → DX12 backend desc → API version desc; the provider
  version (FSR 4.x vs 3.1.x) is printed at context creation. Headers are the
  MIT `thirdparty/amd_ffx_api/`; the signed DLLs ship beside the exe (Phase 4).
- D3D12 driver additions: `command_buffer_get_native_list` and
  `command_buffer_mark_external_commands` (drops descriptor-heap/PSO/root-sig
  caches after foreign SDK work so the driver re-binds lazily), plus the two
  `has_feature` cases.
- Dispatch: two new branches beside FSR2 in `render_forward_clustered.cpp`
  with identical input conventions (jitter = `taa_jitter * internal_size *
  0.5`, MV scale = internal size, reverse-Z, linear-HDR color, ms frame delta).
  Contexts live on `RenderBufferDataForwardClustered` like FSR2/MFX ones.

Known Phase 3 items (need hardware): MV/jitter sign check (wrong sign = obvious
smearing), D3D12 debug-layer pass over the callback resource states, reactive
mask (Godot's is an alpha-swizzle view natives can't consume — omitted),
context destroy while frames are in flight (resize during play).

### Phase 3 tuning knobs (env vars, read at launch)

SDK sign/flag conventions are settled by A/B testing on hardware; set these
before launching the exported game, watch the console wrapper, and report the
combination that looks right — it then becomes the compiled default.

| Env var | Effect |
|---|---|
| `RW_DLSS_JITTER_SIGN_X` / `_Y` = `-1` | flip DLSS jitter offset per axis |
| `RW_DLSS_MV_SIGN_X` / `_Y` = `-1` | flip DLSS motion-vector scale per axis |
| `RW_DLSS_MV_JITTERED` = `1` | set the MVJittered create flag (Godot's velocity IS computed from jittered matrices, so this is a prime suspect) |
| `RW_DLSS_NO_AUTOEXPOSURE` = `1` | drop the AutoExposure create flag (fixed 1.0 exposure) |
| `RW_FSR3_JITTER_SIGN_X` / `_Y`, `RW_FSR3_MV_SIGN_X` / `_Y` = `-1` | same flips for the ffx path |

The FSR3 availability probe is a trial `ffxCreateContext` (ground truth) and
always prints a visible verdict line: `available, provider '...'` or
`no upscale provider (rc N)`.

## The Phase 1 patch series (SDK-free)

All changes are tagged with a `RaceWars fork:` comment.

| File | Change |
|---|---|
| `scene/main/viewport.h` | `Scaling3DMode`: add `SCALING_3D_MODE_DLSS` (5), `SCALING_3D_MODE_FSR3` (6); `_MAX` → 7 |
| `servers/rendering/rendering_server.h` | Mirror `ViewportScaling3DMode` values; classify both as **TEMPORAL** in `scaling_3d_mode_type()` (activates motion vectors, jitter and target-size buffers) |
| `servers/rendering/rendering_device_commons.h` | `Features`: add `SUPPORTS_DLSS = 100`, `SUPPORTS_FSR3_UPSCALER = 101` — high explicit values, **left unbound on purpose** |
| `servers/rendering/renderer_viewport.cpp` | `_configure_3d_render_buffers`: if a fork mode is selected but `has_feature` is false, degrade to FSR 2 (mirrors the MetalFX fallback); add both modes to the FSR2 sizing group |
| `scene/main/viewport.cpp` | `BIND_ENUM_CONSTANT` for both + inspector hint string |
| `servers/rendering/rendering_server.cpp` | `BIND_ENUM_CONSTANT` for both mirror values |
| `doc/classes/{Viewport,RenderingServer}.xml` | Documented constants; `_MAX` bumped to `value="7"` so the `--doctool` classref check stays green |

### Contract with the game (`Game/Graphics/UpscalerCatalog.cs`)

| Thing | Value |
|---|---|
| `Scaling3DMode` DLSS / FSR3 | **5 / 6** |
| `Features::SUPPORTS_DLSS` / `SUPPORTS_FSR3_UPSCALER` | **100 / 101** |
| Catalog ids | `dlss`, `dlaa` (locked scale 1.0), `fsr3` |

**Why the Features bits stay unbound.** The game reads them by integer cast
(`rd.HasFeature((RenderingDevice.Features)100)`), so they need no
`BIND_ENUM_CONSTANT`. Leaving them unbound keeps `extension_api.json` and the C#
API byte-identical to stock, so the game's **stock `Godot.NET.Sdk` / GodotSharp
NuGet keeps working** against a fork-built runtime. The `Scaling3DMode` additions
*are* bound (they're a normal, additive enum extension) — the game still uses
integer casts for them, so stock GodotSharp remains valid there too.

## Build

Local sanity build (what Phase 1 is verified against — no SDKs, no mono needed):

```sh
scons platform=macos target=editor arch=arm64 -j$(sysctl -n hw.ncpu)
```

Windows mono templates are produced by CI
(`.github/workflows/racewars-templates.yml`); see that file. They need the D3D12
SDK (`misc/scripts/install_d3d12_sdk_windows.py`) and `d3d12=yes`.

## Rebasing onto a newer stable

```sh
git fetch origin tag 4.6.4-stable
git rebase --onto 4.6.4-stable 4.6.3-stable racewars-upscalers
```

The patch set is small and localized; keep the `RaceWars fork:` comments so the
conflict surface is obvious. When `godot#114670` (custom-upscaler hooks) ships in
a stable release, port the Phase 2 dispatch onto that callback API and delete
this fork.
