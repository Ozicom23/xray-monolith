# Performance overlay

A display-only overlay (it never captures the mouse or keyboard) drawn with the engine's ImGui integration on top of the UI at native resolution, so it stays sharp with DLSS/FSR. DX11 renderer only.

## Activation

| Command | Values | Notes |
|---|---|---|
| `perf_overlay` | `off`, `compact`, `detailed` | saved in `user.ltx`, applies immediately |
| `perf_overlay_toggle` | | cycles Off → Compact → Detailed; bind a key with `bind_console perf_overlay_toggle kF10` |
| `perf_overlay_pos` | `top_left`, `top_right`, `bottom_left`, `bottom_right` | default `top_right` |

Options → Video → Advanced also has **Performance Overlay** and **Overlay Position** rows.

## Contents

Compact:

- FPS and average frame time over the last second, 1% low over the last 5 seconds (refreshed 4 times per second)
- GPU frame time and CPU time (engine update + render submission) with a GPU-bound / CPU-bound indicator
- VRAM used by the game and the OS budget
- Upscaler, mode and render → output resolution, or the native resolution

Detailed adds:

- Engine CPU breakdown in ms: update, scheduler, objects, physics, AI, animation, render, render calc, HOM
- Draw calls and triangles
- Frame-time graph (last 240 frames)

## How it works

| Item | Source |
|---|---|
| FPS, frame time, 1% low, graph | wall-clock time between frames |
| CPU breakdown | the engine's `CStatTimer`s (`xrEngine/Stats.h`), collected while the overlay is on (`g_stats_gather_timers`) |
| GPU frame time | D3D11 timestamp queries around each frame, read back a few frames later without stalling |
| VRAM | DXGI `IDXGIAdapter3::QueryVideoMemoryInfo`, refreshed 4 times per second |
| Resolution, upscaler | the render target's upscaler state |
| Draw calls, triangles | `RCache.stat` at the end of the frame |

Code: `src/Layers/xrRenderPC_R4/r4_perf_overlay.*`, hooked from `dxRenderDeviceRender::Begin/End` (GPU timing). The window is built in the render pass just before ImGui renders (`seqRender` priority -4); building it earlier in the frame crashed when a `vid_restart` in the same frame recreated ImGui's font texture.

GPU-bound means GPU time is at least 90% of the frame time. With a frame cap or vsync both values are below the frame time, and the indicator shows "CPU-bound / capped".

## Not included

GPU utilization %, clocks, temperatures and power need NVAPI/NVML, which is incomplete under Wine. MangoHud shows them on Linux.
