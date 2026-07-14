/**************************************************************************/
/*  dlss_ngx.cpp                                                          */
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

#ifdef DLSS_D3D12_ENABLED

#include "dlss_ngx.h"

#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/string/print_string.h"

#include "drivers/d3d12/rendering_device_driver_d3d12.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// NGX SDK headers (github.com/NVIDIA/DLSS, via the DLSS_SDK build-time env
// var - proprietary, never vendored into this repo). Links nvsdk_ngx_s.lib;
// the DLSS runtime itself is nvngx_dlss.dll beside the exe.
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_helpers.h>

using namespace RendererRD;

/////////////////////////////////////////////////
// Process-wide NGX state.

namespace {

// Identifies RaceWars to NGX; NVIDIA's SW notification (Phase 4 licensing
// checklist) references this same project id.
const char *NGX_PROJECT_ID = "5a2b3c14-9d7e-4f80-b1a6-cc35d78ae214";
const char *NGX_ENGINE_VERSION = "4.6.3-racewars";

struct NgxState {
	BinaryMutex mutex;
	bool tried = false;
	bool available = false;
	bool initialized = false;
	ID3D12Device *device = nullptr;
	NVSDK_NGX_Parameter *capability_params = nullptr;
	Char16String data_path; // Backing store for the wchar_t* NGX keeps.
};

NgxState ngx;

// Phase 3 hardware-tuning knobs, re-read live every dispatch so the game's
// debug overlay can flip them at runtime through OS.set_environment (the
// zero-API channel into the fork). SDK sign/flag conventions can only be
// settled by A/B on real GPUs; once the right combination is confirmed these
// become the defaults and the knobs go.
//   RW_DLSS_JITTER_SIGN_X / _Y  = -1   flip the jitter offset sign per axis
//   RW_DLSS_MV_SIGN_X / _Y      = -1   flip the motion-vector scale per axis
//   RW_DLSS_MV_JITTERED         = 1    tell DLSS the MVs contain jitter
//                                      (Godot computes velocity from jittered
//                                      matrices, so this may well be correct)
//   RW_DLSS_NO_AUTOEXPOSURE     = 1    drop the AutoExposure create flag
struct DlssKnobs {
	float jitter_sign_x = 1.0f;
	float jitter_sign_y = 1.0f;
	float mv_sign_x = 1.0f;
	float mv_sign_y = 1.0f;
	bool mv_jittered = false;
	bool no_autoexposure = false;
	bool loaded = false;
};

DlssKnobs knobs;

float _knob_sign(const String &p_env) {
	String v = OS::get_singleton()->get_environment(p_env);
	return (v == "-1") ? -1.0f : 1.0f;
}

const DlssKnobs &_dlss_knobs() {
	OS *os = OS::get_singleton();
	DlssKnobs fresh;
	fresh.jitter_sign_x = _knob_sign("RW_DLSS_JITTER_SIGN_X");
	fresh.jitter_sign_y = _knob_sign("RW_DLSS_JITTER_SIGN_Y");
	fresh.mv_sign_x = _knob_sign("RW_DLSS_MV_SIGN_X");
	fresh.mv_sign_y = _knob_sign("RW_DLSS_MV_SIGN_Y");
	fresh.mv_jittered = os->get_environment("RW_DLSS_MV_JITTERED") == "1";
	fresh.no_autoexposure = os->get_environment("RW_DLSS_NO_AUTOEXPOSURE") == "1";

	bool changed = !knobs.loaded ||
			fresh.jitter_sign_x != knobs.jitter_sign_x || fresh.jitter_sign_y != knobs.jitter_sign_y ||
			fresh.mv_sign_x != knobs.mv_sign_x || fresh.mv_sign_y != knobs.mv_sign_y ||
			fresh.mv_jittered != knobs.mv_jittered || fresh.no_autoexposure != knobs.no_autoexposure;
	if (changed) {
		fresh.loaded = true;
		knobs = fresh;
		print_line(vformat("DLSS: tuning knobs - jitter sign (%d,%d), MV sign (%d,%d), MV jittered %s, no auto-exposure %s.",
				(int)knobs.jitter_sign_x, (int)knobs.jitter_sign_y, (int)knobs.mv_sign_x, (int)knobs.mv_sign_y,
				knobs.mv_jittered ? "yes" : "no", knobs.no_autoexposure ? "yes" : "no"));
	}
	return knobs;
}

const wchar_t *_ngx_data_path() {
	if (ngx.data_path.length() == 0) {
		String dir = OS::get_singleton()->get_user_data_dir();
		if (dir.is_empty()) {
			dir = ".";
		}
		ngx.data_path = dir.utf16();
	}
	return (const wchar_t *)ngx.data_path.get_data();
}

bool _ngx_probe(ID3D12Device *p_device, IDXGIAdapter *p_adapter) {
	// 1. Cheap pre-init check: adapter architecture + driver version.
	NVSDK_NGX_FeatureDiscoveryInfo discovery = {};
	discovery.SDKVersion = NVSDK_NGX_Version_API;
	discovery.FeatureID = NVSDK_NGX_Feature_SuperSampling;
	discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
	discovery.Identifier.v.ProjectDesc.ProjectId = NGX_PROJECT_ID;
	discovery.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
	discovery.Identifier.v.ProjectDesc.EngineVersion = NGX_ENGINE_VERSION;
	discovery.ApplicationDataPath = _ngx_data_path();
	discovery.FeatureInfo = nullptr;

	NVSDK_NGX_FeatureRequirement requirement = {};
	NVSDK_NGX_Result res = NVSDK_NGX_D3D12_GetFeatureRequirements(p_adapter, &discovery, &requirement);
	if (NVSDK_NGX_FAILED(res) || requirement.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
		print_verbose(vformat("DLSS: unsupported on this adapter (result 0x%x, support mask %d).", (uint64_t)res, (int)requirement.FeatureSupported));
		return false;
	}

	// 2. Full init (loads the snippet DLL machinery).
	res = NVSDK_NGX_D3D12_Init_with_ProjectID(NGX_PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, NGX_ENGINE_VERSION, _ngx_data_path(), p_device);
	if (NVSDK_NGX_FAILED(res)) {
		print_verbose(vformat("DLSS: NGX init failed (0x%x).", (uint64_t)res));
		return false;
	}
	ngx.initialized = true;
	ngx.device = p_device;

	// 3. The definitive capability triple.
	res = NVSDK_NGX_D3D12_GetCapabilityParameters(&ngx.capability_params);
	if (NVSDK_NGX_FAILED(res) || ngx.capability_params == nullptr) {
		print_verbose(vformat("DLSS: no capability parameters (0x%x).", (uint64_t)res));
		return false;
	}

	int dlss_available = 0;
	ngx.capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &dlss_available);
	if (dlss_available == 0) {
		int needs_driver = 0;
		int min_major = 0;
		int min_minor = 0;
		ngx.capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
		ngx.capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &min_major);
		ngx.capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &min_minor);
		if (needs_driver != 0) {
			WARN_PRINT(vformat("DLSS: driver too old; needs at least %d.%d.", min_major, min_minor));
		} else {
			print_verbose("DLSS: SuperSampling reported unavailable.");
		}
		return false;
	}

	return true;
}

NVSDK_NGX_PerfQuality_Value _ngx_quality_for_ratio(float p_ratio) {
	// Godot exposes a free render-scale slider; snap to the nearest DLSS
	// quality bucket (its official ratios: 0.333 / 0.5 / 0.58 / 0.667 / 1.0).
	if (p_ratio >= 0.995f) {
		return NVSDK_NGX_PerfQuality_Value_DLAA;
	} else if (p_ratio >= 0.66f) {
		return NVSDK_NGX_PerfQuality_Value_MaxQuality;
	} else if (p_ratio >= 0.575f) {
		return NVSDK_NGX_PerfQuality_Value_Balanced;
	} else if (p_ratio >= 0.49f) {
		return NVSDK_NGX_PerfQuality_Value_MaxPerf;
	}
	return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

} // namespace

/////////////////////////////////////////////////
// DlssNgxContext

DlssNgxContext::~DlssNgxContext() {
	// NOTE: releases GPU resources immediately; contexts are destroyed on
	// render-buffer teardown (resize/close), mirroring FSR2/MetalFX lifetime.
	if (feature != nullptr) {
		NVSDK_NGX_D3D12_ReleaseFeature((NVSDK_NGX_Handle *)feature);
		feature = nullptr;
	}
	if (ngx_parameters != nullptr) {
		NVSDK_NGX_D3D12_DestroyParameters((NVSDK_NGX_Parameter *)ngx_parameters);
		ngx_parameters = nullptr;
	}
}

/////////////////////////////////////////////////
// DlssNgxEffect

DlssNgxEffect::DlssNgxEffect() {}

DlssNgxEffect::~DlssNgxEffect() {
	MutexLock lock(ngx.mutex);
	if (ngx.initialized) {
		if (ngx.capability_params != nullptr) {
			NVSDK_NGX_D3D12_DestroyParameters(ngx.capability_params);
			ngx.capability_params = nullptr;
		}
		NVSDK_NGX_D3D12_Shutdown1(ngx.device);
		ngx.initialized = false;
		ngx.available = false;
		ngx.tried = false;
		ngx.device = nullptr;
	}
}

bool DlssNgxEffect::is_available(void *p_d3d12_device, void *p_dxgi_adapter) {
	if (p_d3d12_device == nullptr || p_dxgi_adapter == nullptr) {
		return false;
	}
	MutexLock lock(ngx.mutex);
	if (ngx.tried) {
		return ngx.available;
	}
	ngx.tried = true;
	ngx.available = _ngx_probe((ID3D12Device *)p_d3d12_device, (IDXGIAdapter *)p_dxgi_adapter);
	if (ngx.available) {
		print_line("DLSS: available.");
	}
	return ngx.available;
}

bool DlssNgxEffect::context_stale(const DlssNgxContext *p_context) const {
	if (p_context == nullptr || (p_context->feature == nullptr && !p_context->feature_failed)) {
		return false; // Feature not created yet - it will pick up current knobs.
	}
	const DlssKnobs &k = _dlss_knobs();
	return p_context->created_mv_jittered != k.mv_jittered || p_context->created_no_autoexposure != k.no_autoexposure;
}

DlssNgxContext *DlssNgxEffect::create_context(Size2i p_internal_size, Size2i p_target_size, bool p_has_exposure) {
	ERR_FAIL_COND_V(!ngx.initialized, nullptr);

	NVSDK_NGX_Parameter *params = nullptr;
	NVSDK_NGX_Result res = NVSDK_NGX_D3D12_AllocateParameters(&params);
	ERR_FAIL_COND_V_MSG(NVSDK_NGX_FAILED(res) || params == nullptr, nullptr, vformat("DLSS: AllocateParameters failed (0x%x).", (uint64_t)res));

	DlssNgxContext *context = memnew(DlssNgxContext);
	context->ngx_parameters = params;
	context->internal_size = p_internal_size;
	context->target_size = p_target_size;
	context->has_exposure = p_has_exposure;
	return context;
}

void DlssNgxEffect::upscale(const Parameters &p_params) {
	ERR_FAIL_NULL(p_params.context);
	if (p_params.context->feature_failed) {
		return; // Feature creation already failed; renderer output stays at
				// internal resolution rather than spamming create attempts.
	}

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
	userdata->jitter = p_params.jitter;
	userdata->delta_time_ms = p_params.delta_time * 1000.0f; // NGX wants milliseconds.
	userdata->reset = p_params.reset_accumulation;

	// Declared usages make the render graph transition every resource before
	// the callback runs: sampled inputs to shader-resource, output to UAV -
	// exactly the states NGX requires at evaluate time.
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
	rd->driver_callback_add((RDD::DriverCallback)DlssNgxEffect::callback, userdata, VectorView<RD::CallbackResource>(res, res_count));
}

void DlssNgxEffect::callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata) {
	RenderingDeviceDriverD3D12 *driver = (RenderingDeviceDriverD3D12 *)p_driver;
	ID3D12GraphicsCommandList *cmd_list = driver->command_buffer_get_native_list(p_command_buffer);
	DlssNgxContext *ctx = p_userdata->ctx;
	NVSDK_NGX_Parameter *params = (NVSDK_NGX_Parameter *)ctx->ngx_parameters;

	// Lazy feature creation: NGX records initialization GPU work onto the
	// command list, so it has to happen here rather than in create_context.
	if (ctx->feature == nullptr && !ctx->feature_failed) {
		float ratio = float(ctx->internal_size.width) / MAX(1.0f, float(ctx->target_size.width));

		NVSDK_NGX_DLSS_Create_Params create_params = {};
		create_params.Feature.InWidth = (unsigned int)ctx->internal_size.width;
		create_params.Feature.InHeight = (unsigned int)ctx->internal_size.height;
		create_params.Feature.InTargetWidth = (unsigned int)ctx->target_size.width;
		create_params.Feature.InTargetHeight = (unsigned int)ctx->target_size.height;
		create_params.Feature.InPerfQualityValue = _ngx_quality_for_ratio(ratio);
		// Godot conventions (same as its FSR2/MetalFX integrations): linear
		// HDR color, reverse-Z depth, render-resolution MVs.
		create_params.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
				NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
				NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
		const DlssKnobs &create_knobs = _dlss_knobs();
		if (create_knobs.mv_jittered) {
			create_params.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
		}
		if (!ctx->has_exposure && !create_knobs.no_autoexposure) {
			create_params.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
		}
		ctx->created_mv_jittered = create_knobs.mv_jittered;
		ctx->created_no_autoexposure = create_knobs.no_autoexposure;

		NVSDK_NGX_Handle *handle = nullptr;
		NVSDK_NGX_Result res = NGX_D3D12_CREATE_DLSS_EXT(cmd_list, 1, 1, &handle, params, &create_params);
		if (NVSDK_NGX_FAILED(res) || handle == nullptr) {
			ctx->feature_failed = true;
			ERR_PRINT(vformat("DLSS: feature creation failed (0x%x).", (uint64_t)res));
			driver->command_buffer_mark_external_commands(p_command_buffer);
			CallbackArgs::free(&p_userdata);
			return;
		}
		ctx->feature = handle;
	}

	NVSDK_NGX_D3D12_DLSS_Eval_Params eval = {};
	eval.Feature.pInColor = (ID3D12Resource *)p_userdata->color;
	eval.Feature.pInOutput = (ID3D12Resource *)p_userdata->output;
	eval.pInDepth = (ID3D12Resource *)p_userdata->depth;
	eval.pInMotionVectors = (ID3D12Resource *)p_userdata->velocity;
	eval.pInExposureTexture = (ID3D12Resource *)p_userdata->exposure;
	const DlssKnobs &k = _dlss_knobs();
	eval.InJitterOffsetX = p_userdata->jitter.x * k.jitter_sign_x;
	eval.InJitterOffsetY = p_userdata->jitter.y * k.jitter_sign_y;
	eval.InRenderSubrectDimensions.Width = (unsigned int)p_userdata->internal_size.width;
	eval.InRenderSubrectDimensions.Height = (unsigned int)p_userdata->internal_size.height;
	eval.InReset = p_userdata->reset ? 1 : 0;
	// Godot's velocity buffer stores UV-space motion toward the previous
	// frame; scale by the render size to get pixel-space motion (the exact
	// scale Godot hands its FSR2/MetalFX integrations).
	eval.InMVScaleX = float(p_userdata->internal_size.width) * k.mv_sign_x;
	eval.InMVScaleY = float(p_userdata->internal_size.height) * k.mv_sign_y;
	eval.InPreExposure = 1.0f;
	eval.InFrameTimeDeltaInMsec = p_userdata->delta_time_ms;

	NVSDK_NGX_Result res = NGX_D3D12_EVALUATE_DLSS_EXT(cmd_list, (NVSDK_NGX_Handle *)ctx->feature, params, &eval);
	if (NVSDK_NGX_FAILED(res)) {
		ERR_PRINT_ONCE(vformat("DLSS: evaluate failed (0x%x).", (uint64_t)res));
	}

	// NGX changed descriptor heaps / PSO / root signature behind Godot's
	// back; make the driver re-bind its own state lazily.
	driver->command_buffer_mark_external_commands(p_command_buffer);

	CallbackArgs::free(&p_userdata);
}

#endif // DLSS_D3D12_ENABLED
