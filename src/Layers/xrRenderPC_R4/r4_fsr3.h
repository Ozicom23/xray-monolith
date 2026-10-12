#pragma once

#include "r4_upscaler.h"

// AMD FidelityFX Super Resolution 3.1 upscaler on D3D11.
// AMD ships DX12/Vulkan backends only; r4_fsr3.cpp implements the FidelityFX backend interface on D3D11
// with shaders precompiled for cs_5_0 (src/3rd party/FidelityFX-FSR3/compile_shaders.ps1).
// Without the compiled shaders (XR_FSR3_SHADERS undefined) every call fails gracefully.
class CFSR3 : public CUpscaler
{
public:
	const char* Name() const override { return "FSR 3.1"; }

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
	void* m_context = nullptr; // FfxFsr3UpscalerContext*
	void* m_backend = nullptr; // backend state, owned by the FidelityFX interface
	ID3D11Texture2D* m_shared[3] = {}; // dilated depth, dilated motion vectors, reconstructed previous nearest depth
	u32 m_eval_failures = 0;
};

extern CFSR3 g_FSR3;
