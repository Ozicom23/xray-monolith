# TODO: in-game performance overlay

Status: planned, not implemented.

A display-only overlay (no mouse/keyboard capture) drawn with the engine's existing ImGui integration (`xr_imgui`, `xrEngine/imgui_base.cpp`), on top of the UI at native resolution, so it stays sharp with DLSS. It also works in the main menu and on loading screens.

## Activation

- [ ] Console command `perf_overlay 0|1|2` (off / compact / detailed), saved in `user.ltx`
- [ ] Console command `perf_overlay_toggle` cycling Off → Compact → Detailed, bindable with `bind_console perf_overlay_toggle kF10`
- [ ] Options menu row "Performance Overlay: Off / Compact / Detailed" in Video → Advanced, injected by `gamedata/scripts/ui_options_modded_exes.script` like the DLSS rows; applies instantly, no `vid_restart`
- [ ] Strings in `gamedata/configs/text/{eng,rus}/ui_mm_modded_exes.xml` (`ui_mm_video_advanced_perf_overlay`, `_desc`, list keys)

## Contents

| Item | Source | Difficulty |
|---|---|---|
| FPS, frame time, 1% lows, frame-time graph | engine timers | easy |
| CPU time per engine part (render, physics, AI, scripts) | existing `CStatTimer`s in `xrEngine/Stats.h` (`EngineTOTAL`, `RenderTOTAL`, `Physics`, `Sheduler`, `UpdateClient`, `AI_*`) | easy |
| GPU frame time | D3D11 timestamp queries (`D3D11_QUERY_TIMESTAMP` + `TIMESTAMP_DISJOINT`) around the frame, read back 2-3 frames later | medium |
| GPU vs CPU bound indicator | compare CPU and GPU frame times | easy once GPU time exists |
| VRAM used / budget | DXGI `IDXGIAdapter3::QueryVideoMemoryInfo` (supported by DXVK) | easy |
| Render → output resolution, DLSS mode | `CRenderTarget::dlss_*`, `ps_r4_dlss_quality` | easy |
| Draw calls, visible lights, shadow casters | existing render stats (`RCache.stat`, `RImplementation.stats`) | easy |

- Compact view: FPS, frame time, CPU/GPU ms with bound indicator, VRAM, resolution/DLSS.
- Detailed view: everything above plus the per-part CPU breakdown, render stats and the frame-time graph.

## Notes

- GPU timestamps must work under DXVK (Proton) as well as on Windows; this is the part most likely to need a debugging round.
- GPU utilization %, clocks, temperatures and power are out of scope: they come from NVAPI/NVML, which is incomplete under Wine. MangoHud covers them on Linux.
- Overlay position (corner) and whether the detailed view shows the frame-time graph are still to be decided.
