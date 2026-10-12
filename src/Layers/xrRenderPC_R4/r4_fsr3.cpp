#include "stdafx.h"
#include "r4_fsr3.h"

CFSR3 g_FSR3;

// FSR render scale per quality mode (AMD's fixed ratios: 3.0x, 2.0x, 1.7x, 1.5x, 1.0x)
static const float s_fsr_scale[] = { 1.f / 3.f, 0.5f, 1.f / 1.7f, 1.f / 1.5f, 1.f };

void CFSR3::GetRenderSize(u32 out_w, u32 out_h, u32 quality, u32& render_w, u32& render_h)
{
	quality = _min(quality, u32(upscaler_quality_native));
	render_w = _max(u32(iFloor(float(out_w) * s_fsr_scale[quality] + 0.5f)), 1u);
	render_h = _max(u32(iFloor(float(out_h) * s_fsr_scale[quality] + 0.5f)), 1u);
}

#if __has_include("FidelityFX-FSR3/generated/fsr3_dx11_shaders.h")
#define XR_FSR3_SHADERS
#endif

#ifdef XR_FSR3_SHADERS

#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include "FidelityFX-FSR3/generated/fsr3_dx11_shaders.h"

// Matches FSR3UPSCALER_SHADER_PERMUTATION_* in ffx_fsr3upscaler_private.h
static const uint32_t FSR3_PERMUTATION_LOW_RES_MOTION_VECTORS = 1 << 2;
static const uint32_t FSR3_PERMUTATION_ENABLE_SHARPENING = 1 << 5;

//////////////////////////////////////////////////////////////////////////
// FidelityFX backend interface on D3D11

namespace
{
	const u32 kMaxResources = 128;
	const u32 kMaxMips = 16;
	const u32 kMaxSrvSlots = 32;
	const u32 kMaxUavSlots = 8; // D3D11.0 compute shaders
	const u32 kStagingSize = 64 * 1024;

	struct Dx11Resource
	{
		ID3D11Resource* resource;
		bool owned;
		bool is_uint;
		FfxResourceDescription desc;
		ID3D11ShaderResourceView* srv;
		ID3D11UnorderedAccessView* uav[kMaxMips];
		DXGI_FORMAT view_format;
	};

	struct Backend
	{
		ID3D11Device* device;
		ID3D11DeviceContext* context;
		Dx11Resource resources[kMaxResources];
		u32 next_static;  // grows up from 1 (0 is the null resource)
		u32 next_dynamic; // grows down, reset every frame by UnregisterResources
		ID3D11Buffer* cb[FFX_MAX_NUM_CONST_BUFFERS];
		u32 cb_size[FFX_MAX_NUM_CONST_BUFFERS];
		ID3D11SamplerState* samplers[2]; // s0 point clamp, s1 linear clamp
		xr_vector<FfxGpuJobDescription> jobs;
		u32 staging_pos;
		alignas(16) u8 staging[kStagingSize];

		Backend()
		{
			device = nullptr;
			context = nullptr;
			ZeroMemory(resources, sizeof(resources));
			next_static = 1;
			next_dynamic = kMaxResources - 1;
			ZeroMemory(cb, sizeof(cb));
			ZeroMemory(cb_size, sizeof(cb_size));
			ZeroMemory(samplers, sizeof(samplers));
			staging_pos = 0;
		}
	};

	Backend* get_backend(FfxInterface* iface) { return (Backend*)iface->scratchBuffer; }

	DXGI_FORMAT to_dxgi(FfxSurfaceFormat f)
	{
		switch (f)
		{
		case FFX_SURFACE_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_TYPELESS;
		case FFX_SURFACE_FORMAT_R32G32B32A32_UINT: return DXGI_FORMAT_R32G32B32A32_UINT;
		case FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
		case FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		case FFX_SURFACE_FORMAT_R32G32B32_FLOAT: return DXGI_FORMAT_R32G32B32_FLOAT;
		case FFX_SURFACE_FORMAT_R32G32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
		case FFX_SURFACE_FORMAT_R8_UINT: return DXGI_FORMAT_R8_UINT;
		case FFX_SURFACE_FORMAT_R32_UINT: return DXGI_FORMAT_R32_UINT;
		case FFX_SURFACE_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
		case FFX_SURFACE_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case FFX_SURFACE_FORMAT_R8G8B8A8_SNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
		case FFX_SURFACE_FORMAT_R8G8B8A8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		case FFX_SURFACE_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
		case FFX_SURFACE_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case FFX_SURFACE_FORMAT_B8G8R8A8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
		case FFX_SURFACE_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
		case FFX_SURFACE_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
		case FFX_SURFACE_FORMAT_R16G16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
		case FFX_SURFACE_FORMAT_R16G16_UINT: return DXGI_FORMAT_R16G16_UINT;
		case FFX_SURFACE_FORMAT_R16G16_SINT: return DXGI_FORMAT_R16G16_SINT;
		case FFX_SURFACE_FORMAT_R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
		case FFX_SURFACE_FORMAT_R16_UINT: return DXGI_FORMAT_R16_UINT;
		case FFX_SURFACE_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_UNORM;
		case FFX_SURFACE_FORMAT_R16_SNORM: return DXGI_FORMAT_R16_SNORM;
		case FFX_SURFACE_FORMAT_R8_UNORM: return DXGI_FORMAT_R8_UNORM;
		case FFX_SURFACE_FORMAT_R8G8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
		case FFX_SURFACE_FORMAT_R8G8_UINT: return DXGI_FORMAT_R8G8_UINT;
		case FFX_SURFACE_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
		case FFX_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP: return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
		case FFX_SURFACE_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
		case FFX_SURFACE_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_TYPELESS;
		case FFX_SURFACE_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
		case FFX_SURFACE_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_TYPELESS;
		case FFX_SURFACE_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_TYPELESS;
		case FFX_SURFACE_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_TYPELESS;
		case FFX_SURFACE_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_TYPELESS;
		case FFX_SURFACE_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_TYPELESS;
		default: return DXGI_FORMAT_UNKNOWN;
		}
	}

	// Typed format for views of typeless textures (engine depth is R24G8_TYPELESS)
	DXGI_FORMAT view_format(DXGI_FORMAT f)
	{
		switch (f)
		{
		case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
		case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
		case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
		case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
		case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
		case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
		case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
		case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
		case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
		default: return f;
		}
	}

	bool is_uint_format(DXGI_FORMAT f)
	{
		switch (f)
		{
		case DXGI_FORMAT_R32G32B32A32_UINT:
		case DXGI_FORMAT_R32_UINT:
		case DXGI_FORMAT_R16G16_UINT:
		case DXGI_FORMAT_R16_UINT:
		case DXGI_FORMAT_R8G8_UINT:
		case DXGI_FORMAT_R8_UINT:
			return true;
		default:
			return false;
		}
	}

	u32 bytes_per_pixel(DXGI_FORMAT f)
	{
		switch (f)
		{
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
		case DXGI_FORMAT_R32G32B32A32_UINT: return 16;
		case DXGI_FORMAT_R32G32B32_FLOAT: return 12;
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R32G32_FLOAT: return 8;
		case DXGI_FORMAT_R8_UINT:
		case DXGI_FORMAT_R8_UNORM: return 1;
		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_R16_UINT:
		case DXGI_FORMAT_R16_UNORM:
		case DXGI_FORMAT_R16_SNORM:
		case DXGI_FORMAT_R8G8_UNORM:
		case DXGI_FORMAT_R8G8_UINT: return 2;
		default: return 4;
		}
	}

	void release_views(Dx11Resource& r)
	{
		_RELEASE(r.srv);
		for (u32 m = 0; m < kMaxMips; ++m)
			_RELEASE(r.uav[m]);
	}

	void release_resource(Dx11Resource& r)
	{
		release_views(r);
		if (r.owned)
			_RELEASE(r.resource);
		ZeroMemory(&r, sizeof(r));
	}

	ID3D11UnorderedAccessView* get_uav(Backend* b, Dx11Resource& r, u32 mip)
	{
		if (!r.resource || mip >= kMaxMips)
			return nullptr;
		if (!r.uav[mip])
		{
			D3D11_UNORDERED_ACCESS_VIEW_DESC d = {};
			d.Format = r.view_format;
			d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			d.Texture2D.MipSlice = mip;
			if (FAILED(b->device->CreateUnorderedAccessView(r.resource, &d, &r.uav[mip])))
				r.uav[mip] = nullptr;
		}
		return r.uav[mip];
	}

	// Creates the SRV (all mips) for a texture; UAVs are created on first use per mip
	bool create_views(Backend* b, Dx11Resource& r)
	{
		ID3D11Texture2D* tex = nullptr;
		if (FAILED(r.resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)))
			return false;
		D3D11_TEXTURE2D_DESC td;
		tex->GetDesc(&td);
		tex->Release();

		r.view_format = view_format(td.Format);
		r.is_uint = is_uint_format(r.view_format);
		r.desc.width = td.Width;
		r.desc.height = td.Height;
		r.desc.depth = 1;
		r.desc.mipCount = td.MipLevels;

		if (td.BindFlags & D3D11_BIND_SHADER_RESOURCE)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
			d.Format = r.view_format;
			d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			d.Texture2D.MostDetailedMip = 0;
			d.Texture2D.MipLevels = td.MipLevels;
			if (FAILED(b->device->CreateShaderResourceView(r.resource, &d, &r.srv)))
				return false;
		}
		return true;
	}

	FfxVersionNumber ffx_GetSDKVersion(FfxInterface*) { return FFX_SDK_MAKE_VERSION(1, 1, 4); }

	FfxErrorCode ffx_GetEffectGpuMemoryUsage(FfxInterface*, FfxUInt32, FfxEffectMemoryUsage* out)
	{
		out->totalUsageInBytes = 0;
		out->aliasableUsageInBytes = 0;
		return FFX_OK;
	}

	FfxErrorCode ffx_CreateBackendContext(FfxInterface*, FfxEffect, FfxEffectBindlessConfig*, FfxUInt32* id)
	{
		*id = 0;
		return FFX_OK;
	}

	FfxErrorCode ffx_GetDeviceCapabilities(FfxInterface*, FfxDeviceCapabilities* caps)
	{
		ZeroMemory(caps, sizeof(*caps));
		// Selects the shader permutations compiled by compile_shaders.ps1: no fp16, no wave64, no Lanczos LUT
		caps->maximumSupportedShaderModel = FFX_SHADER_MODEL_5_1;
		caps->waveLaneCountMin = 64;
		caps->waveLaneCountMax = 64;
		caps->fp16Supported = false;
		return FFX_OK;
	}

	FfxErrorCode ffx_DestroyBackendContext(FfxInterface* iface, FfxUInt32)
	{
		Backend* b = get_backend(iface);
		for (u32 i = 0; i < kMaxResources; ++i)
			release_resource(b->resources[i]);
		b->next_static = 1;
		b->next_dynamic = kMaxResources - 1;
		b->jobs.clear();
		return FFX_OK;
	}

	FfxErrorCode ffx_CreateResource(FfxInterface* iface, const FfxCreateResourceDescription* desc, FfxUInt32, FfxResourceInternal* out)
	{
		Backend* b = get_backend(iface);
		const FfxResourceDescription& rd = desc->resourceDescription;

		if (rd.type != FFX_RESOURCE_TYPE_TEXTURE2D && rd.type != FFX_RESOURCE_TYPE_TEXTURE1D)
		{
			Msg("! FSR: unsupported resource type %d", int(rd.type));
			return FFX_ERROR_BACKEND_API_ERROR;
		}
		if (b->next_static >= b->next_dynamic)
			return FFX_ERROR_OUT_OF_MEMORY;

		const DXGI_FORMAT format = to_dxgi(rd.format);

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = rd.width;
		td.Height = rd.type == FFX_RESOURCE_TYPE_TEXTURE1D ? 1 : rd.height;
		td.MipLevels = rd.mipCount; // 0 = full chain
		td.ArraySize = 1;
		td.Format = format;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (rd.usage & FFX_RESOURCE_USAGE_UAV)
			td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

		// Initial data for the small lookup/default textures (single mip)
		xr_vector<u8> fill;
		D3D11_SUBRESOURCE_DATA init = {};
		D3D11_SUBRESOURCE_DATA* init_ptr = nullptr;
		const u32 pitch = td.Width * bytes_per_pixel(format);
		if (desc->initData.type == FFX_RESOURCE_INIT_DATA_TYPE_BUFFER && desc->initData.buffer)
		{
			init.pSysMem = desc->initData.buffer;
			init.SysMemPitch = pitch;
			init_ptr = &init;
		}
		else if (desc->initData.type == FFX_RESOURCE_INIT_DATA_TYPE_VALUE)
		{
			fill.assign(size_t(pitch) * td.Height, desc->initData.value);
			init.pSysMem = fill.data();
			init.SysMemPitch = pitch;
			init_ptr = &init;
		}
		if (init_ptr)
			td.MipLevels = 1;

		ID3D11Texture2D* tex = nullptr;
		if (FAILED(b->device->CreateTexture2D(&td, init_ptr, &tex)))
		{
			Msg("! FSR: failed to create %ls (%ux%u, format %d)", desc->name ? desc->name : L"", td.Width, td.Height, int(format));
			return FFX_ERROR_BACKEND_API_ERROR;
		}

		const u32 index = b->next_static++;
		Dx11Resource& r = b->resources[index];
		ZeroMemory(&r, sizeof(r));
		r.resource = tex;
		r.owned = true;
		r.desc = rd;
		if (!create_views(b, r))
		{
			release_resource(r);
			return FFX_ERROR_BACKEND_API_ERROR;
		}

		out->internalIndex = int32_t(index);
		return FFX_OK;
	}

	FfxErrorCode ffx_DestroyResource(FfxInterface* iface, FfxResourceInternal res, FfxUInt32)
	{
		Backend* b = get_backend(iface);
		if (res.internalIndex > 0 && u32(res.internalIndex) < kMaxResources)
			release_resource(b->resources[res.internalIndex]);
		return FFX_OK;
	}

	FfxErrorCode ffx_RegisterResource(FfxInterface* iface, const FfxResource* in, FfxUInt32, FfxResourceInternal* out)
	{
		Backend* b = get_backend(iface);
		if (!in->resource)
		{
			out->internalIndex = 0; // null resource
			return FFX_OK;
		}
		if (b->next_dynamic <= b->next_static)
			return FFX_ERROR_OUT_OF_MEMORY;

		const u32 index = b->next_dynamic--;
		Dx11Resource& r = b->resources[index];
		release_resource(r);
		r.resource = (ID3D11Resource*)in->resource;
		r.owned = false;
		r.desc = in->description;
		if (!create_views(b, r))
		{
			release_resource(r);
			out->internalIndex = 0;
			return FFX_ERROR_BACKEND_API_ERROR;
		}

		out->internalIndex = int32_t(index);
		return FFX_OK;
	}

	FfxErrorCode ffx_UnregisterResources(FfxInterface* iface, FfxCommandList, FfxUInt32)
	{
		Backend* b = get_backend(iface);
		for (u32 i = b->next_dynamic + 1; i < kMaxResources; ++i)
			release_resource(b->resources[i]);
		b->next_dynamic = kMaxResources - 1;
		return FFX_OK;
	}

	FfxResource ffx_GetResource(FfxInterface* iface, FfxResourceInternal res)
	{
		Backend* b = get_backend(iface);
		FfxResource out = {};
		if (res.internalIndex > 0 && u32(res.internalIndex) < kMaxResources)
		{
			out.resource = b->resources[res.internalIndex].resource;
			out.description = b->resources[res.internalIndex].desc;
		}
		return out;
	}

	FfxResourceDescription ffx_GetResourceDescription(FfxInterface* iface, FfxResourceInternal res)
	{
		Backend* b = get_backend(iface);
		if (res.internalIndex > 0 && u32(res.internalIndex) < kMaxResources)
			return b->resources[res.internalIndex].desc;
		FfxResourceDescription empty = {};
		return empty;
	}

	FfxErrorCode ffx_StageConstantBufferData(FfxInterface* iface, void* data, FfxUInt32 size, FfxConstantBuffer* cb)
	{
		Backend* b = get_backend(iface);
		if (!data || !cb)
			return FFX_ERROR_INVALID_POINTER;

		const u32 aligned = (size + 255) & ~255u;
		if (b->staging_pos + aligned > kStagingSize)
			b->staging_pos = 0;

		u32* dst = (u32*)(b->staging + b->staging_pos);
		CopyMemory(dst, data, size);
		cb->data = dst;
		cb->num32BitEntries = size / sizeof(u32);
		b->staging_pos += aligned;
		return FFX_OK;
	}

	const char* pass_shader_name(FfxPass pass)
	{
		switch (pass)
		{
		case FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS: return "prepare_inputs";
		case FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID: return "luma_pyramid";
		case FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID: return "shading_change_pyramid";
		case FFX_FSR3UPSCALER_PASS_SHADING_CHANGE: return "shading_change";
		case FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY: return "prepare_reactivity";
		case FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY: return "luma_instability";
		case FFX_FSR3UPSCALER_PASS_ACCUMULATE:
		case FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN: return "accumulate";
		case FFX_FSR3UPSCALER_PASS_RCAS: return "rcas";
		case FFX_FSR3UPSCALER_PASS_DEBUG_VIEW: return "debug_view";
		case FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE: return "autogen_reactive";
		default: return nullptr;
		}
	}

	void to_wide(const char* src, wchar_t* dst, size_t dst_size)
	{
		MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, int(dst_size));
	}

	FfxErrorCode ffx_CreatePipeline(FfxInterface* iface, FfxEffect, FfxPass pass, uint32_t permutation,
	                            const FfxPipelineDescription* desc, FfxUInt32, FfxPipelineState* out)
	{
		Backend* b = get_backend(iface);

		// Only the configuration compiled by compile_shaders.ps1 is available
		const uint32_t supported = FSR3_PERMUTATION_LOW_RES_MOTION_VECTORS | FSR3_PERMUTATION_ENABLE_SHARPENING;
		if ((permutation & ~supported) || !(permutation & FSR3_PERMUTATION_LOW_RES_MOTION_VECTORS))
		{
			Msg("! FSR: shader permutation 0x%x is not compiled for D3D11", permutation);
			return FFX_ERROR_BACKEND_API_ERROR;
		}

		const char* name = pass_shader_name(pass);
		const u32 sharpen = (permutation & FSR3_PERMUTATION_ENABLE_SHARPENING) ? 1 : 0;
		const FsrDx11Shader* shader = nullptr;
		for (const FsrDx11Shader& s : g_fsr3_dx11_shaders)
		{
			if (name && s.sharpen == sharpen && 0 == xr_strcmp(s.pass, name))
			{
				shader = &s;
				break;
			}
		}
		if (!shader)
		{
			Msg("! FSR: no D3D11 shader for pass %u", pass);
			return FFX_ERROR_BACKEND_API_ERROR;
		}

		ID3D11ComputeShader* cs = nullptr;
		if (FAILED(b->device->CreateComputeShader(shader->code, shader->size, nullptr, &cs)))
		{
			Msg("! FSR: CreateComputeShader failed for %s", shader->pass);
			return FFX_ERROR_BACKEND_API_ERROR;
		}

		out->pipeline = cs;
		out->srvTextureCount = 0;
		out->uavTextureCount = 0;
		out->srvBufferCount = 0;
		out->uavBufferCount = 0;
		out->constCount = 0;
		for (u32 i = 0; i < shader->binding_count; ++i)
		{
			const FsrDx11Binding& bind = shader->bindings[i];
			for (u32 a = 0; a < bind.count; ++a)
			{
				FfxResourceBinding* dst = nullptr;
				switch (bind.type)
				{
				case 't': dst = &out->srvTextureBindings[out->srvTextureCount++]; break;
				case 'u': dst = &out->uavTextureBindings[out->uavTextureCount++]; break;
				case 'c': dst = &out->constantBufferBindings[out->constCount++]; break;
				default: break; // samplers are bound by slot
				}
				if (!dst)
					continue;
				dst->slotIndex = bind.slot;
				dst->arrayIndex = a;
				dst->resourceIdentifier = 0;
				to_wide(bind.name, dst->name, FFX_RESOURCE_NAME_SIZE);
			}
		}
		wcscpy_s(out->name, desc->name);
		return FFX_OK;
	}

	FfxErrorCode ffx_DestroyPipeline(FfxInterface*, FfxPipelineState* pipeline, FfxUInt32)
	{
		if (pipeline && pipeline->pipeline)
		{
			((ID3D11ComputeShader*)pipeline->pipeline)->Release();
			pipeline->pipeline = nullptr;
		}
		return FFX_OK;
	}

	FfxErrorCode ffx_ScheduleGpuJob(FfxInterface* iface, const FfxGpuJobDescription* job)
	{
		get_backend(iface)->jobs.push_back(*job);
		return FFX_OK;
	}

	bool upload_cb(Backend* b, u32 slot, const FfxConstantBuffer& cb)
	{
		if (slot >= FFX_MAX_NUM_CONST_BUFFERS || !cb.data)
			return false;

		const u32 size = (cb.num32BitEntries * sizeof(u32) + 15) & ~15u;
		if (!b->cb[slot] || b->cb_size[slot] < size)
		{
			_RELEASE(b->cb[slot]);
			D3D11_BUFFER_DESC d = {};
			d.ByteWidth = _max(size, 256u);
			d.Usage = D3D11_USAGE_DYNAMIC;
			d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(b->device->CreateBuffer(&d, nullptr, &b->cb[slot])))
				return false;
			b->cb_size[slot] = d.ByteWidth;
		}

		D3D11_MAPPED_SUBRESOURCE mapped;
		if (FAILED(b->context->Map(b->cb[slot], 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return false;
		CopyMemory(mapped.pData, cb.data, cb.num32BitEntries * sizeof(u32));
		b->context->Unmap(b->cb[slot], 0);
		return true;
	}

	void execute_compute(Backend* b, const FfxComputeJobDescription& job)
	{
		ID3D11DeviceContext* ctx = b->context;
		const FfxPipelineState& p = job.pipeline;

		ctx->CSSetShader((ID3D11ComputeShader*)p.pipeline, nullptr, 0);
		ctx->CSSetSamplers(0, 2, b->samplers);

		for (u32 i = 0; i < p.constCount; ++i)
		{
			const u32 slot = p.constantBufferBindings[i].slotIndex;
			if (upload_cb(b, slot, job.cbs[i]))
				ctx->CSSetConstantBuffers(slot, 1, &b->cb[slot]);
		}

		ID3D11ShaderResourceView* srv[kMaxSrvSlots] = {};
		u32 srv_count = 0;
		for (u32 i = 0; i < p.srvTextureCount; ++i)
		{
			const u32 slot = p.srvTextureBindings[i].slotIndex + p.srvTextureBindings[i].arrayIndex;
			const int32_t idx = job.srvTextures[i].resource.internalIndex;
			if (slot >= kMaxSrvSlots)
				continue;
			srv[slot] = (idx > 0 && u32(idx) < kMaxResources) ? b->resources[idx].srv : nullptr;
			srv_count = _max(srv_count, slot + 1);
		}

		ID3D11UnorderedAccessView* uav[kMaxUavSlots] = {};
		u32 uav_count = 0;
		for (u32 i = 0; i < p.uavTextureCount; ++i)
		{
			const u32 slot = p.uavTextureBindings[i].slotIndex + p.uavTextureBindings[i].arrayIndex;
			const int32_t idx = job.uavTextures[i].resource.internalIndex;
			if (slot >= kMaxUavSlots)
				continue;
			uav[slot] = (idx > 0 && u32(idx) < kMaxResources) ? get_uav(b, b->resources[idx], job.uavTextures[i].mip) : nullptr;
			uav_count = _max(uav_count, slot + 1);
		}

		if (srv_count)
			ctx->CSSetShaderResources(0, srv_count, srv);
		if (uav_count)
			ctx->CSSetUnorderedAccessViews(0, uav_count, uav, nullptr);

		ctx->Dispatch(job.dimensions[0], job.dimensions[1], job.dimensions[2]);

		// Unbind, the next pass may read what this one wrote
		ID3D11ShaderResourceView* null_srv[kMaxSrvSlots] = {};
		ID3D11UnorderedAccessView* null_uav[kMaxUavSlots] = {};
		if (srv_count)
			ctx->CSSetShaderResources(0, srv_count, null_srv);
		if (uav_count)
			ctx->CSSetUnorderedAccessViews(0, uav_count, null_uav, nullptr);
	}

	void execute_clear(Backend* b, const FfxClearFloatJobDescription& job)
	{
		const int32_t idx = job.target.internalIndex;
		if (idx <= 0 || u32(idx) >= kMaxResources)
			return;
		Dx11Resource& r = b->resources[idx];
		ID3D11UnorderedAccessView* view = get_uav(b, r, 0);
		if (!view)
			return;
		if (r.is_uint)
		{
			const UINT v[4] = { UINT(job.color[0]), UINT(job.color[1]), UINT(job.color[2]), UINT(job.color[3]) };
			b->context->ClearUnorderedAccessViewUint(view, v);
		}
		else
			b->context->ClearUnorderedAccessViewFloat(view, job.color);
	}

	FfxErrorCode ffx_ExecuteGpuJobs(FfxInterface* iface, FfxCommandList, FfxUInt32)
	{
		Backend* b = get_backend(iface);
		for (const FfxGpuJobDescription& job : b->jobs)
		{
			switch (job.jobType)
			{
			case FFX_GPU_JOB_COMPUTE:
				execute_compute(b, job.computeJobDescriptor);
				break;
			case FFX_GPU_JOB_CLEAR_FLOAT:
				execute_clear(b, job.clearJobDescriptor);
				break;
			case FFX_GPU_JOB_COPY:
			{
				const int32_t src = job.copyJobDescriptor.src.internalIndex;
				const int32_t dst = job.copyJobDescriptor.dst.internalIndex;
				if (src > 0 && dst > 0 && b->resources[src].resource && b->resources[dst].resource)
					b->context->CopyResource(b->resources[dst].resource, b->resources[src].resource);
				break;
			}
			default: // barriers and discards are not needed on D3D11
				break;
			}
		}
		b->jobs.clear();
		b->staging_pos = 0;
		return FFX_OK;
	}

	FfxResource make_resource(ID3D11Resource* res, FfxResourceStates state, const wchar_t* name)
	{
		FfxResource r = {};
		r.resource = res;
		r.state = state;
		if (res)
		{
			ID3D11Texture2D* tex = nullptr;
			if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)))
			{
				D3D11_TEXTURE2D_DESC td;
				tex->GetDesc(&td);
				tex->Release();
				r.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
				r.description.width = td.Width;
				r.description.height = td.Height;
				r.description.depth = 1;
				r.description.mipCount = td.MipLevels;
				r.description.usage = (td.BindFlags & D3D11_BIND_UNORDERED_ACCESS) ? FFX_RESOURCE_USAGE_UAV : FFX_RESOURCE_USAGE_READ_ONLY;
			}
		}
		wcscpy_s(r.name, name);
		return r;
	}

	void fsr_message(FfxMsgType type, const wchar_t* message)
	{
		Msg("%s FSR: %ls", type == FFX_MESSAGE_TYPE_ERROR ? "!" : "~", message);
	}
}

bool CFSR3::Init()
{
	if (m_initialized)
		return m_available;
	m_initialized = true;
	m_available = HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0;
	if (!m_available)
		Msg("! FSR: Direct3D 11.0 compute shaders required");
	else
		Msg("* FSR: FidelityFX FSR 3.1.4 upscaler available (D3D11 backend)");
	return m_available;
}

void CFSR3::Shutdown()
{
	ReleaseFeature();
	m_initialized = false;
	m_available = false;
}

bool CFSR3::CreateFeature(u32 render_w, u32 render_h, u32 out_w, u32 out_h, u32 quality)
{
	ReleaseFeature();
	if (!m_available)
		return false;

	Backend* b = xr_new<Backend>();
	b->device = HW.pDevice;
	b->context = HW.pContext;
	b->jobs.reserve(32);

	D3D11_SAMPLER_DESC sd = {};
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	sd.MaxAnisotropy = 1;
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	HW.pDevice->CreateSamplerState(&sd, &b->samplers[0]);
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	HW.pDevice->CreateSamplerState(&sd, &b->samplers[1]);
	m_backend = b;

	FfxFsr3UpscalerContextDescription cd = {};
	// LDR color, render-resolution motion vectors without jitter, regular depth: matches compile_shaders.ps1
	cd.flags = 0;
	cd.maxRenderSize.width = render_w;
	cd.maxRenderSize.height = render_h;
	cd.maxUpscaleSize.width = out_w;
	cd.maxUpscaleSize.height = out_h;
	cd.fpMessage = fsr_message;

	FfxInterface& fi = cd.backendInterface;
	fi.fpGetSDKVersion = ffx_GetSDKVersion;
	fi.fpGetEffectGpuMemoryUsage = ffx_GetEffectGpuMemoryUsage;
	fi.fpCreateBackendContext = ffx_CreateBackendContext;
	fi.fpGetDeviceCapabilities = ffx_GetDeviceCapabilities;
	fi.fpDestroyBackendContext = ffx_DestroyBackendContext;
	fi.fpCreateResource = ffx_CreateResource;
	fi.fpRegisterResource = ffx_RegisterResource;
	fi.fpGetResource = ffx_GetResource;
	fi.fpUnregisterResources = ffx_UnregisterResources;
	fi.fpGetResourceDescription = ffx_GetResourceDescription;
	fi.fpDestroyResource = ffx_DestroyResource;
	fi.fpStageConstantBufferDataFunc = ffx_StageConstantBufferData;
	fi.fpCreatePipeline = ffx_CreatePipeline;
	fi.fpDestroyPipeline = ffx_DestroyPipeline;
	fi.fpScheduleGpuJob = ffx_ScheduleGpuJob;
	fi.fpExecuteGpuJobs = ffx_ExecuteGpuJobs;
	fi.scratchBuffer = b;
	fi.scratchBufferSize = sizeof(Backend);
	fi.device = HW.pDevice;

	FfxFsr3UpscalerContext* ctx = xr_new<FfxFsr3UpscalerContext>();
	const FfxErrorCode res = ffxFsr3UpscalerContextCreate(ctx, &cd);
	if (res != FFX_OK)
	{
		Msg("! FSR: context creation failed (0x%08x) for %ux%u -> %ux%u", u32(res), render_w, render_h, out_w, out_h);
		xr_delete(ctx);
		ReleaseFeature();
		return false;
	}
	m_context = ctx;

	// Shared resources FSR 3.1 expects from the application
	FfxFsr3UpscalerSharedResourceDescriptions shared = {};
	ffxFsr3UpscalerGetSharedResourceDescriptions(ctx, &shared);
	const FfxCreateResourceDescription* shared_desc[3] = { &shared.dilatedDepth, &shared.dilatedMotionVectors, &shared.reconstructedPrevNearestDepth };
	for (u32 i = 0; i < 3; ++i)
	{
		const FfxResourceDescription& rd = shared_desc[i]->resourceDescription;
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = rd.width;
		td.Height = rd.height;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = to_dxgi(rd.format);
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		if (FAILED(HW.pDevice->CreateTexture2D(&td, nullptr, &m_shared[i])))
		{
			Msg("! FSR: failed to create shared resource %ls", shared_desc[i]->name);
			ReleaseFeature();
			return false;
		}
	}

	m_eval_failures = 0;
	Msg("* FSR: context created, %ux%u -> %ux%u, quality %u", render_w, render_h, out_w, out_h, quality);
	return true;
}

void CFSR3::ReleaseFeature()
{
	if (m_context)
	{
		FfxFsr3UpscalerContext* ctx = (FfxFsr3UpscalerContext*)m_context;
		ffxFsr3UpscalerContextDestroy(ctx);
		xr_delete(ctx);
		m_context = nullptr;
	}
	for (u32 i = 0; i < 3; ++i)
		_RELEASE(m_shared[i]);
	if (m_backend)
	{
		Backend* b = (Backend*)m_backend;
		for (u32 i = 0; i < kMaxResources; ++i)
			release_resource(b->resources[i]);
		for (u32 i = 0; i < FFX_MAX_NUM_CONST_BUFFERS; ++i)
			_RELEASE(b->cb[i]);
		_RELEASE(b->samplers[0]);
		_RELEASE(b->samplers[1]);
		xr_delete(b);
		m_backend = nullptr;
	}
}

bool CFSR3::Evaluate(const UpscalerEvalParams& p)
{
	if (!m_context)
		return false;

	FfxFsr3UpscalerDispatchDescription d = {};
	d.commandList = HW.pContext;
	d.color = make_resource(p.color, FFX_RESOURCE_STATE_COMPUTE_READ, L"FSR3_Color");
	d.depth = make_resource(p.depth, FFX_RESOURCE_STATE_COMPUTE_READ, L"FSR3_Depth");
	d.motionVectors = make_resource(p.motion, FFX_RESOURCE_STATE_COMPUTE_READ, L"FSR3_MotionVectors");
	d.exposure = make_resource(nullptr, FFX_RESOURCE_STATE_COMPUTE_READ, L"FSR3_Exposure");
	d.reactive = make_resource(nullptr, FFX_RESOURCE_STATE_COMPUTE_READ, L"FSR3_Reactive");
	d.transparencyAndComposition = make_resource(nullptr, FFX_RESOURCE_STATE_COMPUTE_READ, L"FSR3_Transparency");
	d.dilatedDepth = make_resource(m_shared[0], FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"FSR3_DilatedDepth");
	d.dilatedMotionVectors = make_resource(m_shared[1], FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"FSR3_DilatedMotionVectors");
	d.reconstructedPrevNearestDepth = make_resource(m_shared[2], FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"FSR3_ReconstructedPrevNearestDepth");
	d.output = make_resource(p.output, FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"FSR3_Output");
	d.jitterOffset.x = p.jitter_x;
	d.jitterOffset.y = p.jitter_y;
	d.motionVectorScale.x = p.mv_scale_x;
	d.motionVectorScale.y = p.mv_scale_y;
	d.renderSize.width = p.render_w;
	d.renderSize.height = p.render_h;
	d.upscaleSize.width = p.output_w;
	d.upscaleSize.height = p.output_h;
	d.enableSharpening = p.sharpness > 0.f;
	d.sharpness = _min(p.sharpness, 1.f);
	d.frameTimeDelta = p.frame_time_ms;
	d.preExposure = 1.f;
	d.reset = p.reset;
	d.cameraNear = p.camera_near;
	d.cameraFar = p.camera_far;
	d.cameraFovAngleVertical = p.camera_fov_y;
	d.viewSpaceToMetersFactor = 1.f; // X-Ray units are meters
	d.flags = 0;

	const FfxErrorCode res = ffxFsr3UpscalerContextDispatch((FfxFsr3UpscalerContext*)m_context, &d);
	if (res != FFX_OK)
	{
		if (m_eval_failures++ < 8)
			Msg("! FSR: dispatch failed (0x%08x)", u32(res));
		return false;
	}
	return true;
}

#else // XR_FSR3_SHADERS

bool CFSR3::Init()
{
	Msg("! FSR: this build was compiled without the FSR shaders (compile_shaders.ps1)");
	m_initialized = true;
	m_available = false;
	return false;
}

void CFSR3::Shutdown()
{
	m_initialized = false;
	m_available = false;
}

bool CFSR3::CreateFeature(u32, u32, u32, u32, u32) { return false; }
void CFSR3::ReleaseFeature() {}
bool CFSR3::Evaluate(const UpscalerEvalParams&) { return false; }

#endif // XR_FSR3_SHADERS
