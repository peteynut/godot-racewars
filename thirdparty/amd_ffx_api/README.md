# AMD FidelityFX API headers (ffx-api)

- Upstream: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK
- Vendored from commit `60f4ea81909200d8542eca14dccb2628b763a9a3` (SDK 2.x line,
  upscaler API version 4.1.1).
- License: MIT (header block at the top of every file).

Files (RaceWars fork, used by
`servers/rendering/renderer_rd/effects/ffx_upscaler.cpp`):

| File | Upstream path |
|---|---|
| `ffx_api.h` | `Kits/FidelityFX/api/include/ffx_api.h` |
| `ffx_api_types.h` | `Kits/FidelityFX/api/include/ffx_api_types.h` |
| `ffx_upscale.h` | `Kits/FidelityFX/upscalers/include/ffx_upscale.h` |
| `dx12/ffx_api_dx12.h` | `Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h` |

Local patch: `ffx_upscale.h`'s two relative `#include`s flattened to match this
layout (marked with a `RaceWars fork:` comment).

**Only these MIT headers are vendored.** The runtime implementation lives in
AMD's *signed, binary-only* DLLs (`amd_fidelityfx_loader_dx12.dll`,
`amd_fidelityfx_upscaler_dx12.dll`, from `Kits/FidelityFX/signedbin/`), which
are loaded with `LoadLibrary` at runtime, shipped beside the game executable,
and must never be committed to this repository or modified (AMD's binary
license forbids it; signature intact = driver-upgrade eligible).
