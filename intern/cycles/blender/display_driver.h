/* SPDX-FileCopyrightText: 2021-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <atomic>

#include "RNA_blender_cpp.hh"

#include "session/display_driver.h"

#include "util/unique_ptr.h"

struct GPUContext;
struct GPUFence;
namespace blender::gpu {
class Shader;
}  // namespace blender::gpu

CCL_NAMESPACE_BEGIN

/* Base class of shader used for display driver rendering. */
class BlenderDisplayShader {
 public:
  static constexpr const char *position_attribute_name = "pos";
  static constexpr const char *tex_coord_attribute_name = "texCoord";

  /* Create shader implementation suitable for the given render engine and scene configuration. */
  static unique_ptr<BlenderDisplayShader> create(BL::RenderEngine &b_engine, BL::Scene &b_scene);

  BlenderDisplayShader() = default;
  virtual ~BlenderDisplayShader() = default;

  virtual blender::gpu::Shader *bind(const int width, const int height) = 0;
  virtual void unbind() = 0;

  /* Get attribute location for position and texture coordinate respectively.
   * NOTE: The shader needs to be bound to have access to those. */
  virtual int get_position_attrib_location();
  virtual int get_tex_coord_attrib_location();

 protected:
  /* Get program of this display shader.
   * NOTE: The shader needs to be bound to have access to this. */
  virtual blender::gpu::Shader *get_shader_program() = 0;

  /* Cached values of various OpenGL resources. */
  int position_attribute_location_ = -1;
  int tex_coord_attribute_location_ = -1;
};

/* Implementation of display rendering shader used in the case when render engine does not support
 * display space shader. */
class BlenderFallbackDisplayShader : public BlenderDisplayShader {
 public:
  ~BlenderFallbackDisplayShader() override;

  blender::gpu::Shader *bind(const int width, const int height) override;
  void unbind() override;

 protected:
  blender::gpu::Shader *get_shader_program() override;

  void create_shader_if_needed();
  void destroy_shader();

  blender::gpu::Shader *shader_program_ = nullptr;
  int image_texture_location_ = -1;
  int fullscreen_location_ = -1;

  /* Shader compilation attempted. Which means, that if the shader program is 0 then compilation or
   * linking has failed. Do not attempt to re-compile the shader. */
  bool shader_compile_attempted_ = false;
};

class BlenderDisplaySpaceShader : public BlenderDisplayShader {
 public:
  BlenderDisplaySpaceShader(BL::RenderEngine &b_engine, BL::Scene &b_scene);

  blender::gpu::Shader *bind(const int width, const int height) override;
  void unbind() override;

 protected:
  blender::gpu::Shader *get_shader_program() override;

  BL::RenderEngine b_engine_;
  BL::Scene &b_scene_;

  /* Cached values of various OpenGL resources. */
  blender::gpu::Shader *shader_program_ = nullptr;
};

/* Display driver implementation which is specific for Blender viewport integration. */
class BlenderDisplayDriver : public DisplayDriver {
 public:
  BlenderDisplayDriver(BL::RenderEngine &b_engine, BL::Scene &b_scene, const bool background);
  ~BlenderDisplayDriver() override;

  void graphics_interop_activate() override;
  void graphics_interop_deactivate() override;

  void zero() override;

  void set_zoom(const float zoom_x, const float zoom_y);

 protected:
  void next_tile_begin() override;

  bool update_begin(const Params &params,
                    const int texture_width,
                    const int texture_height) override;
  void update_end() override;

  half4 *map_texture_buffer() override;
  void unmap_texture_buffer() override;

  GraphicsInteropDevice graphics_interop_get_device() override;
  void graphics_interop_update_buffer() override;

  void draw(const Params &params) override;

  void flush() override;

  /* Helper function which allocates new GPU context. */
  void gpu_context_create();
  bool gpu_context_enable();
  void gpu_context_disable();
  void gpu_context_destroy();
  void gpu_context_lock();
  void gpu_context_unlock();

  /* Create GPU resources used by the display driver. */
  bool gpu_resources_create();

  /* Destroy all GPU resources which are being used by this object. */
  void gpu_resources_destroy();

  BL::RenderEngine b_engine_;
  bool background_;

  /* Content of the display is to be filled with zeroes. */
  std::atomic<bool> need_zero_ = true;

  unique_ptr<BlenderDisplayShader> display_shader_;

  /* Opaque storage for an internal state and data for tiles. */
  struct Tiles;
  unique_ptr<Tiles> tiles_;

  GPUFence *gpu_render_sync_ = nullptr;
  GPUFence *gpu_upload_sync_ = nullptr;

  float2 zoom_ = make_float2(1.0f, 1.0f);
};

CCL_NAMESPACE_END
