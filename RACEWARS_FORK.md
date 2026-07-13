# RaceWars engine fork

A thin, rebasable fork of Godot, branched off the **`4.6.3-stable`** tag on
branch **`racewars-upscalers`**. Its only purpose is to add the DLSS / DLAA and
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
| **1** | Enum / feature / viewport plumbing, SDK-free; compiles on every platform; templates CI | **this branch** |
| 2 | `dlss_ngx` + `ffx_upscaler` effect wrappers (NGX + ffx-api), D3D12 `has_feature` probe, dispatch | not started |
| 3 | Hardware verification (any GPU → FSR 3.1.5; RTX 20xx+ → DLSS/DLAA; RX 7000/9000 → FSR 4.1) | not started |
| 4 | `export.sh` DLL shipping + licensing checklist | not started |

Phase 1 adds the *contract* (enum values + capability bits + safe fallback) but
**no SDK code and no upscale dispatch**. Because every stock driver's
`has_feature()` returns false for the new bits, the new modes are never selected
on any build produced today and always degrade to FSR 2 — which is exactly why
this is safe to carry ahead of the SDK work.

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
