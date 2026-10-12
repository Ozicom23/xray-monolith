#include "stdafx.h"
#include "r4_upscaler.h"
#include "r4_dlss.h"
#include "r4_fsr3.h"

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

void CUpscaler::GetJitter(u32 index, u32 phase_count, float& x, float& y)
{
	const u32 i = (index % phase_count) + 1;
	x = halton(i, 2) - 0.5f;
	y = halton(i, 3) - 0.5f;
}

u32 CUpscaler::GetJitterPhaseCount(u32 render_w, u32 out_w)
{
	// At least 8 phases per output pixel covered by a render pixel (NVIDIA and AMD both recommend this)
	const float ratio = float(out_w) / float(_max(render_w, 1u));
	return _max(8u, u32(iCeil(8.f * ratio * ratio)));
}

CUpscaler* GetUpscaler(u32 type)
{
	switch (type)
	{
	case upscaler_dlss: return &g_DLSS;
	case upscaler_fsr: return &g_FSR3;
	default: return nullptr;
	}
}
