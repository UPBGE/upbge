/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2016 by Mike Erwin. All rights reserved. */

/** \file
 * \ingroup gpu
 *
 * This interface allow GPU to manage VAOs for multiple context and threads.
 */

#pragma once

#include "GPU_batch.h"
#include "GPU_common.h"
#include "GPU_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GPU back-ends abstract the differences between different APIs. #GPU_context_create
 * automatically initializes the back-end, and #GPU_context_discard frees it when there
 * are no more contexts. */
bool GPU_backend_supported(void);
eGPUBackendType GPU_backend_get_type(void);

/** Opaque type hiding blender::gpu::Context. */
typedef struct GPUContext GPUContext;

GPUContext *GPU_context_create(void *ghost_window);
/**
 * To be called after #GPU_context_active_set(ctx_to_destroy).
 */
void GPU_context_discard(GPUContext *);

/**
 * Ctx can be NULL.
 */
void GPU_context_active_set(GPUContext *);
GPUContext *GPU_context_active_get(void);

/* Begin and end frame are used to mark the singular boundary representing the lifetime of a whole
 * frame. This also acts as a divisor for ensuring workload submission and flushing, especially for
 * background rendering when there is no call to present.
 * This is required by explicit-API's where there is no implicit workload flushing. */
void GPU_context_begin_frame(GPUContext *ctx);
void GPU_context_end_frame(GPUContext *ctx);

/* Legacy GPU (Intel HD4000 series) do not support sharing GPU objects between GPU
 * contexts. EEVEE/Workbench can create different contexts for image/preview rendering, baking or
 * compiling. When a legacy GPU is detected (`GPU_use_main_context_workaround()`) any worker
 * threads should use the draw manager opengl context and make sure that they are the only one
 * using it by locking the main context using these two functions. */
void GPU_context_main_lock(void);
void GPU_context_main_unlock(void);

/* GPU Begin/end work blocks */
void GPU_render_begin(void);
void GPU_render_end(void);

/* For operations which need to run exactly once per frame -- even if there are no render updates.
 */
void GPU_render_step(void);

#ifdef __cplusplus
}
#endif
