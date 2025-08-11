/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * GPU Compute Pipeline
 *
 * Allows to dispatch compute shader tasks on the GPU.
 * Every dispatch is sent to the active `GPUContext`.
 */

#pragma once

#include "BLI_sys_types.h"

#include "GPU_shader.hh"
#include "GPU_storage_buffer.hh"

/**
 * Dispatch a compute shader task.
 * The number of work groups (aka thread groups) is bounded by `GPU_max_work_group_count()` which
 * might be different in each of the 3 dimensions.
 */
void GPU_compute_dispatch(
    blender::gpu::Shader *shader,
    uint groups_x_len,
    uint groups_y_len,
    uint groups_z_len,
    const blender::gpu::shader::SpecializationConstants *constants_state = nullptr);

/**
 * Dispatch a compute shader task. The size of the dispatch is sourced from a \a indirect_buf
 * which must follow this layout:
 * \code{.c}
 * struct DispatchIndirectCommand {
 *   uint groups_x_len;
 *   uint groups_y_len;
 *   uint groups_z_len;
 * };
 * \encode
 *
 * \note The writes to the \a indirect_buf do not need to be synchronized as a memory barrier is
 * emitted internally.
 *
 * The number of work groups (aka thread groups) is bounded by `GPU_max_work_group_count()` which
 * might be different in each of the 3 dimensions.
 */
void GPU_compute_dispatch_indirect(
    blender::gpu::Shader *shader,
    blender::gpu::StorageBuf *indirect_buf,
    const blender::gpu::shader::SpecializationConstants *constants_state = nullptr);
