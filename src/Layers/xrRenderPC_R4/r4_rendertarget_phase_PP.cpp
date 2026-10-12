#include "stdafx.h"
#include "r4_rendertarget.h"
#include "r4_dlss.h"

void CRenderTarget::u_calc_tc_noise(Fvector2& p0, Fvector2& p1)
{
	CTexture* T = RCache.get_ActiveTexture(2);
	VERIFY2(T, "Texture #3 in noise shader should be setted up");
	u32 tw = iCeil(float(T->get_Width()) * param_noise_scale + EPS_S);
	u32 th = iCeil(float(T->get_Height()) * param_noise_scale + EPS_S);
	VERIFY2(tw && th, "Noise scale can't be zero in any way");

	// calculate shift from FPSes
	im_noise_time -= Device.fTimeDelta;
	if (im_noise_time < 0)
	{
		im_noise_shift_w = ::Random.randI(tw ? tw : 1);
		im_noise_shift_h = ::Random.randI(th ? th : 1);
		float fps_time = 1 / param_noise_fps;
		while (im_noise_time < 0) im_noise_time += fps_time;
	}

	u32 shift_w = im_noise_shift_w;
	u32 shift_h = im_noise_shift_h;
	float start_u = (float(shift_w) + .5f) / (tw);
	float start_v = (float(shift_h) + .5f) / (th);
	u32 _w = Device.dwWidth;
	u32 _h = Device.dwHeight;
	u32 cnt_w = _w / tw;
	u32 cnt_h = _h / th;
	float end_u = start_u + float(cnt_w) + 1;
	float end_v = start_v + float(cnt_h) + 1;

	p0.set(start_u, start_v);
	p1.set(end_u, end_v);
}

void CRenderTarget::u_calc_tc_duality_ss(Fvector2& r0, Fvector2& r1, Fvector2& l0, Fvector2& l1)
{
	// Calculate ordinaty TCs from blur and SS
	float tw = float(dwWidth);
	float th = float(dwHeight);
	if (dwHeight != Device.dwHeight) param_blur = 1.f;
	Fvector2 shift, p0, p1;
	shift.set(.5f / tw, .5f / th);
	shift.mul(param_blur);
	p0.set(.5f / tw, .5f / th).add(shift);
	p1.set((tw + .5f) / tw, (th + .5f) / th).add(shift);

	// Calculate Duality TC
	float shift_u = param_duality_h * .5f;
	float shift_v = param_duality_v * .5f;

	r0.set(p0.x, p0.y);
	r1.set(p1.x - shift_u, p1.y - shift_v);
	l0.set(p0.x + shift_u, p0.y + shift_v);
	l1.set(p1.x, p1.y);
}

BOOL CRenderTarget::u_need_PP()
{
	bool _blur = (param_blur > 0.001f);
	bool _gray = (param_gray > 0.001f);
	bool _noise = (param_noise > 0.001f);
	bool _dual = (param_duality_h > 0.001f) || (param_duality_v > 0.001f);

	// bool	_menu_pp= g_pGamePersistent?g_pGamePersistent->OnRenderPPUI_query():false;

	bool _cbase = false;
	{
		int _r = color_get_R(param_color_base);
		_r = _abs(_r - int(0x7f));
		int _g = color_get_G(param_color_base);
		_g = _abs(_g - int(0x7f));
		int _b = color_get_B(param_color_base);
		_b = _abs(_b - int(0x7f));
		if (_r > 2 || _g > 2 || _b > 2) _cbase = true;
	}
	bool _cadd = false;
	{
		//int		_r	= color_get_R(param_color_add)	;
		//int		_g	= color_get_G(param_color_add)	;
		//int		_b	= color_get_B(param_color_add)	;
		//if (_r>2 || _g>2 || _b>2)	_cadd	= true	;
		int _r = _abs((int)(param_color_add.x * 255));
		int _g = _abs((int)(param_color_add.y * 255));
		int _b = _abs((int)(param_color_add.z * 255));
		if (_r > 2 || _g > 2 || _b > 2) _cadd = true;
	}
	return _blur || _gray || _noise || _dual || _cbase || _cadd || u_need_CM();
}

bool CRenderTarget::u_need_CM()
{
	return (param_color_map_influence > 0.001f);
}

struct TL_2c3uv
{
	Fvector4 p;
	u32 color0;
	u32 color1;
	Fvector2 uv [3];
	IC void set(float x, float y, u32 c0, u32 c1, float u0, float v0, float u1, float v1, float u2, float v2)
	{
		p.set(x, y, EPS_S, 1.f);
		color0 = c0;
		color1 = c1;
		uv[0].set(u0, v0);
		uv[1].set(u1, v1);
		uv[2].set(u2, v2);
	}
};

void CRenderTarget::phase_pp()
{
	// combination/postprocess
	u_setrt(Device.dwWidth, Device.dwHeight, HW.pBaseRT,NULL,NULL, HW.pBaseZB);
	//	Element 0 for for normal post-process
	//	Element 4 for color map post-process
	bool bCMap = u_need_CM();
	//RCache.set_Element	(s_postprocess->E[bCMap ? 4 : 0]);
	if (m_dlss_upscaled)
	{
		RCache.set_Element(s_postprocess_dlss->E[bCMap ? 4 : 0]);
	}
	else if (!RImplementation.o.dx10_msaa)
	{
		//		RCache.set_Shader	(s_postprocess	);
		RCache.set_Element(s_postprocess->E[bCMap ? 4 : 0]);
	}
	else
	{
		//		RCache.set_Shader( s_postprocess_msaa );
		RCache.set_Element(s_postprocess_msaa->E[bCMap ? 4 : 0]);
	}

	int gblend = clampr(iFloor((1 - param_gray) * 255.f), 0, 255);
	int nblend = clampr(iFloor((1 - param_noise) * 255.f), 0, 255);
	u32 p_color = subst_alpha(param_color_base, nblend);
	u32 p_gray = subst_alpha(param_color_gray, gblend);
	Fvector p_brightness = param_color_add;
	// Msg				("param_gray:%f(%d),param_noise:%f(%d)",param_gray,gblend,param_noise,nblend);
	// Msg				("base: %d,%d,%d",	color_get_R(p_color),		color_get_G(p_color),		color_get_B(p_color));
	// Msg				("gray: %d,%d,%d",	color_get_R(p_gray),		color_get_G(p_gray),		color_get_B(p_gray));
	// Msg				("add:  %d,%d,%d",	color_get_R(p_brightness),	color_get_G(p_brightness),	color_get_B(p_brightness));

	// Draw full-screen quad textured with our scene image
	u32 Offset;
	float _w = float(Device.dwWidth);
	float _h = float(Device.dwHeight);

	Fvector2 n0, n1, r0, r1, l0, l1;
	u_calc_tc_duality_ss(r0, r1, l0, l1);
	u_calc_tc_noise(n0, n1);

	// Fill vertex buffer
	float du = ps_r1_pps_u, dv = ps_r1_pps_v;
	TL_2c3uv* pv = (TL_2c3uv*)RCache.Vertex.Lock(4, g_postprocess.stride(), Offset);
	pv->set(du + 0, dv + float(_h), p_color, p_gray, r0.x, r1.y, l0.x, l1.y, n0.x, n1.y);
	pv++;
	pv->set(du + 0, dv + 0, p_color, p_gray, r0.x, r0.y, l0.x, l0.y, n0.x, n0.y);
	pv++;
	pv->set(du + float(_w), dv + float(_h), p_color, p_gray, r1.x, r1.y, l1.x, l1.y, n1.x, n1.y);
	pv++;
	pv->set(du + float(_w), dv + 0, p_color, p_gray, r1.x, r0.y, l1.x, l0.y, n1.x, n0.y);
	pv++;
	RCache.Vertex.Unlock(4, g_postprocess.stride());

	// Actual rendering
	static shared_str s_brightness = "c_brightness";
	static shared_str s_colormap = "c_colormap";
	RCache.set_c(s_brightness, p_brightness.x, p_brightness.y, p_brightness.z, 0);
	RCache.set_c(s_colormap, param_color_map_influence, param_color_map_interpolate, 0, 0);
	RCache.set_Geometry(g_postprocess);
	RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
}

void CRenderTarget::dlss_begin_scene()
{
	m_dlss_upscaled = false;
	if (!m_dlss_enabled || m_dlss_in_scene)
		return;
	m_dlss_in_scene = true;

	Device.dwWidth = m_dlss_render_w;
	Device.dwHeight = m_dlss_render_h;
	m_dlss_saved_zb = HW.pBaseZB;
	HW.pBaseZB = rt_dlss_depth->pZRT;

	// Second viewport (3D scope) frames are not fed to DLSS, render them without jitter
	if (Device.m_SecondViewport.IsSVPFrame())
	{
		m_dlss_jitter.set(0.f, 0.f);
		RImplementation.dlss_jitter_on = false;
		RImplementation.dlss_jitter_ndc.set(0.f, 0.f);
	}
	else
	{
		float jx, jy;
		CDLSS::GetJitter(m_dlss_jitter_index, m_dlss_jitter_phases, jx, jy);
		m_dlss_jitter.set(jx, jy);
		// Pixel offset to clip space, pixel Y goes down
		RImplementation.dlss_jitter_on = true;
		RImplementation.dlss_jitter_ndc.set(2.f * jx / float(m_dlss_render_w), -2.f * jy / float(m_dlss_render_h));
	}

	// Viewport follows the last bound target, start the frame at render resolution
	u_setrt(rt_Generic_0, NULL, NULL, HW.pBaseZB);
	RImplementation.rmNormal();
}

void CRenderTarget::dlss_end_scene()
{
	RImplementation.dlss_jitter_on = false;
	if (!m_dlss_in_scene)
		return;
	m_dlss_in_scene = false;

	Device.dwWidth = m_dlss_output_w;
	Device.dwHeight = m_dlss_output_h;
	HW.pBaseZB = m_dlss_saved_zb;
	m_dlss_saved_zb = nullptr;

	u_setrt(Device.dwWidth, Device.dwHeight, HW.pBaseRT, NULL, NULL, HW.pBaseZB);
	RImplementation.rmNormal();
}

void CRenderTarget::phase_dlss()
{
	if (!m_dlss_in_scene)
		return;

	if (!Device.m_SecondViewport.IsSVPFrame())
	{
		PIX_EVENT(phase_dlss);

		// History is useless after a pause (menus, loading) or a camera cut
		if (Device.dwFrame - m_dlss_last_frame > 4 || Device.vCameraPosition.distance_to(m_dlss_last_cam) > 10.f)
			m_dlss_reset = true;

		CDLSS::EvalParams p = {};
		p.color = rt_Color->pSurface;
		p.output = rt_dlss_output->pSurface;
		p.depth = rt_dlss_depth->pSurface;
		p.motion = rt_ssfx_motion_vectors->pSurface;
		p.jitter_x = m_dlss_jitter.x * ps_r4_dlss_signs.z;
		p.jitter_y = m_dlss_jitter.y * ps_r4_dlss_signs.w;
		// SSS motion vectors are (current - previous) in UV units, DLSS wants (previous - current) in pixels
		p.mv_scale_x = -float(m_dlss_render_w) * ps_r4_dlss_signs.x;
		p.mv_scale_y = -float(m_dlss_render_h) * ps_r4_dlss_signs.y;
		p.render_w = m_dlss_render_w;
		p.render_h = m_dlss_render_h;
		p.reset = m_dlss_reset;
		p.frame_time_ms = Device.fTimeDelta * 1000.f;

		// NGX binds its own state, start and leave with a clean context
		HW.pContext->ClearState();
		RCache.Invalidate();
		m_dlss_upscaled = g_DLSS.Evaluate(p);
		HW.pContext->ClearState();
		RCache.Invalidate();

		if (m_dlss_upscaled)
		{
			m_dlss_reset = false;
			m_dlss_jitter_index++;
			m_dlss_last_frame = Device.dwFrame;
			m_dlss_last_cam.set(Device.vCameraPosition);
		}
	}

	// Without an upscaled image phase_pp stretches rt_Color bilinearly
	dlss_end_scene();
}
