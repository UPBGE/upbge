/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Debug features of OpenGL.
 */

#include "BLI_compiler_attrs.h"
#include "BLI_string.h"
#include "BLI_system.h"
#include "BLI_utildefines.h"

#include "BKE_global.hh"

#include "GPU_debug.hh"
#include "GPU_platform.hh"
#include "gpu_profile_report.hh"

#include "CLG_log.h"

#include "gl_backend.hh"
#include "gl_context.hh"
#include "gl_uniform_buffer.hh"

#include "gl_debug.hh"

static CLG_LogRef LOG = {"gpu.debug"};

/* Avoid too much NVidia buffer info in the output log. */
#define TRIM_NVIDIA_BUFFER_INFO 1
/* Avoid unneeded shader statistics. */
#define TRIM_SHADER_STATS_INFO 1

namespace blender::gpu::debug {

/* -------------------------------------------------------------------- */
/** \name Debug Callbacks
 *
 * Hooks up debug callbacks to a debug OpenGL context using extensions or 4.3 core debug
 * capabilities.
 * \{ */

/* Debug callbacks need the same calling convention as OpenGL functions. */
#if defined(_WIN32)
#  define APIENTRY __stdcall
#else
#  define APIENTRY
#endif

static void APIENTRY debug_callback(GLenum /*source*/,
                                    GLenum type,
                                    GLuint /*id*/,
                                    GLenum severity,
                                    GLsizei /*length*/,
                                    const GLchar *message,
                                    const GLvoid * /*userParm*/)
{
  if (ELEM(type, GL_DEBUG_TYPE_PUSH_GROUP, GL_DEBUG_TYPE_POP_GROUP)) {
    /* The debug layer will emit a message each time a debug group is pushed or popped.
     * We use that for easy command grouping inside frame analyzer tools. */
    return;
  }

  /* NOTE: callback function can be triggered during before the platform is initialized.
   *       In this case invoking `GPU_type_matches` would fail and
   *       therefore the message is checked before the platform matching. */
  if (TRIM_NVIDIA_BUFFER_INFO && STRPREFIX(message, "Buffer detailed info") &&
      GPU_type_matches(GPU_DEVICE_NVIDIA, GPU_OS_ANY, GPU_DRIVER_OFFICIAL))
  {
    /* Suppress buffer information flooding the output. */
    return;
  }

  if (TRIM_SHADER_STATS_INFO && STRPREFIX(message, "Shader Stats")) {
    /* Suppress buffer information flooding the output. */
    return;
  }

  const bool use_color = CLG_color_support_get(&LOG);

  if (ELEM(severity, GL_DEBUG_SEVERITY_LOW, GL_DEBUG_SEVERITY_NOTIFICATION)) {
    if (CLOG_CHECK(&LOG, CLG_LEVEL_INFO)) {
      const char *format = use_color ? "\033[2m%s\033[0m" : "%s";
      CLG_logf(LOG.type, CLG_LEVEL_INFO, "Notification", "", format, message);
    }
  }
  else {
    char debug_groups[512] = "";
    GPU_debug_get_groups_names(sizeof(debug_groups), debug_groups);
    CLG_Level clog_level;

    if (GPU_debug_group_match(GPU_DEBUG_SHADER_COMPILATION_GROUP) ||
        GPU_debug_group_match(GPU_DEBUG_SHADER_SPECIALIZATION_GROUP))
    {
      /* Do not duplicate shader compilation error/warnings. */
      return;
    }

    switch (type) {
      case GL_DEBUG_TYPE_ERROR:
      case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
      case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
        clog_level = CLG_LEVEL_ERROR;
        break;
      case GL_DEBUG_TYPE_PORTABILITY:
      case GL_DEBUG_TYPE_PERFORMANCE:
      case GL_DEBUG_TYPE_OTHER:
      case GL_DEBUG_TYPE_MARKER: /* KHR has this, ARB does not */
      default:
        clog_level = CLG_LEVEL_WARN;
        break;
    }

    if (CLOG_CHECK(&LOG, clog_level)) {
      CLG_logf(LOG.type, clog_level, debug_groups, "", "%s", message);
      if (severity == GL_DEBUG_SEVERITY_HIGH) {
        /* Focus on error message. */
        if (use_color) {
          fprintf(stderr, "\033[2m");
        }
        BLI_system_backtrace(stderr);
        if (use_color) {
          fprintf(stderr, "\033[0m\n");
        }
        fflush(stderr);
      }
    }
  }
}

#undef APIENTRY

void init_gl_callbacks()
{
  glEnable(GL_DEBUG_OUTPUT);
  glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  glDebugMessageCallback((GLDEBUGPROC)debug_callback, nullptr);
  glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
  glDebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION,
                       GL_DEBUG_TYPE_MARKER,
                       0,
                       GL_DEBUG_SEVERITY_NOTIFICATION,
                       -1,
                       "Successfully hooked OpenGL debug callback");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Error Checking
 *
 * This is only useful for implementation that does not support the KHR_debug extension OR when the
 * implementations do not report any errors even when clearly doing shady things.
 * \{ */

void check_gl_error(const char *info)
{
  if (!(G.debug & G_DEBUG_GPU)) {
    return;
  }
  GLenum error = glGetError();

#define ERROR_CASE(err) \
  case err: { \
    char msg[256]; \
    SNPRINTF(msg, "%s : %s", #err, info); \
    debug_callback(0, GL_DEBUG_TYPE_ERROR, 0, GL_DEBUG_SEVERITY_HIGH, 0, msg, nullptr); \
    break; \
  }

  switch (error) {
    ERROR_CASE(GL_INVALID_ENUM)
    ERROR_CASE(GL_INVALID_VALUE)
    ERROR_CASE(GL_INVALID_OPERATION)
    ERROR_CASE(GL_INVALID_FRAMEBUFFER_OPERATION)
    ERROR_CASE(GL_OUT_OF_MEMORY)
    ERROR_CASE(GL_STACK_UNDERFLOW)
    ERROR_CASE(GL_STACK_OVERFLOW)
    case GL_NO_ERROR:
      break;
    default:
      char msg[256];
      SNPRINTF(msg, "Unknown GL error: %x : %s", error, info);
      debug_callback(0, GL_DEBUG_TYPE_ERROR, 0, GL_DEBUG_SEVERITY_HIGH, 0, msg, nullptr);
      break;
  }
}

void check_gl_resources(const char *info)
{
  if (!(G.debug & G_DEBUG_GPU)) {
    return;
  }

  GLContext *ctx = GLContext::get();
  ShaderInterface *interface = ctx->shader->interface;
  /* NOTE: This only check binding. To be valid, the bound ubo needs to
   * be big enough to feed the data range the shader awaits. */
  uint16_t ubo_needed = interface->enabled_ubo_mask_;
  ubo_needed &= ~ctx->bound_ubo_slots;
  /* NOTE: This only check binding. To be valid, the bound ssbo needs to
   * be big enough to feed the data range the shader awaits. */
  uint16_t ssbo_needed = interface->enabled_ssbo_mask_;
  ssbo_needed &= ~ctx->bound_ssbo_slots;
  /* NOTE: This only check binding. To be valid, the bound texture needs to
   * be the same format/target the shader expects. */
  uint64_t tex_needed = interface->enabled_tex_mask_;
  tex_needed &= ~GLContext::state_manager_active_get()->bound_texture_slots();
  /* NOTE: This only check binding. To be valid, the bound image needs to
   * be the same format/target the shader expects. */
  uint8_t ima_needed = interface->enabled_ima_mask_;
  ima_needed &= ~GLContext::state_manager_active_get()->bound_image_slots();

  if (ubo_needed == 0 && tex_needed == 0 && ima_needed == 0 && ssbo_needed == 0) {
    return;
  }

  for (int i = 0; ubo_needed != 0; i++, ubo_needed >>= 1) {
    if ((ubo_needed & 1) != 0) {
      const ShaderInput *ubo_input = interface->ubo_get(i);
      const char *ubo_name = interface->input_name_get(ubo_input);
      const StringRefNull sh_name = ctx->shader->name_get();
      char msg[256];
      SNPRINTF(
          msg, "Missing UBO bind at slot %d : %s > %s : %s", i, sh_name.c_str(), ubo_name, info);
      debug_callback(0, GL_DEBUG_TYPE_ERROR, 0, GL_DEBUG_SEVERITY_HIGH, 0, msg, nullptr);
    }
  }

  for (int i = 0; ssbo_needed != 0; i++, ssbo_needed >>= 1) {
    if ((ssbo_needed & 1) != 0) {
      const ShaderInput *ssbo_input = interface->ssbo_get(i);
      const char *ssbo_name = interface->input_name_get(ssbo_input);
      const StringRefNull sh_name = ctx->shader->name_get();
      char msg[256];
      SNPRINTF(
          msg, "Missing SSBO bind at slot %d : %s > %s : %s", i, sh_name.c_str(), ssbo_name, info);
      debug_callback(0, GL_DEBUG_TYPE_ERROR, 0, GL_DEBUG_SEVERITY_HIGH, 0, msg, nullptr);
    }
  }

  for (int i = 0; tex_needed != 0; i++, tex_needed >>= 1) {
    if ((tex_needed & 1) != 0) {
      /* FIXME: texture_get might return an image input instead. */
      const ShaderInput *tex_input = interface->texture_get(i);
      const char *tex_name = interface->input_name_get(tex_input);
      const StringRefNull sh_name = ctx->shader->name_get();
      char msg[256];
      SNPRINTF(msg,
               "Missing Texture bind at slot %d : %s > %s : %s",
               i,
               sh_name.c_str(),
               tex_name,
               info);
      debug_callback(0, GL_DEBUG_TYPE_ERROR, 0, GL_DEBUG_SEVERITY_HIGH, 0, msg, nullptr);
    }
  }

  for (int i = 0; ima_needed != 0; i++, ima_needed >>= 1) {
    if ((ima_needed & 1) != 0) {
      /* FIXME: texture_get might return a texture input instead. */
      const ShaderInput *tex_input = interface->texture_get(i);
      const char *tex_name = interface->input_name_get(tex_input);
      const StringRefNull sh_name = ctx->shader->name_get();
      char msg[256];
      SNPRINTF(
          msg, "Missing Image bind at slot %d : %s > %s : %s", i, sh_name.c_str(), tex_name, info);
      debug_callback(0, GL_DEBUG_TYPE_ERROR, 0, GL_DEBUG_SEVERITY_HIGH, 0, msg, nullptr);
    }
  }
}

void raise_gl_error(const char *info)
{
  debug_callback(0, GL_DEBUG_TYPE_ERROR, 0, GL_DEBUG_SEVERITY_HIGH, 0, info, nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Label
 *
 * Useful for debugging through render-doc. Only defined if using `--debug-gpu`.
 * Make sure to bind the object first so that it gets defined by the GL implementation.
 * \{ */

static const char *to_str_prefix(GLenum type)
{
  switch (type) {
    case GL_FRAGMENT_SHADER:
    case GL_GEOMETRY_SHADER:
    case GL_VERTEX_SHADER:
    case GL_SHADER:
    case GL_PROGRAM:
      return "SHD-";
    case GL_SAMPLER:
      return "SAM-";
    case GL_TEXTURE:
      return "TEX-";
    case GL_FRAMEBUFFER:
      return "FBO-";
    case GL_VERTEX_ARRAY:
      return "VAO-";
    case GL_UNIFORM_BUFFER:
      return "UBO-";
    case GL_BUFFER:
      return "BUF-";
    default:
      return "";
  }
}
static const char *to_str_suffix(GLenum type)
{
  switch (type) {
    case GL_FRAGMENT_SHADER:
      return "-Frag";
    case GL_GEOMETRY_SHADER:
      return "-Geom";
    case GL_VERTEX_SHADER:
      return "-Vert";
    default:
      return "";
  }
}

void object_label(GLenum type, GLuint object, const char *name)
{
  if ((G.debug & G_DEBUG_GPU) &&
      (epoxy_gl_version() >= 43 || epoxy_has_gl_extension("GL_KHR_debug")))
  {
    char label[64];
    SNPRINTF(label, "%s%s%s", to_str_prefix(type), name, to_str_suffix(type));
    /* Small convenience for caller. */
    switch (type) {
      case GL_FRAGMENT_SHADER:
      case GL_GEOMETRY_SHADER:
      case GL_VERTEX_SHADER:
      case GL_COMPUTE_SHADER:
        type = GL_SHADER;
        break;
      case GL_UNIFORM_BUFFER:
      case GL_SHADER_STORAGE_BUFFER:
      case GL_ARRAY_BUFFER:
      case GL_ELEMENT_ARRAY_BUFFER:
        type = GL_BUFFER;
        break;
      default:
        break;
    }
    glObjectLabel(type, object, -1, label);
  }
}

/** \} */

}  // namespace blender::gpu::debug

namespace blender::gpu {

/* -------------------------------------------------------------------- */
/** \name Debug Groups
 *
 * Useful for debugging through render-doc. This makes all the API calls grouped into "passes".
 * \{ */

void GLContext::debug_group_begin(const char *name, int index)
{
  if ((G.debug & G_DEBUG_GPU) &&
      (epoxy_gl_version() >= 43 || epoxy_has_gl_extension("GL_KHR_debug")))
  {
    /* Add 10 to avoid collision with other indices from other possible callback layers. */
    index += 10;
    glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, index, -1, name);
  }

  if (!G.profile_gpu) {
    return;
  }

  TimeQuery query = {};
  query.name = name;
  query.finished = false;

  glGetInteger64v(GL_TIMESTAMP, &query.cpu_start);
  /* Use GL_TIMESTAMP instead of GL_ELAPSED_TIME to support nested debug groups */
  glGenQueries(2, query.handles);
  glQueryCounter(query.handle_start, GL_TIMESTAMP);

  if (frame_timings.is_empty()) {
    frame_timings.append({});
  }
  frame_timings.last().queries.append(query);
}

void GLContext::debug_group_end()
{
  if ((G.debug & G_DEBUG_GPU) &&
      (epoxy_gl_version() >= 43 || epoxy_has_gl_extension("GL_KHR_debug")))
  {
    glPopDebugGroup();
  }

  if (!G.profile_gpu) {
    return;
  }

  Vector<TimeQuery> &queries = frame_timings.last().queries;
  for (int i = queries.size() - 1; i >= 0; i--) {
    TimeQuery &query = queries[i];
    if (!query.finished) {
      query.finished = true;
      glQueryCounter(query.handle_end, GL_TIMESTAMP);
      glGetInteger64v(GL_TIMESTAMP, &query.cpu_end);
      break;
    }
    if (i == 0) {
      CLOG_ERROR(&LOG, "Profile GPU error: Extra GPU_debug_group_end() call.");
    }
  }
}

void GLContext::process_frame_timings()
{
  if (!G.profile_gpu) {
    return;
  }

  for (int frame_i = 0; frame_i < frame_timings.size(); frame_i++) {
    Vector<TimeQuery> &queries = frame_timings[frame_i].queries;

    GLint frame_is_ready = 0;
    bool frame_is_valid = !queries.is_empty();

    for (int i = queries.size() - 1; i >= 0; i--) {
      if (!queries[i].finished) {
        frame_is_valid = false;
        CLOG_ERROR(&LOG, "Profile GPU error: Missing GPU_debug_group_end() call");
      }
      else {
        glGetQueryObjectiv(queries.last().handle_end, GL_QUERY_RESULT_AVAILABLE, &frame_is_ready);
      }
      break;
    }

    if (!frame_is_valid) {
      /* Cleanup. */
      for (TimeQuery &query : queries) {
        glDeleteQueries(2, query.handles);
      }
      frame_timings.remove(frame_i--);
      continue;
    }

    if (!frame_is_ready) {
      break;
    }

    for (TimeQuery &query : queries) {
      GLuint64 gpu_start = 0;
      GLuint64 gpu_end = 0;
      glGetQueryObjectui64v(query.handle_start, GL_QUERY_RESULT, &gpu_start);
      glGetQueryObjectui64v(query.handle_end, GL_QUERY_RESULT, &gpu_end);
      glDeleteQueries(2, query.handles);

      ProfileReport::get().add_group(
          query.name, gpu_start, gpu_end, query.cpu_start, query.cpu_end);
    }

    frame_timings.remove(frame_i--);
  }

  frame_timings.append({});
}

bool GLContext::debug_capture_begin(const char *title)
{
  return GLBackend::get()->debug_capture_begin(title);
}

bool GLBackend::debug_capture_begin(const char *title)
{
#ifdef WITH_RENDERDOC
  if (G.debug & G_DEBUG_GPU_RENDERDOC) {
    bool result = renderdoc_.start_frame_capture(nullptr, nullptr);
    if (result && title) {
      renderdoc_.set_frame_capture_title(title);
    }
    return result;
  }
#endif
  UNUSED_VARS(title);
  return false;
}

void GLContext::debug_capture_end()
{
  GLBackend::get()->debug_capture_end();
}

void GLBackend::debug_capture_end()
{
#ifdef WITH_RENDERDOC
  if (G.debug & G_DEBUG_GPU_RENDERDOC) {
    renderdoc_.end_frame_capture(nullptr, nullptr);
  }
#endif
}

void *GLContext::debug_capture_scope_create(const char *name)
{
  return (void *)name;
}

bool GLContext::debug_capture_scope_begin(void *scope)
{
#ifdef WITH_RENDERDOC
  const char *title = (const char *)scope;
  if (StringRefNull(title) != StringRefNull(G.gpu_debug_scope_name)) {
    return false;
  }
  GLBackend::get()->debug_capture_begin(title);
#else
  UNUSED_VARS(scope);
#endif
  return false;
}

void GLContext::debug_capture_scope_end(void *scope)
{
#ifdef WITH_RENDERDOC
  const char *title = (const char *)scope;
  if (StringRefNull(title) == StringRefNull(G.gpu_debug_scope_name)) {
    GLBackend::get()->debug_capture_end();
  }
#else
  UNUSED_VARS(scope);
#endif
}

void GLContext::debug_unbind_all_ubo()
{
  this->bound_ubo_slots = 0u;
}

void GLContext::debug_unbind_all_ssbo()
{
  this->bound_ssbo_slots = 0u;
}

/** \} */

}  // namespace blender::gpu
