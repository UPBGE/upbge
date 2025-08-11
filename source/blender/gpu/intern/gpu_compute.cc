/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "GPU_compute.hh"

#include "gpu_backend.hh"

void GPU_compute_dispatch(blender::gpu::Shader *shader,
                          uint groups_x_len,
                          uint groups_y_len,
                          uint groups_z_len,
                          const blender::gpu::shader::SpecializationConstants *constants_state)
{
  blender::gpu::GPUBackend &gpu_backend = *blender::gpu::GPUBackend::get();
  GPU_shader_bind(shader, constants_state);
  gpu_backend.compute_dispatch(groups_x_len, groups_y_len, groups_z_len);
}

void GPU_compute_dispatch_indirect(
    blender::gpu::Shader *shader,
    blender::gpu::StorageBuf *indirect_buf_,
    const blender::gpu::shader::SpecializationConstants *constants_state)
{
  blender::gpu::GPUBackend &gpu_backend = *blender::gpu::GPUBackend::get();
  blender::gpu::StorageBuf *indirect_buf = reinterpret_cast<blender::gpu::StorageBuf *>(
      indirect_buf_);

  GPU_shader_bind(shader, constants_state);
  gpu_backend.compute_dispatch_indirect(indirect_buf);
}
