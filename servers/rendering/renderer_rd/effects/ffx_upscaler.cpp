/**************************************************************************/
/*  ffx_upscaler.cpp                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#ifdef FFX_UPSCALER_D3D12_ENABLED

#include "ffx_upscaler.h"

#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/string/print_string.h"

#include "drivers/d3d12/rendering_device_driver_d3d12.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Vendored MIT headers (thirdparty/amd_ffx_api); the implementation is AMD's
// signed DLLs, loaded below - never linked, never committed.
#include "ffx_api.h"
#include "ffx_upscale.h"
#include "dx12/ffx_api_dx12.h"

using namespace RendererRD;

/////////////////////////////////////////////////
// Loader: amd_fidelityfx_loader_dx12.dll, resolved once.

namespace {

struct FfxLib {
	HMODULE module = nullptr;
	PfnFfxCreateContext create_context = nullptr;
	PfnFfxDestroyContext destroy_context = nullptr;
	PfnFfxConfigure configure = nullptr;
	PfnFfxQuery query = nullptr;
	PfnFfxDispatch dispatch = nullptr;
	bool tried = false;

	bool loaded() const { return dispatch != nullptr; }
};

FfxLib ffx_lib;
BinaryMutex ffx_lib_mutex;

FfxLib &_ffx_ensure_loaded() {
	MutexLock lock(ffx_lib_mutex);
	if (ffx_lib.tried) {
		return ffx_lib;
	}
	ffx_lib.tried = true;

	// The signed loader ships beside the exe; restrict the search to the
	// application directory (and the DLL's own directory for its provider
	// DLLs) so a planted DLL elsewhere can't be picked up.
	ffx_lib.module = LoadLibraryExW(L"amd_fidelityfx_loader_dx12.dll", nullptr,
			LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
	if (ffx_lib.module == nullptr) {
		DWORD err = GetLastError();
		if (err == ERROR_MOD_NOT_FOUND) {
			// DLL simply not shipped - the normal case on installs without it.
			print_verbose("FFX upscaler: amd_fidelityfx_loader_dx12.dll not found; FSR 3/4 unavailable.");
		} else {
			// Present but unloadable - always worth a visible line.
			print_line(vformat("FFX upscaler: amd_fidelityfx_loader_dx12.dll failed to load (Win32 error %d); FSR 3/4 unavailable.", (uint64_t)err));
		}
		return ffx_lib;
	}
	print_verbose("FFX upscaler: loader DLL loaded.");

	ffx_lib.create_context = (PfnFfxCreateContext)(void *)GetProcAddress(ffx_lib.module, "ffxCreateContext");
	ffx_lib.destroy_context = (PfnFfxDestroyContext)(void *)GetProcAddress(ffx_lib.module, "ffxDestroyContext");
	ffx_lib.configure = (PfnFfxConfigure)(void *)GetProcAddress(ffx_lib.module, "ffxConfigure");
	ffx_lib.query = (PfnFfxQuery)(void *)GetProcAddress(ffx_lib.module, "ffxQuery");
	ffx_lib.dispatch = (PfnFfxDispatch)(void *)GetProcAddress(ffx_lib.module, "ffxDispatch");

	if (!ffx_lib.create_context || !ffx_lib.destroy_context || !ffx_lib.query || !ffx_lib.dispatch) {
		print_line("FFX upscaler: loader DLL is missing entry points; FSR 3/4 unavailable.");
		ffx_lib = FfxLib();
		ffx_lib.tried = true;
	}
	return ffx_lib;
}

// Phase 3 hardware-tuning knobs, mirroring the DLSS ones (see dlss_ngx.cpp):
//   RW_FSR3_JITTER_SIGN_X / _Y = -1   flip the jitter offset sign per axis
//   RW_FSR3_MV_SIGN_X / _Y     = -1   flip the motion-vector scale per axis
struct FfxKnobs {
	float jitter_sign_x = 1.0f;
	float jitter_sign_y = 1.0f;
	float mv_sign_x = 1.0f;
	float mv_sign_y = 1.0f;
	bool loaded = false;
};

FfxKnobs ffx_knobs;

const FfxKnobs &_ffx_get_knobs() {
	if (!ffx_knobs.loaded) {
		OS *os = OS::get_singleton();
		ffx_knobs.jitter_sign_x = (os->get_environment("RW_FSR3_JITTER_SIGN_X") == "-1") ? -1.0f : 1.0f;
		ffx_knobs.jitter_sign_y = (os->get_environment("RW_FSR3_JITTER_SIGN_Y") == "-1") ? -1.0f : 1.0f;
		ffx_knobs.mv_sign_x = (os->get_environment("RW_FSR3_MV_SIGN_X") == "-1") ? -1.0f : 1.0f;
		ffx_knobs.mv_sign_y = (os->get_environment("RW_FSR3_MV_SIGN_Y") == "-1") ? -1.0f : 1.0f;
		if (ffx_knobs.jitter_sign_x < 0 || ffx_knobs.jitter_sign_y < 0 || ffx_knobs.mv_sign_x < 0 || ffx_knobs.mv_sign_y < 0) {
			print_line(vformat("FFX upscaler: tuning knobs active - jitter sign (%d,%d), MV sign (%d,%d).",
					(int)ffx_knobs.jitter_sign_x, (int)ffx_knobs.jitter_sign_y, (int)ffx_knobs.mv_sign_x, (int)ffx_knobs.mv_sign_y));
		}
		ffx_knobs.loaded = true;
	}
	return ffx_knobs;
}

void _ffx_message(uint32_t p_type, const wchar_t *p_message) {
	if (p_type == FFX_API_MESSAGE_TYPE_ERROR) {
		ERR_PRINT(vformat("FFX upscaler: %s", String(p_message)));
	} else {
		WARN_PRINT(vformat("FFX upscaler: %s", String(p_message)));
	}
}

// Shared by the availability probe (trial context) and real context creation.
ffxReturnCode_t _ffx_create_raw(ID3D12Device *p_device, Size2i p_internal_size, Size2i p_target_size, ffxContext *r_context) {
	// Same conventions Godot's FSR2 integration declares: linear HDR input,
	// reverse-Z depth. MVs are low-res (no display-res flag needed).
	ffxCreateContextDescUpscale desc = {};
	desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
	desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
	desc.maxRenderSize = { (uint32_t)p_internal_size.width, (uint32_t)p_internal_size.height };
	desc.maxUpscaleSize = { (uint32_t)p_target_size.width, (uint32_t)p_target_size.height };
	desc.fpMessage = _ffx_message;

	ffxCreateBackendDX12Desc backend = {};
	backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
	backend.device = p_device;
	desc.header.pNext = &backend.header;

	// Declare which API revision these structs were compiled against.
	ffxCreateContextDescUpscaleVersion api_version = {};
	api_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
	api_version.version = FFX_UPSCALER_VERSION;
	backend.header.pNext = &api_version.header;

	return ffx_lib.create_context(r_context, &desc.header, nullptr);
}

String _ffx_provider_name(ffxContext p_context) {
	ffxQueryGetProviderVersion provider = {};
	provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
	if (ffx_lib.query(&p_context, &provider.header) == FFX_API_RETURN_OK && provider.versionName != nullptr) {
		return String(provider.versionName);
	}
	return "unknown";
}

} // namespace

/////////////////////////////////////////////////
// FfxUpscalerContext

FfxUpscalerContext::~FfxUpscalerContext() {
	if (ffx_context != nullptr && ffx_lib.loaded()) {
		// NOTE: destroys the provider's internal GPU resources immediately.
		// Contexts are destroyed on render-buffer teardown (resize/close),
		// mirroring FSR2/MetalFX context lifetime.
		ffxContext ctx = ffx_context;
		ffx_lib.destroy_context(&ctx, nullptr);
		ffx_context = nullptr;
	}
}

/////////////////////////////////////////////////
// FfxUpscalerEffect

FfxUpscalerEffect::FfxUpscalerEffect() {}

FfxUpscalerEffect::~FfxUpscalerEffect() {}

bool FfxUpscalerEffect::is_available(void *p_d3d12_device) {
	if (p_d3d12_device == nullptr) {
		return false;
	}
	FfxLib &lib = _ffx_ensure_loaded();
	if (!lib.loaded()) {
		return false;
	}

	// Ground-truth probe: create (and immediately destroy) a small trial
	// context. This exercises the loader's provider selection for real -
	// version-count queries proved unreliable as a gate. One-time; the
	// result is cached by the driver's has_feature caller pattern (the
	// catalog only asks once) and the cost is trivial.
	static int cached = -1;
	if (cached != -1) {
		return cached == 1;
	}

	ffxContext trial = nullptr;
	ffxReturnCode_t rc = _ffx_create_raw((ID3D12Device *)p_d3d12_device, Size2i(640, 360), Size2i(1280, 720), &trial);
	if (rc != FFX_API_RETURN_OK || trial == nullptr) {
		print_line(vformat("FFX upscaler: no upscale provider for this device (ffxCreateContext rc %d); FSR 3/4 unavailable.", (int)rc));
		cached = 0;
		return false;
	}
	print_line(vformat("FFX upscaler: available, provider '%s'.", _ffx_provider_name(trial)));
	lib.destroy_context(&trial, nullptr);
	cached = 1;
	return true;
}

FfxUpscalerContext *FfxUpscalerEffect::create_context(Size2i p_internal_size, Size2i p_target_size) {
	FfxLib &lib = _ffx_ensure_loaded();
	ERR_FAIL_COND_V(!lib.loaded(), nullptr);

	ID3D12Device *device = (ID3D12Device *)RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE, RID());
	ERR_FAIL_NULL_V(device, nullptr);

	ffxContext ffx_context = nullptr;
	ffxReturnCode_t rc = _ffx_create_raw(device, p_internal_size, p_target_size, &ffx_context);
	ERR_FAIL_COND_V_MSG(rc != FFX_API_RETURN_OK || ffx_context == nullptr, nullptr, vformat("FFX upscaler: ffxCreateContext failed (%d).", (int)rc));

	// Which provider did we actually get? RDNA3/4 resolve to FSR 4.x, other
	// GPUs to FSR 3.1.x - worth surfacing in the logs.
	print_line(vformat("FFX upscaler: using provider '%s'.", _ffx_provider_name(ffx_context)));

	FfxUpscalerContext *context = memnew(FfxUpscalerContext);
	context->ffx_context = ffx_context;
	context->internal_size = p_internal_size;
	context->target_size = p_target_size;
	return context;
}

void FfxUpscalerEffect::upscale(const Parameters &p_params) {
	ERR_FAIL_NULL(p_params.context);
	ERR_FAIL_COND(!ffx_lib.loaded());

	RD *rd = RD::get_singleton();

	CallbackArgs *userdata = args_allocator.alloc();
	userdata->owner = this;
	userdata->ctx = p_params.context;
	userdata->color = (void *)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_params.color);
	userdata->depth = (void *)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_params.depth);
	userdata->velocity = (void *)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_params.velocity);
	userdata->exposure = p_params.exposure.is_valid() ? (void *)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_params.exposure) : nullptr;
	userdata->output = (void *)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_params.output);
	userdata->internal_size = p_params.internal_size;
	userdata->target_size = p_params.context->target_size;
	userdata->jitter = p_params.jitter;
	userdata->sharpness = p_params.sharpness;
	userdata->delta_time_ms = p_params.delta_time * 1000.0f; // ffx-api wants milliseconds.
	userdata->z_near = p_params.z_near;
	userdata->z_far = p_params.z_far;
	userdata->fovy = p_params.fovy;
	userdata->reset = p_params.reset_accumulation;

	// Declared usages make the render graph transition every resource before
	// the callback runs; the states below must match what these usages map to.
	// (No designated initializers: MSVC builds Godot as C++17.)
	RD::CallbackResource res[5];
	res[0].rid = p_params.color;
	res[0].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	res[1].rid = p_params.depth;
	res[1].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	res[2].rid = p_params.velocity;
	res[2].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	res[3].rid = p_params.output;
	res[3].usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE;
	res[4].rid = p_params.exposure;
	res[4].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	uint32_t res_count = p_params.exposure.is_valid() ? 5 : 4;
	rd->driver_callback_add((RDD::DriverCallback)FfxUpscalerEffect::callback, userdata, VectorView<RD::CallbackResource>(res, res_count));
}

void FfxUpscalerEffect::callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata) {
	RenderingDeviceDriverD3D12 *driver = (RenderingDeviceDriverD3D12 *)p_driver;
	ID3D12GraphicsCommandList *cmd_list = driver->command_buffer_get_native_list(p_command_buffer);

	// The graph transitioned sampled inputs to shader-resource and the output
	// to UAV (see the usages declared in upscale()).
	ffxDispatchDescUpscale dispatch = {};
	dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
	dispatch.commandList = cmd_list;
	dispatch.color = ffxApiGetResourceDX12((ID3D12Resource *)p_userdata->color, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
	dispatch.depth = ffxApiGetResourceDX12((ID3D12Resource *)p_userdata->depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
	dispatch.motionVectors = ffxApiGetResourceDX12((ID3D12Resource *)p_userdata->velocity, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
	if (p_userdata->exposure != nullptr) {
		dispatch.exposure = ffxApiGetResourceDX12((ID3D12Resource *)p_userdata->exposure, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
	}
	// Reactive/T&C masks omitted: Godot's "reactive" texture is an
	// alpha-swizzled *view* of the color texture, which native SDKs can't
	// consume (they'd read the R channel). Optional inputs; quality item
	// for Phase 3.
	dispatch.output = ffxApiGetResourceDX12((ID3D12Resource *)p_userdata->output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
	const FfxKnobs &k = _ffx_get_knobs();
	dispatch.jitterOffset = { p_userdata->jitter.x * k.jitter_sign_x, p_userdata->jitter.y * k.jitter_sign_y };
	dispatch.motionVectorScale = { float(p_userdata->internal_size.width) * k.mv_sign_x, float(p_userdata->internal_size.height) * k.mv_sign_y };
	dispatch.renderSize = { (uint32_t)p_userdata->internal_size.width, (uint32_t)p_userdata->internal_size.height };
	dispatch.upscaleSize = { (uint32_t)p_userdata->target_size.width, (uint32_t)p_userdata->target_size.height };
	dispatch.enableSharpening = p_userdata->sharpness > 1e-6f;
	dispatch.sharpness = p_userdata->sharpness;
	dispatch.frameTimeDelta = p_userdata->delta_time_ms;
	dispatch.preExposure = 1.0f;
	dispatch.reset = p_userdata->reset;
	dispatch.cameraNear = p_userdata->z_near;
	dispatch.cameraFar = p_userdata->z_far;
	dispatch.cameraFovAngleVertical = p_userdata->fovy;
	dispatch.viewSpaceToMetersFactor = 1.0f;
	dispatch.flags = 0;

	ffxContext ctx = p_userdata->ctx->ffx_context;
	ffxReturnCode_t rc = ffx_lib.dispatch(&ctx, &dispatch.header);
	if (rc != FFX_API_RETURN_OK) {
		ERR_PRINT_ONCE(vformat("FFX upscaler: ffxDispatch failed (%d).", (int)rc));
	}

	// The provider changed descriptor heaps / PSO / root signature behind
	// Godot's back; make the driver re-bind its own state lazily.
	driver->command_buffer_mark_external_commands(p_command_buffer);

	CallbackArgs::free(&p_userdata);
}

#endif // FFX_UPSCALER_D3D12_ENABLED
