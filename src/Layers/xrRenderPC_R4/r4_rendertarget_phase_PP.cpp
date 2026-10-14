#include "stdafx.h"
#include "r4_rendertarget.h"
#include "r4_upscaler.h"

// The upscaler's mask (r4_upscaler_mask): where it should trust the current frame over its history. 3D scope lenses
// (their reticle shader marks them with stencil 254) show the world through the scope, which moves unlike the lens;
// SSS marks foliage in the motion vectors' w (branches -1, grass > 0), whose wind isn't in the branches' motion
// vectors; first-person items are their z. Built by a small pass right before the upscaler runs.
namespace upscale_mask
{
	ID3D11Texture2D* s_tex = nullptr;
	ID3D11RenderTargetView* s_rtv = nullptr;
	ID3D11ShaderResourceView* s_mv_srv = nullptr;
	ID3D11ShaderResourceView* s_stencil_srv = nullptr;
	ID3D11Resource* s_mv_res = nullptr; // the views above are of these
	ID3D11Resource* s_depth_res = nullptr;
	ID3D11VertexShader* s_vs = nullptr;
	ID3D11PixelShader* s_ps = nullptr;
	ID3D11Buffer* s_cb = nullptr;
	ID3D11RasterizerState* s_rs = nullptr;
	ID3D11DepthStencilState* s_ds = nullptr;
	ID3D11BlendState* s_bs = nullptr;
	u32 s_w = 0, s_h = 0;
	bool s_failed = false;

	const char* kShader =
		"Texture2D<float4> s_mv : register(t0);\n"
		"Texture2D<uint2> s_stencil : register(t1);\n"
		"cbuffer params : register(b0) { float4 weight; };\n"
		"float4 vs(uint id : SV_VertexID) : SV_Position\n"
		"{\n"
		"	float2 uv = float2((id << 1) & 2, id & 2);\n"
		"	return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
		"}\n"
		"float ps(float4 pos : SV_Position) : SV_Target\n"
		"{\n"
		"	int3 p = int3(pos.xy, 0);\n"
		"	float4 mv = s_mv.Load(p);\n"
		"	float m = s_stencil.Load(p).g == 254 ? weight.x : 0;\n"
		"	if (mv.w != 0) m = max(m, weight.y);\n"
		"	if (mv.z > 0.5) m = max(m, weight.z);\n"
		"	return m;\n"
		"}\n";

	void release()
	{
		_RELEASE(s_rtv);
		_RELEASE(s_tex);
		_RELEASE(s_mv_srv);
		_RELEASE(s_stencil_srv);
		s_mv_res = s_depth_res = nullptr;
		_RELEASE(s_vs);
		_RELEASE(s_ps);
		_RELEASE(s_cb);
		_RELEASE(s_rs);
		_RELEASE(s_ds);
		_RELEASE(s_bs);
		s_w = s_h = 0;
		s_failed = false;
	}

	static bool create_shaders()
	{
		if (s_ps)
			return true;
		if (s_failed)
			return false;
		ID3DBlob* vs = nullptr;
		ID3DBlob* ps = nullptr;
		ID3DBlob* err = nullptr;
		HRESULT hr = D3DCompile(kShader, xr_strlen(kShader), "upscale_mask", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err);
		if (SUCCEEDED(hr))
			hr = D3DCompile(kShader, xr_strlen(kShader), "upscale_mask", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err);
		if (SUCCEEDED(hr))
			hr = HW.pDevice->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &s_vs);
		if (SUCCEEDED(hr))
			hr = HW.pDevice->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &s_ps);
		if (SUCCEEDED(hr))
		{
			D3D11_BUFFER_DESC bd = {};
			bd.ByteWidth = 16;
			bd.Usage = D3D11_USAGE_DYNAMIC;
			bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			hr = HW.pDevice->CreateBuffer(&bd, nullptr, &s_cb);
		}
		if (SUCCEEDED(hr))
		{
			D3D11_RASTERIZER_DESC rd = {};
			rd.FillMode = D3D11_FILL_SOLID;
			rd.CullMode = D3D11_CULL_NONE;
			rd.DepthClipEnable = TRUE;
			hr = HW.pDevice->CreateRasterizerState(&rd, &s_rs);
		}
		if (SUCCEEDED(hr))
		{
			D3D11_DEPTH_STENCIL_DESC dd = {}; // no depth, no stencil
			hr = HW.pDevice->CreateDepthStencilState(&dd, &s_ds);
		}
		if (SUCCEEDED(hr))
		{
			D3D11_BLEND_DESC bl = {};
			bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			hr = HW.pDevice->CreateBlendState(&bl, &s_bs);
		}
		if (FAILED(hr))
		{
			Msg("! Upscaler mask: shader setup failed: %s", err ? (const char*)err->GetBufferPointer() : "");
			s_failed = true;
		}
		_RELEASE(vs);
		_RELEASE(ps);
		_RELEASE(err);
		return !s_failed;
	}

	static bool ensure_targets(u32 w, u32 h, ID3D11Resource* mv, ID3D11Resource* depth)
	{
		if (s_tex && s_w == w && s_h == h && s_mv_res == mv && s_depth_res == depth)
			return true;
		_RELEASE(s_rtv);
		_RELEASE(s_tex);
		_RELEASE(s_mv_srv);
		_RELEASE(s_stencil_srv);
		s_mv_res = s_depth_res = nullptr;

		D3D11_TEXTURE2D_DESC d = {};
		d.Width = w;
		d.Height = h;
		d.MipLevels = d.ArraySize = 1;
		d.Format = DXGI_FORMAT_R8_UNORM;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		HRESULT hr = HW.pDevice->CreateTexture2D(&d, nullptr, &s_tex);
		if (SUCCEEDED(hr))
			hr = HW.pDevice->CreateRenderTargetView(s_tex, nullptr, &s_rtv);
		if (SUCCEEDED(hr))
			hr = HW.pDevice->CreateShaderResourceView(mv, nullptr, &s_mv_srv);
		if (SUCCEEDED(hr))
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
			sv.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
			sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sv.Texture2D.MipLevels = 1;
			hr = HW.pDevice->CreateShaderResourceView(depth, &sv, &s_stencil_srv);
		}
		if (FAILED(hr))
		{
			Msg("! Upscaler mask: targets failed (0x%08x)", hr);
			_RELEASE(s_rtv);
			_RELEASE(s_tex);
			_RELEASE(s_mv_srv);
			_RELEASE(s_stencil_srv);
			s_failed = true;
			return false;
		}
		s_w = w;
		s_h = h;
		s_mv_res = mv;
		s_depth_res = depth;
		return true;
	}

	// The mask for this frame, or null when nothing is masked. Raw D3D11: the caller resets the context and
	// RCache's state afterwards (phase_upscale does around the upscaler anyway)
	ID3D11Resource* build(u32 w, u32 h, ID3D11Resource* mv, ID3D11Resource* depth)
	{
		const Fvector4& wt = ps_r4_upscaler_mask;
		if (wt.x <= 0.f && wt.y <= 0.f && wt.z <= 0.f)
			return nullptr;
		if (!mv || !depth || !create_shaders() || !ensure_targets(w, h, mv, depth))
			return nullptr;

		ID3D11DeviceContext* ctx = HW.pContext;
		D3D11_MAPPED_SUBRESOURCE m;
		if (FAILED(ctx->Map(s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
			return nullptr;
		float* c = (float*)m.pData;
		c[0] = wt.x;
		c[1] = wt.y;
		c[2] = wt.z;
		c[3] = 0.f;
		ctx->Unmap(s_cb, 0);

		ctx->OMSetRenderTargets(1, &s_rtv, nullptr);
		D3D11_VIEWPORT vp = { 0.f, 0.f, float(w), float(h), 0.f, 1.f };
		ctx->RSSetViewports(1, &vp);
		ctx->IASetInputLayout(nullptr);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->VSSetShader(s_vs, nullptr, 0);
		ctx->GSSetShader(nullptr, nullptr, 0);
		ctx->HSSetShader(nullptr, nullptr, 0);
		ctx->DSSetShader(nullptr, nullptr, 0);
		ctx->PSSetShader(s_ps, nullptr, 0);
		ID3D11ShaderResourceView* srv[2] = { s_mv_srv, s_stencil_srv };
		ctx->PSSetShaderResources(0, 2, srv);
		ctx->PSSetConstantBuffers(0, 1, &s_cb);
		ctx->RSSetState(s_rs);
		ctx->OMSetDepthStencilState(s_ds, 0);
		const float blend[4] = {};
		ctx->OMSetBlendState(s_bs, blend, 0xffffffff);
		ctx->Draw(3, 0);
		ID3D11ShaderResourceView* none[2] = {};
		ctx->PSSetShaderResources(0, 2, none);
		ID3D11RenderTargetView* no_rtv = nullptr;
		ctx->OMSetRenderTargets(1, &no_rtv, nullptr);
		return s_tex;
	}
}

void upscale_mask_release() { upscale_mask::release(); }

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
	u32 _w = Device.dwRenderWidth;
	u32 _h = Device.dwRenderHeight;
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
	if (dwHeight != Device.dwRenderHeight) param_blur = 1.f;
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
	u_setrt(Device.dwRenderWidth, Device.dwRenderHeight, HW.pBaseRT,NULL,NULL, HW.pBaseZB);
	//	Element 0 for for normal post-process
	//	Element 4 for color map post-process
	bool bCMap = u_need_CM();
	//RCache.set_Element	(s_postprocess->E[bCMap ? 4 : 0]);
	if (m_upscale_done)
	{
		RCache.set_Element(s_postprocess_upscaled->E[bCMap ? 4 : 0]);
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
	float _w = float(Device.dwRenderWidth);
	float _h = float(Device.dwRenderHeight);

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

void CRenderTarget::upscale_begin_scene()
{
	m_upscale_done = false;
	if (!m_upscale_enabled || m_upscale_in_scene)
		return;
	m_upscale_in_scene = true;

	Device.dwRenderWidth = m_upscale_render_w;
	Device.dwRenderHeight = m_upscale_render_h;
	m_upscale_saved_zb = HW.pBaseZB;
	HW.pBaseZB = rt_upscale_depth->pZRT;
	RImplementation.upscale_scene_active = true;

	// Second viewport (3D scope) frames are not fed to the upscaler, render them without jitter
	if (Device.m_SecondViewport.IsSVPFrame())
	{
		m_upscale_jitter.set(0.f, 0.f);
		RImplementation.upscale_jitter_on = false;
		RImplementation.upscale_jitter_ndc.set(0.f, 0.f);
	}
	else
	{
		float jx, jy;
		CUpscaler::GetJitter(m_upscale_jitter_index, m_upscale_jitter_phases, jx, jy);
		m_upscale_jitter.set(jx, jy);
		// Pixel offset to clip space, pixel Y goes down
		RImplementation.upscale_jitter_on = true;
		RImplementation.upscale_jitter_ndc.set(2.f * jx / float(m_upscale_render_w), -2.f * jy / float(m_upscale_render_h));
	}

	// Viewport follows the last bound target, start the frame at render resolution
	u_setrt(rt_Generic_0, NULL, NULL, HW.pBaseZB);
	RImplementation.rmNormal();
}

void CRenderTarget::upscale_end_scene()
{
	RImplementation.upscale_jitter_on = false;
	RImplementation.upscale_scene_active = false;
	if (!m_upscale_in_scene)
		return;
	m_upscale_in_scene = false;

	Device.dwRenderWidth = m_upscale_output_w;
	Device.dwRenderHeight = m_upscale_output_h;
	HW.pBaseZB = m_upscale_saved_zb;
	m_upscale_saved_zb = nullptr;

	u_setrt(Device.dwRenderWidth, Device.dwRenderHeight, HW.pBaseRT, NULL, NULL, HW.pBaseZB);
	RImplementation.rmNormal();
}

void CRenderTarget::phase_upscale()
{
	if (!m_upscale_in_scene)
		return;

	if (!Device.m_SecondViewport.IsSVPFrame())
	{
		PIX_EVENT(phase_upscale);

		// History is useless after a pause (menus, loading) or a camera cut
		if (Device.dwFrame - m_upscale_last_frame > 4 || Device.vCameraPosition.distance_to(m_upscale_last_cam) > 10.f)
			m_upscale_reset = true;

		// Camera from the main projection: near/far from the depth terms, vertical FOV from the Y scale
		const Fmatrix& P = Device.mProject;
		const float q = P._33;

		UpscalerEvalParams p = {};
		p.color = rt_Color->pSurface;
		p.output = rt_upscale_output->pSurface;
		p.depth = rt_upscale_depth->pSurface;
		p.motion = rt_ssfx_motion_vectors->pSurface;
		p.jitter_x = m_upscale_jitter.x * ps_r4_upscaler_signs.z;
		p.jitter_y = m_upscale_jitter.y * ps_r4_upscaler_signs.w;
		// SSS motion vectors are (current - previous) in UV units, upscalers want (previous - current) in pixels
		p.mv_scale_x = -float(m_upscale_render_w) * ps_r4_upscaler_signs.x;
		p.mv_scale_y = -float(m_upscale_render_h) * ps_r4_upscaler_signs.y;
		p.render_w = m_upscale_render_w;
		p.render_h = m_upscale_render_h;
		p.output_w = m_upscale_output_w;
		p.output_h = m_upscale_output_h;
		p.reset = m_upscale_reset;
		p.frame_time_ms = Device.fTimeDelta * 1000.f;
		p.camera_near = -P._43 / q;
		p.camera_far = -P._43 / (q - 1.f);
		p.camera_fov_y = 2.f * atanf(1.f / P._22);
		p.sharpness = ps_r4_fsr_sharpness;

		// The upscaler binds its own state, start and leave with a clean context
		// (OnFrameEnd clears the D3D context and RCache's cached state)
		RCache.OnFrameEnd();
		p.reactive = upscale_mask::build(m_upscale_render_w, m_upscale_render_h, p.motion, p.depth);
		m_upscale_done = m_upscaler->Evaluate(p);
		RCache.OnFrameEnd();

		// The screen overlays held back for this (phase_combine). Without an upscaled image at render resolution,
		// onto rt_Color, which phase_pp stretches
		if (m_overlays_after_upscale)
		{
			if (m_upscale_done)
				phase_screen_overlays_upscaled();
			else
				phase_screen_overlays();
			m_overlays_after_upscale = false;
		}

		if (m_upscale_done)
		{
			m_upscale_reset = false;
			m_upscale_jitter_index++;
			m_upscale_last_frame = Device.dwFrame;
			m_upscale_last_cam.set(Device.vCameraPosition);
		}
	}

	// Without an upscaled image phase_pp stretches rt_Color bilinearly
	upscale_end_scene();
}
