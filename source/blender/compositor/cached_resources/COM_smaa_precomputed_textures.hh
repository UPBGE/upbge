/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <memory>

#include "GPU_shader.hh"
#include "GPU_texture.hh"

#include "COM_cached_resource.hh"
#include "COM_result.hh"

namespace blender::compositor {

class Context;

/* -------------------------------------------------------------------------------------------------
 * SMAA Precomputed Textures.
 *
 * A cached resource that caches the precomputed textures needed by the SMAA algorithm. The
 * precomputed textures are constants, so this is a parameterless cached resource. */
class SMAAPrecomputedTextures : public CachedResource {
 public:
  /* CPU storage, unused for GPU execution device. We can't store the GPU textures in the result
   * because it requires special data types that are not supported by the Result class. */
  Result search_texture;
  Result area_texture;

 private:
  /* GPU storage, unused for CPU execution device. */
  blender::gpu::Texture *search_texture_ = nullptr;
  blender::gpu::Texture *area_texture_ = nullptr;

 public:
  SMAAPrecomputedTextures(Context &context);

  ~SMAAPrecomputedTextures();

  void bind_search_texture(gpu::Shader *shader, const char *sampler_name) const;

  void unbind_search_texture() const;

  void bind_area_texture(gpu::Shader *shader, const char *sampler_name) const;

  void unbind_area_texture() const;

 private:
  void compute_gpu();

  void compute_cpu();
};

/* ------------------------------------------------------------------------------------------------
 * SMAA Precomputed Textures Container.
 */
class SMAAPrecomputedTexturesContainer : public CachedResourceContainer {
 private:
  std::unique_ptr<SMAAPrecomputedTextures> textures_;

 public:
  void reset() override;

  /* Check if a cached SMAA precomputed texture exists, if it does, return it, otherwise, return
   * a newly created one and store it in the container. In both cases, tag the cached resource as
   * needed to keep it cached for the next evaluation. */
  SMAAPrecomputedTextures &get(Context &context);
};

}  // namespace blender::compositor
