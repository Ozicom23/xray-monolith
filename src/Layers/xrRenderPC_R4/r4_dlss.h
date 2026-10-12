#pragma once

// NVIDIA DLSS Super Resolution through the NGX SDK (D3D11).
// When the SDK is not available at build time (XR_DLSS_SDK undefined) every call fails gracefully
// and the renderer keeps its native-resolution path.
class CDLSS
{
public:
	enum EQuality
	{
		qUltraPerformance = 0,
		qPerformance,
		qBalanced,
		qQuality,
		qDLAA,
	};

	struct EvalParams
	{
		ID3D11Resource* color;
		ID3D11Resource* output;
		ID3D11Resource* depth;
		ID3D11Resource* motion;
		float jitter_x; // render pixels
		float jitter_y;
		float mv_scale_x;
		float mv_scale_y;
		u32 render_w;
		u32 render_h;
		bool reset;
		float frame_time_ms;
	};

	// Initializes NGX on the current device and checks DLSS SR availability
	bool Init();
	void Shutdown();
	bool IsAvailable() const { return m_available; }

	// Render resolution recommended for the output resolution and quality mode
	void GetRenderSize(u32 out_w, u32 out_h, u32 quality, u32& render_w, u32& render_h);

	bool CreateFeature(u32 render_w, u32 render_h, u32 out_w, u32 out_h, u32 quality);
	void ReleaseFeature();
	bool HasFeature() const { return m_handle != nullptr; }

	bool Evaluate(const EvalParams& p);

	// Halton(2,3) sub-pixel jitter in [-0.5, 0.5] render pixels
	static void GetJitter(u32 index, u32 phase_count, float& x, float& y);
	static u32 GetJitterPhaseCount(u32 render_w, u32 out_w);

private:
	bool m_initialized = false;
	bool m_available = false;
	void* m_params = nullptr; // NVSDK_NGX_Parameter*
	void* m_handle = nullptr; // NVSDK_NGX_Handle*
	u32 m_eval_failures = 0;
};

extern CDLSS g_DLSS;
