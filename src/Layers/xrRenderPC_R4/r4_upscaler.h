#pragma once

// Temporal upscalers (DLSS, FSR) behind one interface. The render target renders the scene at
// GetRenderSize() and calls Evaluate() once per frame before the final post-process pass.

enum EUpscalerType
{
	upscaler_off = 0,
	upscaler_dlss,
	upscaler_fsr,
};

enum EUpscalerQuality
{
	upscaler_quality_ultra_performance = 0,
	upscaler_quality_performance,
	upscaler_quality_balanced,
	upscaler_quality_quality,
	upscaler_quality_native, // DLAA / FSR Native AA
};

struct UpscalerEvalParams
{
	ID3D11Resource* color;  // render resolution, LDR
	ID3D11Resource* output; // output resolution, UAV
	ID3D11Resource* depth;  // render resolution, regular (non-inverted) depth
	ID3D11Resource* motion; // render resolution, motion vectors (scaled by mv_scale to previous - current in pixels)
	ID3D11Resource* reactive; // render resolution R8, or null: 0 = blend with history as usual, 1 = current frame only
	float jitter_x;         // render pixels, +x right, +y down
	float jitter_y;
	float mv_scale_x;
	float mv_scale_y;
	u32 render_w;
	u32 render_h;
	u32 output_w;
	u32 output_h;
	bool reset;
	float frame_time_ms;
	float camera_near;
	float camera_far;
	float camera_fov_y; // radians
	float sharpness;    // 0 = off
};

class CUpscaler
{
public:
	virtual ~CUpscaler() {}

	virtual const char* Name() const = 0;

	// Checks support on the current device; cheap to call again
	virtual bool Init() = 0;
	virtual void Shutdown() = 0;
	virtual bool IsAvailable() const = 0;

	// Render resolution for the output resolution and quality mode
	virtual void GetRenderSize(u32 out_w, u32 out_h, u32 quality, u32& render_w, u32& render_h) = 0;

	virtual bool CreateFeature(u32 render_w, u32 render_h, u32 out_w, u32 out_h, u32 quality) = 0;
	virtual void ReleaseFeature() = 0;

	virtual bool Evaluate(const UpscalerEvalParams& p) = 0;

	// Halton(2,3) sub-pixel jitter in [-0.5, 0.5] render pixels
	static void GetJitter(u32 index, u32 phase_count, float& x, float& y);
	static u32 GetJitterPhaseCount(u32 render_w, u32 out_w);
};

// Upscaler implementation for EUpscalerType, nullptr for upscaler_off
CUpscaler* GetUpscaler(u32 type);
