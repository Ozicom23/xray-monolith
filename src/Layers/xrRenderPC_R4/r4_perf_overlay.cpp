#include "stdafx.h"
#include "r4_perf_overlay.h"
#include "r4_upscaler.h"
#include "../../xrEngine/Stats.h"

#include <dxgi1_4.h>
#include <algorithm>

#include "imgui.h"
#include "imgui_internal.h"

CPerfOverlay g_PerfOverlay;

static const char* s_quality_names[] = { "Ultra Performance", "Performance", "Balanced", "Quality", "Native AA" };

bool CPerfOverlay::CreateQueries()
{
	if (m_queries[0].disjoint)
		return true;
	if (m_queries_failed)
		return false;

	D3D11_QUERY_DESC disjoint = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
	D3D11_QUERY_DESC stamp = { D3D11_QUERY_TIMESTAMP, 0 };
	for (u32 i = 0; i < kGpuQueries; ++i)
	{
		GpuQuery& q = m_queries[i];
		if (FAILED(HW.pDevice->CreateQuery(&disjoint, &q.disjoint)) ||
			FAILED(HW.pDevice->CreateQuery(&stamp, &q.begin)) ||
			FAILED(HW.pDevice->CreateQuery(&stamp, &q.end)))
		{
			Msg("! perf_overlay: GPU timestamp queries are not available");
			Destroy();
			m_queries_failed = true;
			return false;
		}
		q.pending = false;
	}
	m_query_write = 0;
	return true;
}

void CPerfOverlay::CollectGpuTimes()
{
	// Oldest first; results arrive a few frames later, never wait for them
	for (u32 n = 0; n < kGpuQueries; ++n)
	{
		GpuQuery& q = m_queries[(m_query_write + n) % kGpuQueries];
		if (!q.pending)
			continue;

		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
		if (HW.pContext->GetData(q.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
			break;

		UINT64 t0 = 0, t1 = 0;
		if (HW.pContext->GetData(q.begin, &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
			HW.pContext->GetData(q.end, &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
			break;

		q.pending = false;
		if (!dj.Disjoint && dj.Frequency && t1 > t0)
		{
			const float ms = float(double(t1 - t0) * 1000.0 / double(dj.Frequency));
			m_gpu_ms = m_gpu_ms > 0.f ? m_gpu_ms * 0.9f + ms * 0.1f : ms;
		}
	}
}

void CPerfOverlay::GpuFrameBegin()
{
	m_query_active = false;
	if (!ps_perf_overlay || !CreateQueries())
		return;

	CollectGpuTimes();

	GpuQuery& q = m_queries[m_query_write];
	if (q.pending)
		return; // results not back yet, skip timing this frame

	HW.pContext->Begin(q.disjoint);
	HW.pContext->End(q.begin);
	m_query_active = true;
}

void CPerfOverlay::GpuFrameEnd()
{
	if (!ps_perf_overlay)
		return;

	m_draw_calls = RCache.stat.calls;
	m_polys = RCache.stat.polys;

	if (!m_query_active)
		return;

	GpuQuery& q = m_queries[m_query_write];
	HW.pContext->End(q.end);
	HW.pContext->End(q.disjoint);
	q.pending = true;
	m_query_write = (m_query_write + 1) % kGpuQueries;
	m_query_active = false;
}

void CPerfOverlay::PresentBegin()
{
	if (ps_perf_overlay)
		m_present_timer.Start();
}

void CPerfOverlay::PresentEnd()
{
	if (!ps_perf_overlay)
		return;
	const float ms = m_present_timer.GetElapsed_sec() * 1000.f;
	m_present_ms = m_present_ms > 0.f ? m_present_ms * 0.9f + ms * 0.1f : ms;
}

void CPerfOverlay::UpdateVram()
{
	if (m_vram_failed)
		return;
	if (!m_adapter3 && FAILED(HW.m_pAdapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&m_adapter3)))
	{
		m_adapter3 = nullptr;
		m_vram_failed = true;
		return;
	}

	if (m_vram_total_gb <= 0.f)
	{
		DXGI_ADAPTER_DESC desc = {};
		if (SUCCEEDED(m_adapter3->GetDesc(&desc)))
			m_vram_total_gb = float(double(desc.DedicatedVideoMemory) / (1024.0 * 1024.0 * 1024.0));
	}

	DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
	if (SUCCEEDED(m_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
	{
		m_vram_used_gb = float(double(info.CurrentUsage) / (1024.0 * 1024.0 * 1024.0));
		m_vram_budget_gb = float(double(info.Budget) / (1024.0 * 1024.0 * 1024.0));
	}
}

void CPerfOverlay::Destroy()
{
	for (u32 i = 0; i < kGpuQueries; ++i)
	{
		_RELEASE(m_queries[i].disjoint);
		_RELEASE(m_queries[i].begin);
		_RELEASE(m_queries[i].end);
		m_queries[i].pending = false;
	}
	m_query_active = false;
	_RELEASE(m_adapter3);
}

void CPerfOverlay::Draw()
{
	// The engine frame timers are collected while the overlay is on (normally only with rs_stats)
	g_stats_gather_timers = ps_perf_overlay != 0;

	// Wall-clock frame time, measured here once per frame
	const float frame_ms = m_frame_timer_started ? m_frame_timer.GetElapsed_sec() * 1000.f : 0.f;
	m_frame_timer.Start();
	m_frame_timer_started = true;

	if (!ps_perf_overlay || frame_ms <= 0.f)
		return;

	m_frame_ms[m_frame_pos] = frame_ms;
	m_frame_pos = (m_frame_pos + 1) % kFrameHistory;
	m_frame_count = _min(m_frame_count + 1, kFrameHistory);
	m_graph[m_graph_pos] = frame_ms;
	m_graph_pos = (m_graph_pos + 1) % kGraphSamples;

	// Refresh the numbers 4 times per second
	m_refresh_ms += frame_ms;
	if (m_refresh_ms >= 250.f || m_show_fps <= 0.f)
	{
		m_refresh_ms = 0.f;

		// Walk back from the newest frame: FPS over the last second, 1% low over the last 5 seconds,
		// so the numbers follow changes quickly (e.g. leaving a fast menu) instead of a fixed frame count
		float avg_sum = 0.f, low_time = 0.f;
		u32 avg_n = 0, low_n = 0;
		float recent[kFrameHistory];
		for (u32 i = 0; i < m_frame_count; ++i)
		{
			const float ms = m_frame_ms[(m_frame_pos + kFrameHistory - 1 - i) % kFrameHistory];
			if (avg_sum < 1000.f)
			{
				avg_sum += ms;
				++avg_n;
			}
			if (low_time >= 5000.f)
				break;
			low_time += ms;
			recent[low_n++] = ms;
		}
		const float avg_ms = avg_n ? avg_sum / float(avg_n) : frame_ms;
		m_show_frame_ms = avg_ms;
		m_show_fps = avg_ms > 0.f ? 1000.f / avg_ms : 0.f;

		// 1% low: the frame time 99% of recent frames are faster than
		if (low_n >= 10)
		{
			const u32 idx = u32(float(low_n - 1) * 0.99f);
			std::nth_element(recent, recent + idx, recent + low_n);
			m_show_low_fps = recent[idx] > 0.f ? 1000.f / recent[idx] : 0.f;
		}
		UpdateVram();
	}

	// Only inside an open ImGui frame (not before the engine's first ImGui::NewFrame)
	const ImGuiContext* imgui = ImGui::GetCurrentContext();
	if (!imgui || !imgui->WithinFrameScope)
		return;

	const ImGuiIO& io = ImGui::GetIO();
	const u32 corner = ps_perf_overlay_pos & 3;
	const float pad = 10.f;
	ImGui::SetNextWindowPos(ImVec2((corner & 1) ? io.DisplaySize.x - pad : pad, (corner & 2) ? io.DisplaySize.y - pad : pad),
	                        ImGuiCond_Always, ImVec2((corner & 1) ? 1.f : 0.f, (corner & 2) ? 1.f : 0.f));
	ImGui::SetNextWindowBgAlpha(0.6f);

	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
		ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;

	if (ImGui::Begin("##perf_overlay", nullptr, flags))
	{
		ImGui::SetWindowFontScale(_max(1.f, io.DisplaySize.y / 1080.f));

		ImGui::Text("%4.0f FPS   %5.2f ms   1%% low %4.0f", m_show_fps, m_show_frame_ms, m_show_low_fps);

		const CStats* st = Device.Statistic;
		// Engine update + render submission; RenderTOTAL includes Present, which waits for vsync/the GPU
		const float cpu_ms = st ? _max(0.f, st->EngineTOTAL.result + st->RenderTOTAL.result - m_present_ms) : 0.f;
		if (m_gpu_ms > 0.f)
		{
			const bool gpu_bound = m_gpu_ms >= m_show_frame_ms * 0.9f;
			ImGui::Text("GPU %5.2f ms   CPU %5.2f ms", m_gpu_ms, cpu_ms);
			ImGui::SameLine();
			if (gpu_bound)
				ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "  GPU-bound");
			else
				ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "  CPU-bound / capped");
		}
		else
			ImGui::Text("GPU   n/a     CPU %5.2f ms", cpu_ms);

		// Budget is what the OS/driver lets the game use before evicting, below the card's total
		if (m_vram_budget_gb > 0.f && m_vram_total_gb > 0.f)
			ImGui::Text("VRAM %.2f GB used, budget %.1f / %.1f GB", m_vram_used_gb, m_vram_budget_gb, m_vram_total_gb);
		else if (m_vram_budget_gb > 0.f)
			ImGui::Text("VRAM %.2f GB used, budget %.1f GB", m_vram_used_gb, m_vram_budget_gb);

		CRenderTarget* target = RImplementation.Target;
		CUpscaler* upscaler = GetUpscaler(RImplementation.o.upscaler);
		if (target && upscaler && target->upscale_enabled())
		{
			const u32 q = _min(ps_r4_upscaler_quality, u32(upscaler_quality_native));
			ImGui::Text("%s %s   %ux%u -> %ux%u", upscaler->Name(), s_quality_names[q],
			            target->upscale_render_width(), target->upscale_render_height(), Device.dwWidth, Device.dwHeight);
		}
		else
			ImGui::Text("Native   %ux%u", Device.dwWidth, Device.dwHeight);

		if (ps_perf_overlay >= 2)
		{
			ImGui::Separator();
			if (st)
			{
				ImGui::Text("Update %5.2f   Scheduler %5.2f   Objects %5.2f", st->EngineTOTAL.result, st->Sheduler.result, st->UpdateClient.result);
				ImGui::Text("Physics %5.2f   AI %5.2f   Animation %5.2f", st->Physics.result, st->AI_Think.result, st->Animation.result);
				ImGui::Text("Render %5.2f   Calc %5.2f   HOM %5.2f", st->RenderTOTAL.result, st->RenderCALC.result, st->RenderCALC_HOM.result);
			}
			ImGui::Text("Draw calls %u   Triangles %.2f M", m_draw_calls, float(m_polys) / 1000000.f);

			float graph_max = 0.f;
			for (u32 i = 0; i < kGraphSamples; ++i)
				graph_max = _max(graph_max, m_graph[i]);
			graph_max = _max(graph_max * 1.1f, 16.7f);

			string64 label;
			xr_sprintf(label, "frame time (max %.1f ms)", graph_max);
			ImGui::PlotLines("##frametime", m_graph, int(kGraphSamples), int(m_graph_pos), label, 0.f, graph_max,
			                 ImVec2(320.f * _max(1.f, io.DisplaySize.y / 1080.f), 60.f * _max(1.f, io.DisplaySize.y / 1080.f)));
		}
	}
	ImGui::End();
}
