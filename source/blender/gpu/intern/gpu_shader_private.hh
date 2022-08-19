/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BLI_span.hh"
#include "BLI_string_ref.hh"

#include "GPU_shader.h"
#include "gpu_shader_create_info.hh"
#include "gpu_shader_interface.hh"
#include "gpu_vertex_buffer_private.hh"

#include <string>

namespace blender {
namespace gpu {

class GPULogParser;

/**
 * Implementation of shader compilation and uniforms handling.
 * Base class which is then specialized for each implementation (GL, VK, ...).
 */
class Shader {
 public:
  /** Uniform & attribute locations for shader. */
  ShaderInterface *interface = nullptr;

 protected:
  /** For debugging purpose. */
  char name[64];

 public:
  Shader(const char *name);
  virtual ~Shader();

  virtual void vertex_shader_from_glsl(MutableSpan<const char *> sources) = 0;
  virtual void geometry_shader_from_glsl(MutableSpan<const char *> sources) = 0;
  virtual void fragment_shader_from_glsl(MutableSpan<const char *> sources) = 0;
  virtual void compute_shader_from_glsl(MutableSpan<const char *> sources) = 0;
  virtual bool finalize(const shader::ShaderCreateInfo *info = nullptr) = 0;

  virtual void transform_feedback_names_set(Span<const char *> name_list,
                                            eGPUShaderTFBType geom_type) = 0;
  virtual bool transform_feedback_enable(GPUVertBuf *) = 0;
  virtual void transform_feedback_disable() = 0;

  virtual void bind() = 0;
  virtual void unbind() = 0;

  virtual void uniform_float(int location, int comp_len, int array_size, const float *data) = 0;
  virtual void uniform_int(int location, int comp_len, int array_size, const int *data) = 0;

  virtual void vertformat_from_shader(GPUVertFormat *) const = 0;

  std::string defines_declare(const shader::ShaderCreateInfo &info) const;
  virtual std::string resources_declare(const shader::ShaderCreateInfo &info) const = 0;
  virtual std::string vertex_interface_declare(const shader::ShaderCreateInfo &info) const = 0;
  virtual std::string fragment_interface_declare(const shader::ShaderCreateInfo &info) const = 0;
  virtual std::string geometry_interface_declare(const shader::ShaderCreateInfo &info) const = 0;
  virtual std::string geometry_layout_declare(const shader::ShaderCreateInfo &info) const = 0;
  virtual std::string compute_layout_declare(const shader::ShaderCreateInfo &info) const = 0;

  /* DEPRECATED: Kept only because of BGL API. */
  virtual int program_handle_get() const = 0;

  inline const char *const name_get() const
  {
    return name;
  };

  /* UPBGE */
  virtual char *shader_validate() = 0;
  virtual void shader_bind_attributes(int *locations, const char **names, int len) = 0;
  // GPU_shader_get_uniform doesn't handle array uniforms e.g: uniform vec2
  // bgl_TextureCoordinateOffset[9];
  virtual int shader_get_uniform_location_old(const char *name) = 0;

 protected:
  void print_log(
      Span<const char *> sources, char *log, const char *stage, bool error, GPULogParser *parser);
};

/* Syntactic sugar. */
static inline GPUShader *wrap(Shader *vert)
{
  return reinterpret_cast<GPUShader *>(vert);
}
static inline Shader *unwrap(GPUShader *vert)
{
  return reinterpret_cast<Shader *>(vert);
}
static inline const Shader *unwrap(const GPUShader *vert)
{
  return reinterpret_cast<const Shader *>(vert);
}

enum class Severity {
  Unknown,
  Warning,
  Error,
};

struct LogCursor {
  int source = -1;
  int row = -1;
  int column = -1;
};

struct GPULogItem {
  LogCursor cursor;
  bool source_base_row = false;
  Severity severity = Severity::Unknown;
};

class GPULogParser {
 public:
  virtual char *parse_line(char *log_line, GPULogItem &log_item) = 0;

 protected:
  char *skip_severity(char *log_line,
                      GPULogItem &log_item,
                      const char *error_msg,
                      const char *warning_msg) const;
  char *skip_separators(char *log_line, const StringRef separators) const;
  char *skip_until(char *log_line, char stop_char) const;
  bool at_number(const char *log_line) const;
  bool at_any(const char *log_line, const StringRef chars) const;
  int parse_number(const char *log_line, char **r_new_position) const;

  MEM_CXX_CLASS_ALLOC_FUNCS("GPULogParser");
};

}  // namespace gpu
}  // namespace blender

/* XXX do not use it. Special hack to use OCIO with batch API. */
GPUShader *immGetShader();
