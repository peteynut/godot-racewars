/**************************************************************************/
/*  ffx_upscaler.h                                                        */
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

// RaceWars fork: AMD FSR 3.1 / FSR 4 temporal upscaling through the ffx-api
// (amd_fidelityfx_loader_dx12.dll + amd_fidelityfx_upscaler_dx12.dll, AMD's
// signed binary DLLs shipped beside the exe). RDNA3/4 GPUs get FSR 4.x from
// the same DLL, everything else gets FSR 3.1.x - the provider decides.
// Modeled on metal_fx.h (the other closed-SDK upscaler wrapper).
// Windows D3D12 export templates only; see RACEWARS_FORK.md.

#ifdef FFX_UPSCALER_D3D12_ENABLED

#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/templates/paged_allocator.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

struct FfxUpscalerContext {
	void *ffx_context = nullptr; // ffxContext, owned.
	Size2i internal_size;
	Size2i target_size;
	~FfxUpscalerContext();
};

class FfxUpscalerEffect {
	struct CallbackArgs {
		FfxUpscalerEffect *owner = nullptr;
		FfxUpscalerContext *ctx = nullptr;
		// Native ID3D12Resource pointers, resolved on the render thread.
		void *color = nullptr;
		void *depth = nullptr;
		void *velocity = nullptr;
		void *exposure = nullptr;
		void *output = nullptr;
		Size2i internal_size;
		Size2i target_size;
		Vector2 jitter;
		float sharpness = 0.0f;
		float delta_time_ms = 0.0f;
		float z_near = 0.0f;
		float z_far = 0.0f;
		float fovy = 0.0f;
		bool reset = false;

		static void free(CallbackArgs **p_args) {
			(*p_args)->owner->args_allocator.free(*p_args);
			*p_args = nullptr;
		}
	};

	PagedAllocator<CallbackArgs, true, 16> args_allocator;

	static void callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata);

public:
	// Loads the signed loader DLL (once) and confirms an upscale provider
	// exists for this device. Called by the D3D12 driver's has_feature.
	static bool is_available(void *p_d3d12_device);

	FfxUpscalerContext *create_context(Size2i p_internal_size, Size2i p_target_size);

	struct Parameters {
		FfxUpscalerContext *context = nullptr;
		Size2i internal_size;
		float sharpness = 0.0f;
		RID color;
		RID depth;
		RID velocity;
		RID exposure; // Optional (auto-exposure luminance buffer).
		RID output;
		float z_near = 0.0f;
		float z_far = 0.0f;
		float fovy = 0.0f;
		Vector2 jitter; // In render (internal) pixel space, same as FSR2.
		float delta_time = 0.0f; // Seconds; converted to ms for the SDK.
		bool reset_accumulation = false;
	};

	void upscale(const Parameters &p_params);

	FfxUpscalerEffect();
	~FfxUpscalerEffect();
};

} // namespace RendererRD

#endif // FFX_UPSCALER_D3D12_ENABLED
