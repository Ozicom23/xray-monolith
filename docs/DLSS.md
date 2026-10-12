# DLSS Super Resolution (DX11)

## Version

- NVIDIA DLSS SDK **v310.9.1** (released 2026-09-08), fetched by CI (`.github/workflows/dev-build.yml`, `DLSS_SDK_TAG`).
- Ships the matching `nvngx_dlss.dll` **310.9.1** next to the exe. This is the DLSS 4 generation with the transformer models.
- The DLSS version lives entirely in `nvngx_dlss.dll`: dropping in a newer DLL updates it without rebuilding the exe.

Presets picked by NVIDIA's defaults for each mode (from the NGX log):

| Mode | Preset |
|---|---|
| DLAA, Quality, Balanced | K (first-gen transformer) |
| Performance | M |
| Ultra Performance | L |

## Supported GPUs

NVIDIA GeForce RTX 20, 30, 40 and 50 series (including laptop and professional RTX cards). GTX cards, AMD and Intel GPUs are not supported: NGX fails to initialize, the log says so, and the game keeps rendering at native resolution.

The transformer presets cost noticeably more GPU time on RTX 20/30 than on RTX 40/50.

## Requirements

- DX11 renderer (`renderer_r4`).
- Screen Space Shaders installed (its motion vectors are used).
- MSAA off when the renderer starts.
- `nvngx_dlss.dll` next to the exe.
- Linux (Proton): proprietary NVIDIA driver and `PROTON_ENABLE_NVAPI=1` in the launch options.

## Settings

Options → Video → Advanced (below SMAA), or the console:

| Command | Values | Notes |
|---|---|---|
| `r4_dlss` | `0` / `1` | applied with `vid_restart` |
| `r4_dlss_quality` | `ultra_performance`, `performance`, `balanced`, `quality`, `dlaa` | applied with `vid_restart` |
| `r4_dlss_log` | `0` / `1` / `2` | NGX log into the game log (2 = verbose), applied when NGX initializes |
| `r4_dlss_signs` | 4 values of `1` / `-1` | debug: motion vector x/y and jitter x/y sign multipliers |

Internal render resolution: DLAA 100%, Quality 67%, Balanced 58%, Performance 50%, Ultra Performance 33%.

## How it works

- The whole 3D chain (G-buffer, lighting, SSS effects, combine, post effects) renders at the internal resolution. DLSS upscales the result just before the final `postprocess` pass; that pass and the UI run at native resolution.
- Jitter goes through the SSS `ssfx_jitter` shader constant (Halton sequence). Motion vectors are SSS's `rt_ssfx_motion_vectors`.
- 3D scope (second viewport) frames are not fed to DLSS and are upscaled bilinearly.
- Code: `src/Layers/xrRenderPC_R4/r4_dlss.*` (NGX wrapper), `r4_rendertarget_phase_PP.cpp` (frame scope and evaluate).

## Known limitations

- Particles have no motion vectors and there is no reactive mask yet, so smoke, fire and anomalies can smear.
- Post effects (NVG noise, motion blur) are applied before DLSS.
- ReShade effects that use depth see the wrong depth buffer.
- The main menu distortion effect is not drawn while DLSS is on.
