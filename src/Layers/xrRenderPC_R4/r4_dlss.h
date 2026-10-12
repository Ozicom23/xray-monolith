#pragma once

#include "r4_upscaler.h"

// NVIDIA DLSS Super Resolution through the NGX SDK (D3D11).
// When the SDK is not available at build time (XR_DLSS_SDK undefined) every call fails gracefully
// and the renderer keeps its native-resolution path.
class CDLSS : public CUpscaler
{
public:
	const char* Name() const override { return "DLSS"; }

	bool Init() override;
	void Shutdown() override;
	bool IsAvailable() const override { return m_available; }

	void GetRenderSize(u32 out_w, u32 out_h, u32 quality, u32& render_w, u32& render_h) override;

	bool CreateFeature(u32 render_w, u32 render_h, u32 out_w, u32 out_h, u32 quality) override;
	void ReleaseFeature() override;

	bool Evaluate(const UpscalerEvalParams& p) override;

private:
	bool m_initialized = false;
	bool m_available = false;
	void* m_params = nullptr; // NVSDK_NGX_Parameter*
	void* m_handle = nullptr; // NVSDK_NGX_Handle*
	u32 m_eval_failures = 0;
};

extern CDLSS g_DLSS;
