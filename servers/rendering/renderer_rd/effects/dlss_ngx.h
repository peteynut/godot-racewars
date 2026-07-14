/**************************************************************************/
/*  dlss_ngx.h                                                            */
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

#pragma once

// RaceWars fork: NVIDIA DLSS Super Resolution / DLAA through direct NGX
// (nvngx_dlss.dll shipped beside the exe; headers + static lib from the
// public DLSS SDK, github.com/NVIDIA/DLSS, pulled at build time via the
// DLSS_SDK env var). DLAA is simply DLSS evaluated at a 1.0 render scale.
// Modeled on metal_fx.h (the other closed-SDK upscaler wrapper).
// Windows D3D12 export templates only; see RACEWARS_FORK.md.

#ifdef DLSS_D3D12_ENABLED

#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/templates/paged_allocator.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

struct DlssNgxContext {
	// NVSDK_NGX_Parameter*, owned. Allocated on the render thread at context
	// creation.
	void *ngx_parameters = nullptr;
	// NVSDK_NGX_Handle*, owned. Created lazily inside the first driver
	// callback (NGX feature creation records GPU work on a command list).
	void *feature = nullptr;
	bool feature_failed = false;
	Size2i internal_size;
	Size2i target_size;
	bool has_exposure = false;
	~DlssNgxContext();
};

class DlssNgxEffect {
	struct CallbackArgs {
		DlssNgxEffect *owner = nullptr;
		DlssNgxContext *ctx = nullptr;
		// Native ID3D12Resource pointers, resolved on the render thread.
		void *color = nullptr;
		void *depth = nullptr;
		void *velocity = nullptr;
		void *exposure = nullptr;
		void *output = nullptr;
		Size2i internal_size;
		Vector2 jitter;
		float delta_time_ms = 0.0f;
		bool reset = false;

		static void free(CallbackArgs **p_args) {
			(*p_args)->owner->args_allocator.free(*p_args);
			*p_args = nullptr;
		}
	};

	PagedAllocator<CallbackArgs, true, 16> args_allocator;

	static void callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata);

public:
	// Full availability probe: adapter/driver requirements, NGX init, and the
	// SuperSampling.Available capability (result cached). Called by the D3D12
	// driver's has_feature.
	static bool is_available(void *p_d3d12_device, void *p_dxgi_adapter);

	// p_has_exposure: an explicit exposure texture will be provided each
	// frame; otherwise the feature is created with NGX auto-exposure.
	DlssNgxContext *create_context(Size2i p_internal_size, Size2i p_target_size, bool p_has_exposure);

	struct Parameters {
		DlssNgxContext *context = nullptr;
		Size2i internal_size;
		RID color;
		RID depth;
		RID velocity;
		RID exposure; // Optional (auto-exposure luminance buffer).
		RID output;
		Vector2 jitter; // In render (internal) pixel space, same as FSR2.
		float delta_time = 0.0f; // Seconds; converted to ms for the SDK.
		bool reset_accumulation = false;
	};

	void upscale(const Parameters &p_params);

	DlssNgxEffect();
	~DlssNgxEffect();
};

} // namespace RendererRD

#endif // DLSS_D3D12_ENABLED
