#define _CRT_SECURE_NO_WARNINGS
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>

#include "graphics-hook.h"
#include "../funchook.h"
#include "d3d9-patches.hpp"

typedef HRESULT (STDMETHODCALLTYPE *present_t)(IDirect3DDevice9*,
		CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*);
typedef HRESULT (STDMETHODCALLTYPE *present_ex_t)(IDirect3DDevice9*,
		CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *present_swap_t)(IDirect3DSwapChain9*,
		CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *reset_t)(IDirect3DDevice9*,
		D3DPRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE *reset_ex_t)(IDirect3DDevice9*,
		D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);

typedef HRESULT (WINAPI *createfactory1_t)(REFIID, void **);

static struct func_hook present      = { 0 };
static struct func_hook present_ex   = { 0 };
static struct func_hook present_swap = { 0 };
static struct func_hook reset        = { 0 };
static struct func_hook reset_ex     = { 0 };

struct d3d9_data {
	HMODULE                d3d9;
	IDirect3DDevice9       *device; /* do not release */
	uint32_t               cx;
	uint32_t               cy;
	D3DFORMAT              d3d9_format;
	DXGI_FORMAT            dxgi_format;
	bool                   using_shtex : 1;
	bool                   using_scale : 1;

	union {
		/* shared texture */
		struct {
			IDirect3DSurface9      *d3d9_copytex;
			ID3D11Device           *d3d11_device;
			ID3D11DeviceContext    *d3d11_context;
			ID3D11Resource         *d3d11_tex;
			struct shtex_data      *shtex_info;
			HANDLE                 handle;
			int                    patch;
		};
		/* shared memory */
		struct {
			IDirect3DSurface9      *copy_surfaces[NUM_BUFFERS];
			IDirect3DSurface9      *render_targets[NUM_BUFFERS];
			IDirect3DQuery9        *queries[NUM_BUFFERS];
			struct shmem_data      *shmem_info;
			bool                   texture_mapped[NUM_BUFFERS];
			uint32_t               pitch;
			int                    cur_tex;
			int                    copy_wait;
		};
	};
	volatile bool          issued_queries[NUM_BUFFERS];
	LUID luid_storage, *luid = nullptr;
};

static struct d3d9_data data = {};

static void d3d9_free()
{
	capture_free();

	if (data.using_shtex) {
		if (data.d3d11_tex)
			data.d3d11_tex->Release();
		if (data.d3d11_context)
			data.d3d11_context->Release();
		if (data.d3d11_device)
			data.d3d11_device->Release();
		if (data.d3d9_copytex)
			data.d3d9_copytex->Release();
	} else {
		for (size_t i = 0; i < NUM_BUFFERS; i++) {
			if (data.copy_surfaces[i]) {
				if (data.texture_mapped[i])
					data.copy_surfaces[i]->UnlockRect();
				data.copy_surfaces[i]->Release();
			}
			if (data.render_targets[i])
				data.render_targets[i]->Release();
			if (data.queries[i])
				data.queries[i]->Release();
		}
	}

	memset(&data, 0, sizeof(data));

	hlog("----------------- d3d9 capture freed -----------------");
}

static bool luid_matches(IDXGIAdapter *adapter)
{
	if (!global_hook_info->luid_valid)
		return true;

	DXGI_ADAPTER_DESC desc;
	HRESULT hr = adapter->GetDesc(&desc);
	if (FAILED(hr)) {
		hlog_hr("luid_matches: Failed to get adapter description", hr);
		return true;
	}

	if (desc.AdapterLuid.LowPart != global_hook_info->luid.LowPart ||
		desc.AdapterLuid.HighPart != global_hook_info->luid.HighPart)
		return false;

	data.luid_storage = desc.AdapterLuid;
	data.luid = &data.luid_storage;
	return true;
}

bool d3d9_luid(void *target)
{
	auto luid = reinterpret_cast<LUID*>(target);
	if (!luid || !data.luid)
		return false;

	*luid = *data.luid;
	return true;
}

static DXGI_FORMAT d3d9_to_dxgi_format(D3DFORMAT format)
{
	switch ((unsigned long)format) {
	case D3DFMT_A2B10G10R10: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case D3DFMT_A8R8G8B8:    return DXGI_FORMAT_B8G8R8A8_UNORM;
	case D3DFMT_X8R8G8B8:    return DXGI_FORMAT_B8G8R8X8_UNORM;
	}

	return DXGI_FORMAT_UNKNOWN;
}

const static D3D_FEATURE_LEVEL feature_levels[] =
{
	D3D_FEATURE_LEVEL_11_0,
	D3D_FEATURE_LEVEL_10_1,
	D3D_FEATURE_LEVEL_10_0,
	D3D_FEATURE_LEVEL_9_3,
};

static inline bool shex_init_d3d11()
{
	PFN_D3D11_CREATE_DEVICE create_device;
	createfactory1_t create_factory;
	D3D_FEATURE_LEVEL level_used;
	IDXGIFactory *factory;
	IDXGIAdapter *adapter;
	HMODULE d3d11;
	HMODULE dxgi;
	HRESULT hr;

	d3d11 = load_system_library("d3d11.dll");
	if (!d3d11) {
		hlog("d3d9_init: Failed to load D3D11");
		global_hook_info->force_shmem = true;
		return false;
	}

	dxgi = load_system_library("dxgi.dll");
	if (!dxgi) {
		hlog("d3d9_init: Failed to load DXGI");
		return false;
	}

	create_factory = (createfactory1_t)GetProcAddress(dxgi,
			"CreateDXGIFactory1");
	if (!create_factory) {
		hlog("d3d9_init: Failed to get CreateDXGIFactory1 address");
		return false;
	}

	create_device = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d3d11,
			"D3D11CreateDevice");
	if (!create_device) {
		hlog("d3d9_init: Failed to get D3D11CreateDevice address");
		return false;
	}

	hr = create_factory(__uuidof(IDXGIFactory1), (void**)&factory);
	if (FAILED(hr)) {
		hlog_hr("d3d9_init: Failed to create factory object", hr);
		return false;
	}

	hr = factory->EnumAdapters(0, &adapter);
	factory->Release();

	if (FAILED(hr)) {
		hlog_hr("d3d9_init: Failed to get adapter", hr);
		return false;
	}

	if (!luid_matches(adapter)) {
		hlog("d3d9_init: LUIDs didn't match, enabling shared memory capture");
		global_hook_info->force_shmem = true;
		return false;
	}

	hr = create_device(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
			0, feature_levels,
			sizeof(feature_levels) / sizeof(D3D_FEATURE_LEVEL),
			D3D11_SDK_VERSION, &data.d3d11_device, &level_used,
			&data.d3d11_context);
	adapter->Release();

	if (FAILED(hr)) {
		hlog_hr("d3d9_init: Failed to create D3D11 device", hr);
		return false;
	}

	return true;
}

static inline bool d3d9_shtex_init_shtex()
{
	IDXGIResource *res;
	HRESULT hr;

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width                = data.cx;
	desc.Height               = data.cy;
	desc.Format               = data.dxgi_format;
	desc.MipLevels            = 1;
	desc.ArraySize            = 1;
	desc.SampleDesc.Count     = 1;
	desc.Usage                = D3D11_USAGE_DEFAULT;
	desc.MiscFlags            = D3D11_RESOURCE_MISC_SHARED;
	desc.BindFlags            = D3D11_BIND_RENDER_TARGET |
	                            D3D11_BIND_SHADER_RESOURCE;

	hr = data.d3d11_device->CreateTexture2D(&desc, nullptr,
			(ID3D11Texture2D**)&data.d3d11_tex);
	if (FAILED(hr)) {
		hlog_hr("d3d9_shtex_init_shtex: Failed to create D3D11 texture",
				hr);
		return false;
	}

	hr = data.d3d11_tex->QueryInterface(__uuidof(IDXGIResource),
			(void**)&res);
	if (FAILED(hr)) {
		hlog_hr("d3d9_shtex_init_shtex: Failed to query IDXGIResource",
				hr);
		return false;
	}

	hr = res->GetSharedHandle(&data.handle);
	res->Release();

	if (FAILED(hr)) {
		hlog_hr("d3d9_shtex_init_shtex: Failed to get shared handle",
				hr);
		return false;
	}

	return true;
}

static bool d3d9_create_shared_tex(UINT width, UINT height, D3DFORMAT format, IDirect3DTexture9 **tex, HANDLE *shared_handle)
{
	if (!global_hook_info || !data.device) {
		hlog("d3d9_create_shared_tex: global_hook_info (%p) or data.device (%p) is NULL");
		return false;
	}

	struct d3d9_offsets offsets = global_hook_info->offsets.d3d9;
	uint8_t *patch_addr = nullptr;
	BOOL *p_is_d3d9 = nullptr;
	uint8_t saved_data[MAX_PATCH_SIZE];
	size_t patch_size = 0;
	BOOL was_d3d9ex = false;
	DWORD protect_val;
	HRESULT hr;

	if (offsets.d3d9_clsoff && offsets.is_d3d9ex_clsoff) {
		uint8_t *device_ptr = (uint8_t*)(data.device);
		uint8_t *d3d9_ptr =
			*(uint8_t**)(device_ptr + offsets.d3d9_clsoff);
		p_is_d3d9 = (BOOL*)(d3d9_ptr + offsets.is_d3d9ex_clsoff);
	} else {
		if (data.patch != -1 && !data.d3d9) {
			hlog("d3d9_create_shared_tex: data.d3d9 (%p) is NULL", data.d3d9);
			return false;
		}

		patch_addr = get_d3d9_patch_addr(data.d3d9, data.patch);
	}

	if (p_is_d3d9) {
		was_d3d9ex = *p_is_d3d9;
		*p_is_d3d9 = true;

	} else if (patch_addr) {
		patch_size = patch[data.patch].size;
		VirtualProtect(patch_addr, patch_size, PAGE_EXECUTE_READWRITE,
				&protect_val);
		memcpy(saved_data, patch_addr, patch_size);
		memcpy(patch_addr, patch[data.patch].data, patch_size);
	}

	hr = data.device->CreateTexture(width, height, 1,
			D3DUSAGE_RENDERTARGET, format,
			D3DPOOL_DEFAULT, tex, shared_handle);

	if (p_is_d3d9) {
		*p_is_d3d9 = was_d3d9ex;

	} else if (patch_addr && patch_size) {
		memcpy(patch_addr, saved_data, patch_size);
		VirtualProtect(patch_addr, patch_size, protect_val,
				&protect_val);
	}

	if (FAILED(hr)) {
		hlog_hr("d3d9_create_shared_tex: Failed to create shared texture",
				hr);
		return false;
	}

	return true;
}

bool d3d9_create_shared_tex_(UINT width, UINT height, DWORD format, void **tex, void **shared_handle)
{
	return d3d9_create_shared_tex(width, height, static_cast<D3DFORMAT>(format), reinterpret_cast<IDirect3DTexture9**>(tex), shared_handle);
}

static inline bool d3d9_shtex_init_copytex()
{
	IDirect3DTexture9 *tex;
	HRESULT hr;

	if (!d3d9_create_shared_tex(data.cx, data.cy, data.d3d9_format, &tex, &data.handle))
		return false;

	hr = tex->GetSurfaceLevel(0, &data.d3d9_copytex);
	tex->Release();

	if (FAILED(hr)) {
		hlog_hr("d3d9_shtex_init_copytex: Failed to get surface level", hr);
		return false;
	}

	return true;
}

static bool d3d9_shtex_init(uint32_t cx, uint32_t cy, HWND window)
{
	data.using_shtex = true;

	if (!shex_init_d3d11()) {
		return false;
	}
	if (!d3d9_shtex_init_shtex()) {
		return false;
	}
	if (!d3d9_shtex_init_copytex()) {
		return false;
	}
	if (!capture_init_shtex(&data.shtex_info, window, cx, cy,
				data.cx, data.cy, data.dxgi_format, false,
				(uintptr_t)data.handle)) {
		return false;
	}

	hlog("d3d9 shared texture capture successful");
	return true;
}

static bool d3d9_shmem_init_buffers(size_t buffer)
{
	HRESULT hr;

	hr = data.device->CreateOffscreenPlainSurface(data.cx, data.cy,
			data.d3d9_format, D3DPOOL_SYSTEMMEM,
			&data.copy_surfaces[buffer], nullptr);
	if (FAILED(hr)) {
		hlog_hr("d3d9_shmem_init_buffers: Failed to create surface",
				hr);
		return false;
	}

	if (buffer == 0) {
		D3DLOCKED_RECT rect;
		hr = data.copy_surfaces[buffer]->LockRect(&rect, nullptr,
				D3DLOCK_READONLY);
		if (FAILED(hr)) {
			hlog_hr("d3d9_shmem_init_buffers: Failed to lock "
			        "buffer", hr);
			return false;
		}

		data.pitch = rect.Pitch;
		data.copy_surfaces[buffer]->UnlockRect();
	}	

	hr = data.device->CreateRenderTarget(data.cx, data.cy,
			data.d3d9_format, D3DMULTISAMPLE_NONE, 0, false,
			&data.render_targets[buffer], nullptr);
	if (FAILED(hr)) {
		hlog_hr("d3d9_shmem_init_buffers: Failed to create render "
		        "target", hr);
		return false;
	}

	hr = data.device->CreateQuery(D3DQUERYTYPE_EVENT,
			&data.queries[buffer]);
	if (FAILED(hr)) {
		hlog_hr("d3d9_shmem_init_buffers: Failed to create query", hr);
		return false;
	}

	return true;
}

static bool d3d9_shmem_init(uint32_t cx, uint32_t cy, HWND window)
{
	data.using_shtex = false;

	static bool d3d9_shmem_init_buffers_logged = false;
	for (size_t i = 0; i < NUM_BUFFERS; i++) {
		if (!d3d9_shmem_init_buffers(i)) {
			if (!d3d9_shmem_init_buffers_logged) {
				hlog("d3d9_shmem_init: failed to init buffer %d", i);
				d3d9_shmem_init_buffers_logged = true;
			}
			return false;
		}
	}
	d3d9_shmem_init_buffers_logged = false;

	if (!capture_init_shmem(&data.shmem_info, window, cx, cy,
				data.cx, data.cy, data.pitch, data.dxgi_format,
				false)) {
		return false;
	}

	hlog("d3d9 memory capture successful");
	return true;
}

static bool d3d9_get_swap_desc(D3DPRESENT_PARAMETERS &pp,
		IDirect3DSwapChain9 *swap)
{
	HRESULT hr;
	bool release_swap = false;

	if (!swap) {
		hr = data.device->GetSwapChain(0, &swap);
		if (FAILED(hr)) {
			hlog_hr("d3d9_get_swap_desc: Failed to get swap chain", hr);
			return false;
		}
		release_swap = true;
	}

	hr = swap->GetPresentParameters(&pp);
	if (release_swap)
		swap->Release();

	if (FAILED(hr)) {
		hlog_hr("d3d9_get_swap_desc: Failed to get "
		        "presentation parameters", hr);
		return false;
	}

	return true;
}

static void set_size(uint32_t &cx, uint32_t &cy,
		uint32_t width, uint32_t height,
		const RECT *rect)
{
	if (rect) {
		cx = rect->right - rect->left;
		cy = rect->bottom - rect->top;

		if (cx >= 1 && cx <= width && cy >= 1 && cy <= height)
			return;
	}

	cx = width;
	cy = height;
}

static inline HRESULT get_backbuffer(IDirect3DDevice9 *device,
		IDirect3DSwapChain9 *swap,
		IDirect3DSurface9 **surface);

static bool d3d9_init_format_backbuffer(uint32_t &cx, uint32_t &cy,
		HWND &window,
		IDirect3DSwapChain9 *swap, HWND override_window,
		const RECT *src_rect)
{
	IDirect3DSurface9 *back_buffer = nullptr;
	D3DPRESENT_PARAMETERS pp;
	D3DSURFACE_DESC desc;
	HRESULT hr;

	if (!d3d9_get_swap_desc(pp, swap)) {
		return false;
	}

	hr = get_backbuffer(data.device, swap, &back_buffer);
	if (FAILED(hr)) {
		return false;
	}

	hr = back_buffer->GetDesc(&desc);
	back_buffer->Release();

	if (FAILED(hr)) {
		hlog_hr("d3d9_init_format_backbuffer: Failed to get "
		        "backbuffer descriptor", hr);
		return false;
	}

	data.d3d9_format = desc.Format;
	data.dxgi_format = d3d9_to_dxgi_format(desc.Format);
	data.using_scale = global_hook_info->use_scale;
	window = override_window ? override_window : pp.hDeviceWindow;
	set_size(cx, cy, desc.Width, desc.Height, src_rect);

	if (data.using_scale) {
		data.cx = global_hook_info->cx;
		data.cy = global_hook_info->cy;
	} else {
		data.cx = desc.Width;
		data.cy = desc.Height;
	}

	return true;
}

static bool d3d9_init_format_swapchain(uint32_t &cx, uint32_t &cy, HWND &window,
		IDirect3DSwapChain9 *swap, HWND override_window,
		const RECT *src_rect)
{
	D3DPRESENT_PARAMETERS pp;

	if (!d3d9_get_swap_desc(pp, swap)) {
		return false;
	}

	data.dxgi_format = d3d9_to_dxgi_format(pp.BackBufferFormat);
	data.d3d9_format = pp.BackBufferFormat;
	data.using_scale = global_hook_info->use_scale;
	window = override_window ? override_window :pp.hDeviceWindow;
	set_size(cx, cy, pp.BackBufferWidth, pp.BackBufferHeight, src_rect);

	if (data.using_scale) {
		data.cx = global_hook_info->cx;
		data.cy = global_hook_info->cy;
	} else {
		data.cx = pp.BackBufferWidth;
		data.cy = pp.BackBufferHeight;
	}

	return true;
}

static void d3d9_init(IDirect3DDevice9 *device,
		IDirect3DSwapChain9 *swap, HWND override_window,
		const RECT *src_rect)
{
	IDirect3DDevice9Ex *d3d9ex = nullptr;
	bool has_d3d9ex_bool_offset =
		global_hook_info->offsets.d3d9.d3d9_clsoff &&
		global_hook_info->offsets.d3d9.is_d3d9ex_clsoff;
	bool success;
	uint32_t cx = 0;
	uint32_t cy = 0;
	HWND window = nullptr;
	HRESULT hr;

	static bool d3d9_init_call_logged = false;
	if (!d3d9_init_call_logged) {
		hlog("d3d9_init called");
		d3d9_init_call_logged = true;
	}

	data.d3d9 = get_system_module("d3d9.dll");
	data.device = device;

	hr = device->QueryInterface(__uuidof(IDirect3DDevice9Ex),
			(void**)&d3d9ex);
	if (SUCCEEDED(hr)) {
		d3d9ex->Release();
		data.patch = -1;
	} else if (!has_d3d9ex_bool_offset) {
		data.patch = get_d3d9_patch(data.d3d9);
	} else {
		data.patch = -1;
	}

	static bool logged_init_format_failure = false;
	if (!d3d9_init_format_backbuffer(cx, cy, window, swap, override_window, src_rect)) {
		if (!d3d9_init_format_swapchain(cx, cy, window, swap, override_window, src_rect)) {
			if (!logged_init_format_failure) {
				hlog("d3d9_init: failed to init format");
				logged_init_format_failure = true;
			}
			return;
		}
	}
	logged_init_format_failure = false;

	if (global_hook_info->force_shmem ||
	    (!d3d9ex && data.patch == -1 && !has_d3d9ex_bool_offset) ||
		data.dxgi_format == DXGI_FORMAT_UNKNOWN) {
		success = d3d9_shmem_init(cx, cy, window);
	} else {
		success = d3d9_shtex_init(cx, cy, window);
	}

	if (!success)
		d3d9_free();
}

static inline HRESULT get_backbuffer(IDirect3DDevice9 *device,
		IDirect3DSwapChain9 *swap,
		IDirect3DSurface9 **surface)
{
	static bool use_backbuffer = false;
	static bool checked_exceptions = false;

	if (!checked_exceptions) {
		if (_strcmpi(get_process_name(), "hotd_ng.exe") == 0 ||
			_strcmpi(get_process_name(), "crosscode-beta.exe") == 0)
			use_backbuffer = true;
		checked_exceptions = true;
	}

	if (use_backbuffer) {
		return swap ?
				swap->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, surface) :
				device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO,
					surface);
	} else {
		return device->GetRenderTarget(0, surface);
	}
}

static inline void d3d9_shtex_capture(IDirect3DSurface9 *backbuffer)
{
	D3DTEXTUREFILTERTYPE filter;
	HRESULT hr;

	filter = data.using_scale ? D3DTEXF_LINEAR : D3DTEXF_NONE;

	hr = data.device->StretchRect(backbuffer, nullptr, data.d3d9_copytex,
			nullptr, filter);
	if (FAILED(hr))
		hlog_hr("d3d9_shtex_capture: StretchRect failed", hr);
}

static inline void d3d9_shmem_capture_queue_copy()
{
	for (int i = 0; i < NUM_BUFFERS; i++) {
		IDirect3DSurface9 *target = data.copy_surfaces[i];
		D3DLOCKED_RECT rect;
		HRESULT hr;

		if (!data.issued_queries[i]) {
			continue;
		}
		if (data.queries[i]->GetData(0, 0, 0) != S_OK) {
			continue;
		}

		data.issued_queries[i] = false;

		hr = target->LockRect(&rect, nullptr, D3DLOCK_READONLY);
		if (SUCCEEDED(hr)) {
			data.texture_mapped[i] = true;
			shmem_copy_data(i, rect.pBits);
		}
		break;
	}
}

static inline void d3d9_shmem_capture(IDirect3DSurface9 *backbuffer)
{
	D3DTEXTUREFILTERTYPE filter;
	IDirect3DSurface9 *copy;
	int next_tex;
	HRESULT hr;

	d3d9_shmem_capture_queue_copy();

	next_tex = (data.cur_tex == NUM_BUFFERS - 1) ?  0 : data.cur_tex + 1;
	filter = data.using_scale ? D3DTEXF_LINEAR : D3DTEXF_NONE;
	copy = data.render_targets[data.cur_tex];

	hr = data.device->StretchRect(backbuffer, nullptr, copy, nullptr,
			filter);

	if (FAILED(hr)) {
		hlog_hr("d3d9_shmem_capture: StretchRect failed", hr);
		return;
	}

	if (data.copy_wait < NUM_BUFFERS - 1) {
		data.copy_wait++;
	} else {
		IDirect3DSurface9 *src = data.render_targets[next_tex];
		IDirect3DSurface9 *dst = data.copy_surfaces[next_tex];

		if (shmem_texture_data_lock(next_tex)) {
			dst->UnlockRect();
			data.texture_mapped[next_tex] = false;
			shmem_texture_data_unlock(next_tex);
		}

		hr = data.device->GetRenderTargetData(src, dst);
		if (FAILED(hr)) {
			hlog_hr("d3d9_shmem_capture: GetRenderTargetData "
			        "failed", hr);
		}

		data.queries[next_tex]->Issue(D3DISSUE_END);
		data.issued_queries[next_tex] = true;
	}

	data.cur_tex = next_tex;
}

static void d3d9_capture(IDirect3DDevice9 *device,
		IDirect3DSurface9 *backbuffer,
		IDirect3DSwapChain9 *swap, HWND override_window,
		const RECT *src_rect)
{
	static bool d3d9_capture_call_logged = false;
	if (!d3d9_capture_call_logged) {
		hlog("d3d9_capture called");
		d3d9_capture_call_logged = true;
	}

	if (capture_should_stop()) {
		d3d9_free();
	}
	if (capture_should_init()) {
		d3d9_init(device, swap, override_window, src_rect);
	}
	if (capture_ready()) {
		if (data.device != device) {
			d3d9_free();
			return;
		}

		if (data.using_shtex)
			d3d9_shtex_capture(backbuffer);
		else
			d3d9_shmem_capture(backbuffer);
	}
}

/* this is used just in case Present calls PresentEx or vise versa. */
static int present_recurse = 0;

static bool present_begin_called = false;
static inline void present_begin(IDirect3DDevice9 *device,
		IDirect3DSurface9 *&backbuffer,
		IDirect3DSwapChain9 *swap, HWND override_window,
		const RECT *src_rect)
{
	HRESULT hr;

	if (!present_begin_called) {
		hlog("present_begin called");
		present_begin_called = true;
	}

	if (!present_recurse) {
		hr = get_backbuffer(device, swap, &backbuffer);
		if (FAILED(hr)) {
			hlog_hr("d3d9_shmem_capture: Failed to get "
				"backbuffer", hr);
		}

		if (!global_hook_info->capture_overlay) {
			d3d9_capture(device, backbuffer, swap, override_window, src_rect);
		}

		if (overlay_info.draw_d3d9)
			overlay_info.draw_d3d9(static_cast<void*>(device), static_cast<void*>(backbuffer),
				static_cast<void*>(swap), override_window, src_rect);

	}

	present_recurse++;
}

static inline void present_end(IDirect3DDevice9 *device,
		IDirect3DSurface9 *backbuffer,
		IDirect3DSwapChain9 *swap, HWND override_window,
		const RECT *src_rect)
{
	present_recurse--;

	if (!present_recurse) {
		if (global_hook_info->capture_overlay) {
			if (!present_recurse)
				d3d9_capture(device, backbuffer, swap, override_window, src_rect);
		}

		if (backbuffer)
			backbuffer->Release();
	}
}

static bool hooked_reset = false;
static void setup_reset_hooks(IDirect3DDevice9 *device);

static HRESULT STDMETHODCALLTYPE hook_present(IDirect3DDevice9 *device,
		CONST RECT *src_rect, CONST RECT *dst_rect,
		HWND override_window, CONST RGNDATA *dirty_region)
{
	IDirect3DSurface9 *backbuffer = nullptr;
	HRESULT hr;

	if (!hooked_reset)
		setup_reset_hooks(device);

	present_begin(device, backbuffer, nullptr, override_window, src_rect);

	unhook(&present);
	present_t call = (present_t)present.call_addr;
	hr = call(device, src_rect, dst_rect, override_window, dirty_region);
	rehook(&present);

	present_end(device, backbuffer, nullptr, override_window, src_rect);

	return hr;
}

static HRESULT STDMETHODCALLTYPE hook_present_ex(IDirect3DDevice9 *device,
		CONST RECT *src_rect, CONST RECT *dst_rect,
		HWND override_window, CONST RGNDATA *dirty_region, DWORD flags)
{
	IDirect3DSurface9 *backbuffer = nullptr;
	HRESULT hr;

	if (!hooked_reset)
		setup_reset_hooks(device);

	present_begin(device, backbuffer, nullptr, override_window, src_rect);

	unhook(&present_ex);
	present_ex_t call = (present_ex_t)present_ex.call_addr;
	hr = call(device, src_rect, dst_rect, override_window, dirty_region,
			flags);
	rehook(&present_ex);

	present_end(device, backbuffer, nullptr, override_window, src_rect);

	return hr;
}

static HRESULT STDMETHODCALLTYPE hook_present_swap(IDirect3DSwapChain9 *swap,
		CONST RECT *src_rect, CONST RECT *dst_rect,
		HWND override_window, CONST RGNDATA *dirty_region, DWORD flags)
{
	IDirect3DSurface9 *backbuffer = nullptr;
	IDirect3DDevice9 *device = nullptr;
	HRESULT hr;

	if (!present_recurse) {
		hr = swap->GetDevice(&device);
		if (SUCCEEDED(hr)) {
			device->Release();
		}
	}

	if (device) {
		if (!hooked_reset)
			setup_reset_hooks(device);

		present_begin(device, backbuffer, swap, override_window, src_rect);
	}

	unhook(&present_swap);
	present_swap_t call = (present_swap_t)present_swap.call_addr;
	hr = call(swap, src_rect, dst_rect, override_window, dirty_region,
			flags);
	rehook(&present_swap);

	if (device)
		present_end(device, backbuffer, swap, override_window, src_rect);

	return hr;
}

static HRESULT STDMETHODCALLTYPE hook_reset(IDirect3DDevice9 *device,
		D3DPRESENT_PARAMETERS *params)
{
	HRESULT hr;

	if (capture_active())
		d3d9_free();

	if (overlay_info.reset)
		overlay_info.reset();

	unhook(&reset);
	reset_t call = (reset_t)reset.call_addr;
	hr = call(device, params);
	rehook(&reset);

	return hr;
}

static HRESULT STDMETHODCALLTYPE hook_reset_ex(IDirect3DDevice9 *device,
		D3DPRESENT_PARAMETERS *params, D3DDISPLAYMODEEX *dmex)
{
	HRESULT hr;

	if (capture_active())
		d3d9_free();

	if (overlay_info.reset)
		overlay_info.reset();

	unhook(&reset_ex);
	reset_ex_t call = (reset_ex_t)reset_ex.call_addr;
	hr = call(device, params, dmex);
	rehook(&reset_ex);

	return hr;
}

static void setup_reset_hooks(IDirect3DDevice9 *device)
{
	IDirect3DDevice9Ex *d3d9ex = nullptr;
	uintptr_t *vtable = *(uintptr_t**)device;
	HRESULT hr;

	hook_init(&reset, (void*)vtable[16], (void*)hook_reset,
			"IDirect3DDevice9::Reset");
	rehook(&reset);

	hr = device->QueryInterface(__uuidof(IDirect3DDevice9Ex),
			(void**)&d3d9ex);
	if (SUCCEEDED(hr)) {
		hook_init(&reset_ex, (void*)vtable[132], (void*)hook_reset_ex,
				"IDirect3DDevice9Ex::ResetEx");
		rehook(&reset_ex);

		d3d9ex->Release();
	}

	apply_hooks();

	hooked_reset = true;
}

typedef HRESULT (WINAPI *d3d9create_ex_t)(UINT, IDirect3D9Ex**);

static bool manually_get_d3d9_addrs(HMODULE d3d9_module,
		void **present_addr,
		void **present_ex_addr,
		void **present_swap_addr)
{
	d3d9create_ex_t create_ex;
	D3DPRESENT_PARAMETERS pp;
	HRESULT hr;

	IDirect3DDevice9Ex *device;
	IDirect3D9Ex *d3d9ex;

	hlog("D3D9 values invalid, manually obtaining");

	create_ex = (d3d9create_ex_t)GetProcAddress(d3d9_module,
			"Direct3DCreate9Ex");
	if (!create_ex) {
		hlog("Failed to load Direct3DCreate9Ex");
		return false;
	}
	if (FAILED(create_ex(D3D_SDK_VERSION, &d3d9ex))) {
		hlog("Failed to create D3D9 context");
		return false;
	}

	memset(&pp, 0, sizeof(pp));
	pp.Windowed                 = 1;
	pp.SwapEffect               = D3DSWAPEFFECT_FLIP;
	pp.BackBufferFormat         = D3DFMT_A8R8G8B8;
	pp.BackBufferCount          = 1;
	pp.hDeviceWindow            = (HWND)dummy_window;
	pp.PresentationInterval     = D3DPRESENT_INTERVAL_IMMEDIATE;

	hr = d3d9ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
			dummy_window,
			D3DCREATE_HARDWARE_VERTEXPROCESSING |
			D3DCREATE_NOWINDOWCHANGES, &pp, NULL, &device);
	d3d9ex->Release();

	if (SUCCEEDED(hr)) {
		uintptr_t *vtable = *(uintptr_t**)device;
		IDirect3DSwapChain9 *swap;

		*present_addr = (void*)vtable[17];
		*present_ex_addr = (void*)vtable[121];

		hr = device->GetSwapChain(0, &swap);
		if (SUCCEEDED(hr)) {
			vtable = *(uintptr_t**)swap;
			*present_swap_addr = (void*)vtable[3];

			swap->Release();
		}

		device->Release();
	} else {
		hlog("Failed to create D3D9 device");
		return false;
	}

	return true;
}

static wchar_t module_path[1024] = { 0 };
static void log_d3d9_module_path(HMODULE loaded_d3d9_module)
{
	UINT length = GetModuleFileNameEx(GetCurrentProcess(), loaded_d3d9_module, module_path, sizeof(module_path) / sizeof(module_path[0]));
	if (!length || length > sizeof(module_path) / sizeof(module_path[0])) {
		hlog("log_d3d9_module_path: GetModuleFileNameEx failed (%lu <-> %lu): %#x",
			length, length > sizeof(module_path) / sizeof(module_path[0]),
			GetLastError());
		return;
	}

	hlog("log_d3d9_module_path: %ls", module_path);
}

static uint64_t hook_init_time = 0;

static HMODULE hooked_d3d9_module = nullptr;
static bool d3d9_module_mismatch_logged = false;
bool hook_d3d9(void)
{
	HMODULE d3d9_module = get_locked_system_module("d3d9.dll");
	uint32_t d3d9_size;
	void *present_addr = nullptr;
	void *present_ex_addr = nullptr;
	void *present_swap_addr = nullptr;
	HMODULE loaded_d3d9_module = GetModuleHandleA("d3d9.dll");

	if (!d3d9_module) {
		if (loaded_d3d9_module && !d3d9_module_mismatch_logged) {
			hlog("Non-system d3d9 module loaded: %p", loaded_d3d9_module);
			log_d3d9_module_path(loaded_d3d9_module);
			d3d9_module_mismatch_logged = true;
		}
		return false;
	}

	release_module module_releaser{ d3d9_module };

	d3d9_size = module_size(d3d9_module);

	if (global_hook_info->offsets.d3d9.present      < d3d9_size &&
	    global_hook_info->offsets.d3d9.present_ex   < d3d9_size &&
	    global_hook_info->offsets.d3d9.present_swap < d3d9_size) {

		present_addr = get_offset_addr(d3d9_module,
				global_hook_info->offsets.d3d9.present);
		present_ex_addr = get_offset_addr(d3d9_module,
				global_hook_info->offsets.d3d9.present_ex);
		present_swap_addr = get_offset_addr(d3d9_module,
				global_hook_info->offsets.d3d9.present_swap);
	} else {
		static bool d3d9_size_mismatch_logged = false;
		if (!d3d9_size_mismatch_logged) {
			hlog("D3D9 offsets are outside of module size boundaries (%#x):"
				"\n\tpresent:      %#x"
				"\n\tpresent_ex:   %#x"
				"\n\tpresent_swap: %#x",
				d3d9_size,
				global_hook_info->offsets.d3d9.present,
				global_hook_info->offsets.d3d9.present_ex,
				global_hook_info->offsets.d3d9.present_swap);
			d3d9_size_mismatch_logged = true;
		}

		if (!dummy_window) {
			return false;
		}

		if (!manually_get_d3d9_addrs(d3d9_module,
					&present_addr,
					&present_ex_addr,
					&present_swap_addr)) {
			hlog("Failed to get D3D9 values");
			return true;
		}
	}

	if (!present_addr && !present_ex_addr && !present_swap_addr) {
		hlog("Invalid D3D9 values");
		return true;
	}

	if (present_swap_addr) {
		hook_init(&present_swap, present_swap_addr,
				(void*)hook_present_swap,
				"IDirect3DSwapChain9::Present");
		rehook(&present_swap);
	}
	if (present_ex_addr) {
		hook_init(&present_ex, present_ex_addr,
				(void*)hook_present_ex,
				"IDirect3DDevice9Ex::PresentEx");
		rehook(&present_ex);
	}
	if (present_addr) {
		hook_init(&present, present_addr,
				(void*)hook_present,
				"IDirect3DDevice9::Present");
		rehook(&present);
	}

	apply_hooks();

	hook_init_time = os_gettime_ns();

	hooked_d3d9_module = d3d9_module;
	hlog("Hooked D3D9");
	return true;
}

bool check_d3d9()
{
	if (present_begin_called)
		return true;

	if (!hooked_d3d9_module)
		return true;

	HMODULE d3d9_module = get_locked_system_module("d3d9.dll");
	if (!d3d9_module) {
		static bool d3d9_unload_logged = false;
		if (!d3d9_unload_logged) {
			hlog("d3d9.dll unloaded after it was hooked");
			d3d9_unload_logged = true;
		}
		return true;
	}

	release_module module_releaser{ d3d9_module };

	if (d3d9_module != hooked_d3d9_module) {
		static bool d3d9_base_moved_logged = false;
		if (!d3d9_base_moved_logged) {
			hlog("d3d9.dll reloaded after it was hooked");
			d3d9_base_moved_logged = true;
		}
		return true;
	}

#if USE_MINHOOK
	uint64_t cur = os_gettime_ns();
	if (hook_init_time < cur && (cur - hook_init_time) > HOOK_CHECK_TIME_NS)
		return false;
#endif

	return check_hook(&present_swap) ||
		check_hook(&present_ex) ||
		check_hook(&present);
}
