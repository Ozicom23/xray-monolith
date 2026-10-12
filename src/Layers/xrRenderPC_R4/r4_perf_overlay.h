#pragma once

// In-game performance overlay (console: perf_overlay, perf_overlay_pos, perf_overlay_toggle).
// Display-only ImGui window: frame and GPU times, VRAM, render/output resolution, engine CPU timers.
class CPerfOverlay : public pureRender
{
public:
	// GPU frame timing, called from dxRenderDeviceRender::Begin / End (before Present)
	void GpuFrameBegin();
	void GpuFrameEnd();

	// Builds the ImGui window in the render pass, just before the engine renders ImGui (seqRender priority
	// -4, ImGui is -5). Recording earlier in the frame is unsafe: a vid_restart later in the same frame
	// recreates ImGui's font texture and the recorded commands would point to the released one.
	void OnRender() override { Draw(); }
	void Draw();

	void Destroy();

private:
	static const u32 kGpuQueries = 4;
	static const u32 kFrameHistory = 600;
	static const u32 kGraphSamples = 240;

	struct GpuQuery
	{
		ID3D11Query* disjoint;
		ID3D11Query* begin;
		ID3D11Query* end;
		bool pending;
	};

	bool CreateQueries();
	void CollectGpuTimes();
	void UpdateVram();

	GpuQuery m_queries[kGpuQueries] = {};
	u32 m_query_write = 0;
	bool m_query_active = false;
	bool m_queries_failed = false;
	float m_gpu_ms = 0.f;

	CTimer m_frame_timer;
	bool m_frame_timer_started = false;
	float m_frame_ms[kFrameHistory] = {};
	u32 m_frame_pos = 0;
	u32 m_frame_count = 0;
	float m_graph[kGraphSamples] = {};
	u32 m_graph_pos = 0;

	// Values shown on screen, refreshed a few times per second so they stay readable
	float m_refresh_ms = 0.f;
	float m_show_fps = 0.f;
	float m_show_frame_ms = 0.f;
	float m_show_low_fps = 0.f;

	struct IDXGIAdapter3* m_adapter3 = nullptr;
	bool m_vram_failed = false;
	float m_vram_used_gb = 0.f;
	float m_vram_budget_gb = 0.f;

	u32 m_draw_calls = 0;
	u32 m_polys = 0;
};

extern CPerfOverlay g_PerfOverlay;
