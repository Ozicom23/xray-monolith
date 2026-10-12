#include "stdafx.h"
#include "r4_dlss.h"

CDLSS g_DLSS;

// Fallback render scale per quality mode, used when NGX cannot report the optimal settings
static const float s_dlss_scale[] = { 1.f / 3.f, 0.5f, 0.58f, 2.f / 3.f, 1.f };

static float halton(u32 index, u32 base)
{
	float f = 1.f, r = 0.f;
	while (index > 0)
	{
		f /= float(base);
		r += f * float(index % base);
		index /= base;
	}
	return r;
}

void CDLSS::GetJitter(u32 index, u32 phase_count, float& x, float& y)
{
	const u32 i = (index % phase_count) + 1;
	x = halton(i, 2) - 0.5f;
	y = halton(i, 3) - 0.5f;
}

u32 CDLSS::GetJitterPhaseCount(u32 render_w, u32 out_w)
{
	// NVIDIA recommends at least 8 phases per output pixel covered by a render pixel
	const float ratio = float(out_w) / float(_max(render_w, 1u));
	return _max(8u, u32(iCeil(8.f * ratio * ratio)));
}

#ifdef XR_DLSS_SDK

#include <nvsdk_ngx_helpers.h>

#pragma comment(lib, "nvsdk_ngx_d.lib")

// Any unique id works for custom engines
static const char* s_ngx_project_id = "7c1d5d0e-3b4a-4f0c-9d55-6a1e2f4b8c31";

static NVSDK_NGX_PerfQuality_Value to_ngx_quality(u32 quality)
{
	switch (quality)
	{
	case CDLSS::qUltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
	case CDLSS::qPerformance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
	case CDLSS::qBalanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
	case CDLSS::qDLAA: return NVSDK_NGX_PerfQuality_Value_DLAA;
	default: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
	}
}

static void NVSDK_CONV ngx_log_callback(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
	// NGX messages already end with a newline
	string1024 line;
	xr_strcpy(line, message ? message : "");
	const size_t len = xr_strlen(line);
	if (len && line[len - 1] == '\n')
		line[len - 1] = 0;
	Msg("[NGX] %s", line);
}

bool CDLSS::Init()
{
	if (m_initialized)
		return m_available;
	m_initialized = true;
	m_available = false;

	// Feature DLLs (nvngx_dlss.dll) are searched next to the executable
	wchar_t exe_dir[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
	if (wchar_t* slash = wcsrchr(exe_dir, L'\\'))
		*slash = 0;
	const wchar_t* search_paths[] = { exe_dir };

	string_path log_dir;
	FS.update_path(log_dir, "$logs$", "");
	wchar_t log_dir_w[MAX_PATH] = {};
	MultiByteToWideChar(CP_ACP, 0, log_dir, -1, log_dir_w, MAX_PATH);

	NVSDK_NGX_FeatureCommonInfo info = {};
	info.PathListInfo.Path = search_paths;
	info.PathListInfo.Length = 1;
	if (ps_r4_dlss_log)
	{
		info.LoggingInfo.LoggingCallback = ngx_log_callback;
		info.LoggingInfo.MinimumLoggingLevel = ps_r4_dlss_log > 1 ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE : NVSDK_NGX_LOGGING_LEVEL_ON;
	}

	NVSDK_NGX_Result res = NVSDK_NGX_D3D11_Init_with_ProjectID(s_ngx_project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
	                                                            log_dir_w, HW.pDevice, &info);
	if (NVSDK_NGX_FAILED(res))
	{
		Msg("! DLSS: NGX init failed (0x%08x). NVIDIA GPU/driver required (under Wine/Proton: dxvk-nvapi)", u32(res));
		return false;
	}

	NVSDK_NGX_Parameter* params = nullptr;
	res = NVSDK_NGX_D3D11_GetCapabilityParameters(&params);
	if (NVSDK_NGX_FAILED(res) || !params)
	{
		Msg("! DLSS: NGX capability query failed (0x%08x)", u32(res));
		NVSDK_NGX_D3D11_Shutdown1(HW.pDevice);
		return false;
	}
	m_params = params;

	int needs_driver = 0;
	unsigned int min_driver_major = 0, min_driver_minor = 0;
	NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
	NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &min_driver_major);
	NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &min_driver_minor);
	if (needs_driver)
		Msg("! DLSS: driver update required (minimum %u.%u)", min_driver_major, min_driver_minor);

	int available = 0;
	res = NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
	if (NVSDK_NGX_FAILED(res) || !available)
	{
		int init_result = 0;
		NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_result);
		Msg("! DLSS: Super Resolution not available (init result 0x%08x). Is nvngx_dlss.dll next to the exe?", u32(init_result));
		Shutdown();
		return false;
	}

	Msg("* DLSS: NGX initialized, Super Resolution available");
	m_available = true;
	return true;
}

void CDLSS::Shutdown()
{
	ReleaseFeature();
	if (m_params)
	{
		NVSDK_NGX_D3D11_DestroyParameters((NVSDK_NGX_Parameter*)m_params);
		m_params = nullptr;
	}
	if (m_initialized)
		NVSDK_NGX_D3D11_Shutdown1(HW.pDevice);
	m_initialized = false;
	m_available = false;
}

void CDLSS::GetRenderSize(u32 out_w, u32 out_h, u32 quality, u32& render_w, u32& render_h)
{
	quality = _min(quality, u32(qDLAA));
	render_w = u32(iFloor(float(out_w) * s_dlss_scale[quality] + 0.5f));
	render_h = u32(iFloor(float(out_h) * s_dlss_scale[quality] + 0.5f));

	if (m_available && m_params && quality != qDLAA)
	{
		unsigned int opt_w = 0, opt_h = 0, max_w = 0, max_h = 0, min_w = 0, min_h = 0;
		float sharpness = 0.f;
		NVSDK_NGX_Result res = NGX_DLSS_GET_OPTIMAL_SETTINGS((NVSDK_NGX_Parameter*)m_params, out_w, out_h,
		                                                     to_ngx_quality(quality), &opt_w, &opt_h, &max_w, &max_h,
		                                                     &min_w, &min_h, &sharpness);
		if (NVSDK_NGX_SUCCEED(res) && opt_w && opt_h)
		{
			render_w = opt_w;
			render_h = opt_h;
		}
	}

	render_w = _max(render_w, 1u);
	render_h = _max(render_h, 1u);
}

bool CDLSS::CreateFeature(u32 render_w, u32 render_h, u32 out_w, u32 out_h, u32 quality)
{
	ReleaseFeature();
	if (!m_available)
		return false;

	NVSDK_NGX_DLSS_Create_Params create = {};
	create.Feature.InWidth = render_w;
	create.Feature.InHeight = render_h;
	create.Feature.InTargetWidth = out_w;
	create.Feature.InTargetHeight = out_h;
	create.Feature.InPerfQualityValue = to_ngx_quality(quality);
	// Input is the tonemapped LDR scene; motion vectors come from SSS at render resolution, without jitter
	create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;

	NVSDK_NGX_Handle* handle = nullptr;
	NVSDK_NGX_Result res = NGX_D3D11_CREATE_DLSS_EXT(HW.pContext, &handle, (NVSDK_NGX_Parameter*)m_params, &create);
	if (NVSDK_NGX_FAILED(res) || !handle)
	{
		Msg("! DLSS: feature creation failed (0x%08x) for %ux%u -> %ux%u", u32(res), render_w, render_h, out_w, out_h);
		return false;
	}

	m_handle = handle;
	m_eval_failures = 0;
	Msg("* DLSS: feature created, %ux%u -> %ux%u, quality %u", render_w, render_h, out_w, out_h, quality);
	return true;
}

void CDLSS::ReleaseFeature()
{
	if (m_handle)
	{
		NVSDK_NGX_D3D11_ReleaseFeature((NVSDK_NGX_Handle*)m_handle);
		m_handle = nullptr;
	}
}

bool CDLSS::Evaluate(const EvalParams& p)
{
	if (!m_handle)
		return false;

	NVSDK_NGX_D3D11_DLSS_Eval_Params eval = {};
	eval.Feature.pInColor = p.color;
	eval.Feature.pInOutput = p.output;
	eval.pInDepth = p.depth;
	eval.pInMotionVectors = p.motion;
	eval.InJitterOffsetX = p.jitter_x;
	eval.InJitterOffsetY = p.jitter_y;
	eval.InMVScaleX = p.mv_scale_x;
	eval.InMVScaleY = p.mv_scale_y;
	eval.InRenderSubrectDimensions.Width = p.render_w;
	eval.InRenderSubrectDimensions.Height = p.render_h;
	eval.InReset = p.reset ? 1 : 0;
	eval.InFrameTimeDeltaInMsec = p.frame_time_ms;

	NVSDK_NGX_Result res = NGX_D3D11_EVALUATE_DLSS_EXT(HW.pContext, (NVSDK_NGX_Handle*)m_handle,
	                                                   (NVSDK_NGX_Parameter*)m_params, &eval);
	if (NVSDK_NGX_FAILED(res))
	{
		// Don't flood the log if every frame fails
		if (m_eval_failures++ < 8)
			Msg("! DLSS: evaluate failed (0x%08x)", u32(res));
		return false;
	}
	return true;
}

#else // XR_DLSS_SDK

bool CDLSS::Init()
{
	Msg("! DLSS: this build was compiled without the NVIDIA NGX SDK");
	m_initialized = true;
	m_available = false;
	return false;
}

void CDLSS::Shutdown()
{
	m_initialized = false;
	m_available = false;
}

void CDLSS::GetRenderSize(u32 out_w, u32 out_h, u32, u32& render_w, u32& render_h)
{
	render_w = out_w;
	render_h = out_h;
}

bool CDLSS::CreateFeature(u32, u32, u32, u32, u32) { return false; }
void CDLSS::ReleaseFeature() {}
bool CDLSS::Evaluate(const EvalParams&) { return false; }

#endif // XR_DLSS_SDK
