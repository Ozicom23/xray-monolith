# Temporal upscaling: DLSS and FSR (DX11)

Two upscalers share one integration: NVIDIA DLSS Super Resolution and AMD FidelityFX Super Resolution 3.1.

## DLSS

### Version

- NVIDIA DLSS SDK **v310.9.1** (released 2026-09-08), fetched by CI (`.github/workflows/dev-build.yml`, `DLSS_SDK_TAG`).
- Ships the matching `nvngx_dlss.dll` **310.9.1** next to the exe. This is the DLSS 4 generation with the transformer models.
- The DLSS version lives entirely in `nvngx_dlss.dll`: dropping in a newer DLL updates it without rebuilding the exe.

Presets picked by NVIDIA's defaults for each mode (from the NGX log):

| Mode | Preset |
|---|---|
| DLAA, Quality, Balanced | K (first-gen transformer) |
| Performance | M |
| Ultra Performance | L |

### Supported GPUs

NVIDIA GeForce RTX 20, 30, 40 and 50 series (including laptop and professional RTX cards). GTX cards, AMD and Intel GPUs are not supported: NGX fails to initialize, the log says so, and the game keeps rendering at native resolution.

The transformer presets cost noticeably more GPU time on RTX 20/30 than on RTX 40/50.

## FSR 3.1

- AMD FidelityFX SDK **v1.1.4**, FSR **3.1.4** upscaler, vendored in `src/3rd party/FidelityFX-FSR3` (MIT license).
- AMD ships DX12/Vulkan backends only. The engine implements the FidelityFX backend interface on D3D11 (`src/Layers/xrRenderPC_R4/r4_fsr3.cpp`).
- The compute shaders are compiled for `cs_5_0` with fxc by `compile_shaders.ps1` (CI runs it before the build). Local patches to AMD's code are listed in `src/3rd party/FidelityFX-FSR3/README_xray.md`.
- Works on any GPU with D3D11 compute shaders: NVIDIA, AMD and Intel. A few passes read back from typed UAVs in formats beyond R32, which current GPUs and DXVK support.
- Quality modes use AMD's fixed ratios: Native AA 1.0x, Quality 1.5x, Balanced 1.7x, Performance 2.0x, Ultra Performance 3.0x.
- Sharpening (RCAS) is controlled by `r4_fsr_sharpness`.

## Requirements

- DX11 renderer (`renderer_r4`).
- Screen Space Shaders installed (its motion vectors are used).
- MSAA off when the renderer starts.
- HDR10 off (`r4_hdr10_on 0`): both upscalers are set up for tonemapped SDR input.
- DLSS only: `nvngx_dlss.dll` next to the exe, and on Linux (Proton) the proprietary NVIDIA driver and `PROTON_ENABLE_NVAPI=1` in the launch options.

## Settings

Options → Video → Advanced (below SMAA), or the console:

| Command | Values | Notes |
|---|---|---|
| `r4_upscaler` | `off`, `dlss`, `fsr` | applied with `vid_restart` |
| `r4_upscaler_quality` | `ultra_performance`, `performance`, `balanced`, `quality`, `native` | applied with `vid_restart`; `native` = DLAA / FSR Native AA |
| `r4_fsr_sharpness` | `0` to `1` | FSR only, applied immediately; `0` turns sharpening off |
| `r4_dlss_log` | `0` / `1` / `2` | NGX log into the game log (2 = verbose), applied when NGX initializes |
| `r4_upscaler_signs` | 4 values of `1` / `-1` | debug: motion vector x/y and jitter x/y sign multipliers |

Internal render resolution for DLSS: DLAA 100%, Quality 67%, Balanced 58%, Performance 50%, Ultra Performance 33%.

## How it works

- The whole 3D chain (G-buffer, lighting, SSS effects, combine, post effects) renders at the internal resolution. The upscaler runs just before the final `postprocess` pass; that pass and the UI run at native resolution.
- Jitter goes through the SSS `ssfx_jitter` shader constant (Halton sequence). Motion vectors are SSS's `rt_ssfx_motion_vectors`.
- 3D scope (second viewport) frames are not fed to the upscaler and are upscaled bilinearly.
- Texture LOD bias: while upscaling, `log2(render / output)` is added to `r__tf_mipbias` for scene materials (G-buffer passes, e.g. -1.58 for Ultra Performance, -1.0 for Performance), so textures and alpha-tested foliage keep output-resolution sharpness. UI and everything drawn after upscaling keep the normal bias.
- The renderer draws at `Device.dwRenderWidth/dwRenderHeight`, which differ from `dwWidth/dwHeight` only while the scene renders at upscaler resolution. Game logic (which runs in parallel with rendering) keeps seeing the real screen size.
- Code: `src/Layers/xrRenderPC_R4/r4_upscaler.*` (shared interface), `r4_dlss.*` (NGX wrapper), `r4_fsr3.*` (FSR with the D3D11 backend), `r4_rendertarget_phase_PP.cpp` (frame scope and evaluate).

## Known limitations

- Particles have no motion vectors and there is no reactive mask yet, so smoke, fire and anomalies can smear.
- Post effects (NVG noise, motion blur) are applied before DLSS.
- ReShade effects that use depth see the wrong depth buffer.
- The main menu distortion effect is not drawn while an upscaler is on.
