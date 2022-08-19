/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2016 by Mike Erwin. All rights reserved. */

/** \file
 * \ingroup gpu
 *
 * Mimics old style opengl immediate mode drawing.
 */

#ifndef GPU_STANDALONE
#  include "UI_resources.h"
#endif

#include "GPU_immediate.h"
#include "GPU_matrix.h"
#include "GPU_texture.h"

#include "gpu_context_private.hh"
#include "gpu_immediate_private.hh"
#include "gpu_shader_private.hh"
#include "gpu_vertex_format_private.h"

using namespace blender::gpu;

static thread_local Immediate *imm = nullptr;

void immActivate()
{
  imm = Context::get()->imm;
}

void immDeactivate()
{
  imm = nullptr;
}

GPUVertFormat *immVertexFormat()
{
  GPU_vertformat_clear(&imm->vertex_format);
  return &imm->vertex_format;
}

void immBindShader(GPUShader *shader)
{
  BLI_assert(imm->shader == nullptr);

  imm->shader = shader;
  imm->builtin_shader_bound = GPU_SHADER_TEXT; /* Default value. */

  if (!imm->vertex_format.packed) {
    VertexFormat_pack(&imm->vertex_format);
    imm->enabled_attr_bits = 0xFFFFu & ~(0xFFFFu << imm->vertex_format.attr_len);
  }

  GPU_shader_bind(shader);
  GPU_matrix_bind(shader);
  GPU_shader_set_srgb_uniform(shader);
}

void immBindBuiltinProgram(eGPUBuiltinShader shader_id)
{
  GPUShader *shader = GPU_shader_get_builtin_shader(shader_id);
  immBindShader(shader);
  imm->builtin_shader_bound = shader_id;
}

void immUnbindProgram()
{
  BLI_assert(imm->shader != nullptr);

  GPU_shader_unbind();
  imm->shader = nullptr;
}

GPUShader *immGetShader()
{
  return imm->shader;
}

#ifndef NDEBUG
static bool vertex_count_makes_sense_for_primitive(uint vertex_len, GPUPrimType prim_type)
{
  /* does vertex_len make sense for this primitive type? */
  if (vertex_len == 0) {
    return false;
  }

  switch (prim_type) {
    case GPU_PRIM_POINTS:
      return true;
    case GPU_PRIM_LINES:
      return vertex_len % 2 == 0;
    case GPU_PRIM_LINE_STRIP:
    case GPU_PRIM_LINE_LOOP:
      return vertex_len >= 2;
    case GPU_PRIM_LINE_STRIP_ADJ:
      return vertex_len >= 4;
    case GPU_PRIM_TRIS:
      return vertex_len % 3 == 0;
    case GPU_PRIM_TRI_STRIP:
    case GPU_PRIM_TRI_FAN:
      return vertex_len >= 3;
    default:
      return false;
  }
}
#endif

/* -------------------------------------------------------------------- */
/** \name Wide line workaround
 *
 * Some systems do not support wide lines.
 * We workaround this by using specialized shaders.
 * \{ */

static void wide_line_workaround_start(GPUPrimType prim_type)
{
  if (!ELEM(prim_type, GPU_PRIM_LINES, GPU_PRIM_LINE_STRIP, GPU_PRIM_LINE_LOOP)) {
    return;
  }

  float line_width = GPU_line_width_get();

  if (line_width == 1.0f) {
    /* No need to change the shader. */
    return;
  }

  eGPUBuiltinShader polyline_sh;
  switch (imm->builtin_shader_bound) {
    case GPU_SHADER_3D_CLIPPED_UNIFORM_COLOR:
      polyline_sh = GPU_SHADER_3D_POLYLINE_CLIPPED_UNIFORM_COLOR;
      break;
    case GPU_SHADER_2D_UNIFORM_COLOR:
    case GPU_SHADER_3D_UNIFORM_COLOR:
      polyline_sh = GPU_SHADER_3D_POLYLINE_UNIFORM_COLOR;
      break;
    case GPU_SHADER_2D_FLAT_COLOR:
    case GPU_SHADER_3D_FLAT_COLOR:
      polyline_sh = GPU_SHADER_3D_POLYLINE_FLAT_COLOR;
      break;
    case GPU_SHADER_2D_SMOOTH_COLOR:
    case GPU_SHADER_3D_SMOOTH_COLOR:
      polyline_sh = GPU_SHADER_3D_POLYLINE_SMOOTH_COLOR;
      break;
    default:
      /* Cannot replace the current shader with a polyline shader. */
      return;
  }

  imm->prev_builtin_shader = imm->builtin_shader_bound;

  immUnbindProgram();

  /* TODO(fclem): Don't use geometry shader and use quad instancing with double load. */
  // GPU_vertformat_multiload_enable(imm->vertex_format, 2);

  immBindBuiltinProgram(polyline_sh);

  float viewport[4];
  GPU_viewport_size_get_f(viewport);
  immUniform2fv("viewportSize", &viewport[2]);
  immUniform1f("lineWidth", line_width);

  if (GPU_blend_get() == GPU_BLEND_NONE) {
    /* Disable line smoothing when blending is disabled (see T81827). */
    immUniform1i("lineSmooth", 0);
  }

  if (ELEM(polyline_sh,
           GPU_SHADER_3D_POLYLINE_CLIPPED_UNIFORM_COLOR,
           GPU_SHADER_3D_POLYLINE_UNIFORM_COLOR)) {
    immUniformColor4fv(imm->uniform_color);
  }
}

static void wide_line_workaround_end()
{
  if (imm->prev_builtin_shader) {
    if (GPU_blend_get() == GPU_BLEND_NONE) {
      /* Restore default. */
      immUniform1i("lineSmooth", 1);
    }
    immUnbindProgram();

    immBindBuiltinProgram(imm->prev_builtin_shader);
    imm->prev_builtin_shader = GPU_SHADER_TEXT;
  }
}

/** \} */

void immBegin(GPUPrimType prim_type, uint vertex_len)
{
  BLI_assert(imm->prim_type == GPU_PRIM_NONE); /* Make sure we haven't already begun. */
  BLI_assert(vertex_count_makes_sense_for_primitive(vertex_len, prim_type));

  wide_line_workaround_start(prim_type);

  imm->prim_type = prim_type;
  imm->vertex_len = vertex_len;
  imm->vertex_idx = 0;
  imm->unassigned_attr_bits = imm->enabled_attr_bits;

  imm->vertex_data = imm->begin();
}

void immBeginAtMost(GPUPrimType prim_type, uint vertex_len)
{
  BLI_assert(vertex_len > 0);
  imm->strict_vertex_len = false;
  immBegin(prim_type, vertex_len);
}

GPUBatch *immBeginBatch(GPUPrimType prim_type, uint vertex_len)
{
  BLI_assert(imm->prim_type == GPU_PRIM_NONE); /* Make sure we haven't already begun. */
  BLI_assert(vertex_count_makes_sense_for_primitive(vertex_len, prim_type));

  imm->prim_type = prim_type;
  imm->vertex_len = vertex_len;
  imm->vertex_idx = 0;
  imm->unassigned_attr_bits = imm->enabled_attr_bits;

  GPUVertBuf *verts = GPU_vertbuf_create_with_format(&imm->vertex_format);
  GPU_vertbuf_data_alloc(verts, vertex_len);

  imm->vertex_data = (uchar *)GPU_vertbuf_get_data(verts);

  imm->batch = GPU_batch_create_ex(prim_type, verts, nullptr, GPU_BATCH_OWNS_VBO);
  imm->batch->flag |= GPU_BATCH_BUILDING;

  return imm->batch;
}

GPUBatch *immBeginBatchAtMost(GPUPrimType prim_type, uint vertex_len)
{
  BLI_assert(vertex_len > 0);
  imm->strict_vertex_len = false;
  return immBeginBatch(prim_type, vertex_len);
}

void immEnd()
{
  BLI_assert(imm->prim_type != GPU_PRIM_NONE); /* Make sure we're between a Begin/End pair. */
  BLI_assert(imm->vertex_data || imm->batch);

  if (imm->strict_vertex_len) {
    BLI_assert(imm->vertex_idx == imm->vertex_len); /* With all vertices defined. */
  }
  else {
    BLI_assert(imm->vertex_idx <= imm->vertex_len);
    BLI_assert(imm->vertex_idx == 0 ||
               vertex_count_makes_sense_for_primitive(imm->vertex_idx, imm->prim_type));
  }

  if (imm->batch) {
    if (imm->vertex_idx < imm->vertex_len) {
      GPU_vertbuf_data_resize(imm->batch->verts[0], imm->vertex_idx);
      /* TODO: resize only if vertex count is much smaller */
    }
    GPU_batch_set_shader(imm->batch, imm->shader);
    imm->batch->flag &= ~GPU_BATCH_BUILDING;
    imm->batch = nullptr; /* don't free, batch belongs to caller */
  }
  else {
    imm->end();
  }

  /* Prepare for next immBegin. */
  imm->prim_type = GPU_PRIM_NONE;
  imm->strict_vertex_len = true;
  imm->vertex_data = nullptr;

  wide_line_workaround_end();
}

static void setAttrValueBit(uint attr_id)
{
  uint16_t mask = 1 << attr_id;
  BLI_assert(imm->unassigned_attr_bits & mask); /* not already set */
  imm->unassigned_attr_bits &= ~mask;
}

/* --- generic attribute functions --- */

void immAttr1f(uint attr_id, float x)
{
  GPUVertAttr *attr = &imm->vertex_format.attrs[attr_id];
  BLI_assert(attr_id < imm->vertex_format.attr_len);
  BLI_assert(attr->comp_type == GPU_COMP_F32);
  BLI_assert(attr->comp_len == 1);
  BLI_assert(imm->vertex_idx < imm->vertex_len);
  BLI_assert(imm->prim_type != GPU_PRIM_NONE); /* make sure we're between a Begin/End pair */
  setAttrValueBit(attr_id);

  float *data = (float *)(imm->vertex_data + attr->offset);
  // printf("%s %td %p\n", __FUNCTION__, (GLubyte*)data - imm->buffer_data, data);

  data[0] = x;
}

void immAttr2f(uint attr_id, float x, float y)
{
  GPUVertAttr *attr = &imm->vertex_format.attrs[attr_id];
  BLI_assert(attr_id < imm->vertex_format.attr_len);
  BLI_assert(attr->comp_type == GPU_COMP_F32);
  BLI_assert(attr->comp_len == 2);
  BLI_assert(imm->vertex_idx < imm->vertex_len);
  BLI_assert(imm->prim_type != GPU_PRIM_NONE); /* make sure we're between a Begin/End pair */
  setAttrValueBit(attr_id);

  float *data = (float *)(imm->vertex_data + attr->offset);
  // printf("%s %td %p\n", __FUNCTION__, (GLubyte*)data - imm->buffer_data, data);

  data[0] = x;
  data[1] = y;
}

void immAttr3f(uint attr_id, float x, float y, float z)
{
  GPUVertAttr *attr = &imm->vertex_format.attrs[attr_id];
  BLI_assert(attr_id < imm->vertex_format.attr_len);
  BLI_assert(attr->comp_type == GPU_COMP_F32);
  BLI_assert(attr->comp_len == 3);
  BLI_assert(imm->vertex_idx < imm->vertex_len);
  BLI_assert(imm->prim_type != GPU_PRIM_NONE); /* make sure we're between a Begin/End pair */
  setAttrValueBit(attr_id);

  float *data = (float *)(imm->vertex_data + attr->offset);
  // printf("%s %td %p\n", __FUNCTION__, (GLubyte*)data - imm->buffer_data, data);

  data[0] = x;
  data[1] = y;
  data[2] = z;
}

void immAttr4f(uint attr_id, float x, float y, float z, float w)
{
  GPUVertAttr *attr = &imm->vertex_format.attrs[attr_id];
  BLI_assert(attr_id < imm->vertex_format.attr_len);
  BLI_assert(attr->comp_type == GPU_COMP_F32);
  BLI_assert(attr->comp_len == 4);
  BLI_assert(imm->vertex_idx < imm->vertex_len);
  BLI_assert(imm->prim_type != GPU_PRIM_NONE); /* make sure we're between a Begin/End pair */
  setAttrValueBit(attr_id);

  float *data = (float *)(imm->vertex_data + attr->offset);
  // printf("%s %td %p\n", __FUNCTION__, (GLubyte*)data - imm->buffer_data, data);

  data[0] = x;
  data[1] = y;
  data[2] = z;
  data[3] = w;
}

void immAttr1u(uint attr_id, uint x)
{
  GPUVertAttr *attr = &imm->vertex_format.attrs[attr_id];
  BLI_assert(attr_id < imm->vertex_format.attr_len);
  BLI_assert(attr->comp_type == GPU_COMP_U32);
  BLI_assert(attr->comp_len == 1);
  BLI_assert(imm->vertex_idx < imm->vertex_len);
  BLI_assert(imm->prim_type != GPU_PRIM_NONE); /* make sure we're between a Begin/End pair */
  setAttrValueBit(attr_id);

  uint *data = (uint *)(imm->vertex_data + attr->offset);

  data[0] = x;
}

void immAttr2i(uint attr_id, int x, int y)
{
  GPUVertAttr *attr = &imm->vertex_format.attrs[attr_id];
  BLI_assert(attr_id < imm->vertex_format.attr_len);
  BLI_assert(attr->comp_type == GPU_COMP_I32);
  BLI_assert(attr->comp_len == 2);
  BLI_assert(imm->vertex_idx < imm->vertex_len);
  BLI_assert(imm->prim_type != GPU_PRIM_NONE); /* make sure we're between a Begin/End pair */
  setAttrValueBit(attr_id);

  int *data = (int *)(imm->vertex_data + attr->offset);

  data[0] = x;
  data[1] = y;
}

void immAttr2s(uint attr_id, short x, short y)
{
  GPUVertAttr *attr = &imm->vertex_format.attrs[attr_id];
  BLI_assert(attr_id < imm->vertex_format.attr_len);
  BLI_assert(attr->comp_type == GPU_COMP_I16);
  BLI_assert(attr->comp_len == 2);
  BLI_assert(imm->vertex_idx < imm->vertex_len);
  BLI_assert(imm->prim_type != GPU_PRIM_NONE); /* make sure we're between a Begin/End pair */
  setAttrValueBit(attr_id);

  short *data = (short *)(imm->vertex_data + attr->offset);

  data[0] = x;
  data[1] = y;
}

void immAttr2fv(uint attr_id, const float data[2])
{
  immAttr2f(attr_id, data[0], data[1]);
}

void immAttr3fv(uint attr_id, const float data[3])
{
  immAttr3f(attr_id, data[0], data[1], data[2]);
}

void immAttr4fv(uint attr_id, const float data[4])
{
  immAttr4f(attr_id, data[0], data[1], data[2], data[3]);
}

void immAttr3ub(uint attr_id, uchar r, uchar g, uchar b)
{
  GPUVertAttr *attr = &imm->vertex_format.attrs[attr_id];
  BLI_assert(attr_id < imm->vertex_format.attr_len);
  BLI_assert(attr->comp_type == GPU_COMP_U8);
  BLI_assert(attr->comp_len == 3);
  BLI_assert(imm->vertex_idx < imm->vertex_len);
  BLI_assert(imm->prim_type != GPU_PRIM_NONE); /* make sure we're between a Begin/End pair */
  setAttrValueBit(attr_id);

  uchar *data = imm->vertex_data + attr->offset;
  // printf("%s %td %p\n", __FUNCTION__, data - imm->buffer_data, data);

  data[0] = r;
  data[1] = g;
  data[2] = b;
}

void immAttr4ub(uint attr_id, uchar r, uchar g, uchar b, uchar a)
{
  GPUVertAttr *attr = &imm->vertex_format.attrs[attr_id];
  BLI_assert(attr_id < imm->vertex_format.attr_len);
  BLI_assert(attr->comp_type == GPU_COMP_U8);
  BLI_assert(attr->comp_len == 4);
  BLI_assert(imm->vertex_idx < imm->vertex_len);
  BLI_assert(imm->prim_type != GPU_PRIM_NONE); /* make sure we're between a Begin/End pair */
  setAttrValueBit(attr_id);

  uchar *data = imm->vertex_data + attr->offset;
  // printf("%s %td %p\n", __FUNCTION__, data - imm->buffer_data, data);

  data[0] = r;
  data[1] = g;
  data[2] = b;
  data[3] = a;
}

void immAttr3ubv(uint attr_id, const uchar data[3])
{
  immAttr3ub(attr_id, data[0], data[1], data[2]);
}

void immAttr4ubv(uint attr_id, const uchar data[4])
{
  immAttr4ub(attr_id, data[0], data[1], data[2], data[3]);
}

void immAttrSkip(uint attr_id)
{
  BLI_assert(attr_id < imm->vertex_format.attr_len);
  BLI_assert(imm->vertex_idx < imm->vertex_len);
  BLI_assert(imm->prim_type != GPU_PRIM_NONE); /* make sure we're between a Begin/End pair */
  setAttrValueBit(attr_id);
}

static void immEndVertex() /* and move on to the next vertex */
{
  BLI_assert(imm->prim_type != GPU_PRIM_NONE); /* make sure we're between a Begin/End pair */
  BLI_assert(imm->vertex_idx < imm->vertex_len);

  /* Have all attributes been assigned values?
   * If not, copy value from previous vertex. */
  if (imm->unassigned_attr_bits) {
    BLI_assert(imm->vertex_idx > 0); /* first vertex must have all attributes specified */
    for (uint a_idx = 0; a_idx < imm->vertex_format.attr_len; a_idx++) {
      if ((imm->unassigned_attr_bits >> a_idx) & 1) {
        const GPUVertAttr *a = &imm->vertex_format.attrs[a_idx];

#if 0
        printf("copying %s from vertex %u to %u\n", a->name, imm->vertex_idx - 1, imm->vertex_idx);
#endif

        uchar *data = imm->vertex_data + a->offset;
        memcpy(data, data - imm->vertex_format.stride, a->size);
        /* TODO: consolidate copy of adjacent attributes */
      }
    }
  }

  imm->vertex_idx++;
  imm->vertex_data += imm->vertex_format.stride;
  imm->unassigned_attr_bits = imm->enabled_attr_bits;
}

void immVertex2f(uint attr_id, float x, float y)
{
  immAttr2f(attr_id, x, y);
  immEndVertex();
}

void immVertex3f(uint attr_id, float x, float y, float z)
{
  immAttr3f(attr_id, x, y, z);
  immEndVertex();
}

void immVertex4f(uint attr_id, float x, float y, float z, float w)
{
  immAttr4f(attr_id, x, y, z, w);
  immEndVertex();
}

void immVertex2i(uint attr_id, int x, int y)
{
  immAttr2i(attr_id, x, y);
  immEndVertex();
}

void immVertex2s(uint attr_id, short x, short y)
{
  immAttr2s(attr_id, x, y);
  immEndVertex();
}

void immVertex2fv(uint attr_id, const float data[2])
{
  immAttr2f(attr_id, data[0], data[1]);
  immEndVertex();
}

void immVertex3fv(uint attr_id, const float data[3])
{
  immAttr3f(attr_id, data[0], data[1], data[2]);
  immEndVertex();
}

void immVertex2iv(uint attr_id, const int data[2])
{
  immAttr2i(attr_id, data[0], data[1]);
  immEndVertex();
}

/* --- generic uniform functions --- */

void immUniform1f(const char *name, float x)
{
  GPU_shader_uniform_1f(imm->shader, name, x);
}

void immUniform2f(const char *name, float x, float y)
{
  GPU_shader_uniform_2f(imm->shader, name, x, y);
}

void immUniform2fv(const char *name, const float data[2])
{
  GPU_shader_uniform_2fv(imm->shader, name, data);
}

void immUniform3f(const char *name, float x, float y, float z)
{
  GPU_shader_uniform_3f(imm->shader, name, x, y, z);
}

void immUniform3fv(const char *name, const float data[3])
{
  GPU_shader_uniform_3fv(imm->shader, name, data);
}

void immUniform4f(const char *name, float x, float y, float z, float w)
{
  GPU_shader_uniform_4f(imm->shader, name, x, y, z, w);
}

void immUniform4fv(const char *name, const float data[4])
{
  GPU_shader_uniform_4fv(imm->shader, name, data);
}

void immUniformArray4fv(const char *name, const float *data, int count)
{
  GPU_shader_uniform_4fv_array(imm->shader, name, count, (const float(*)[4])data);
}

void immUniformMatrix4fv(const char *name, const float data[4][4])
{
  GPU_shader_uniform_mat4(imm->shader, name, data);
}

void immUniform1i(const char *name, int x)
{
  GPU_shader_uniform_1i(imm->shader, name, x);
}

void immBindTexture(const char *name, GPUTexture *tex)
{
  int binding = GPU_shader_get_texture_binding(imm->shader, name);
  GPU_texture_bind(tex, binding);
}

void immBindTextureSampler(const char *name, GPUTexture *tex, eGPUSamplerState state)
{
  int binding = GPU_shader_get_texture_binding(imm->shader, name);
  GPU_texture_bind_ex(tex, state, binding, true);
}

void immBindUniformBuf(const char *name, GPUUniformBuf *ubo)
{
  int binding = GPU_shader_get_uniform_block_binding(imm->shader, name);
  GPU_uniformbuf_bind(ubo, binding);
}

/* --- convenience functions for setting "uniform vec4 color" --- */

void immUniformColor4f(float r, float g, float b, float a)
{
  int32_t uniform_loc = GPU_shader_get_builtin_uniform(imm->shader, GPU_UNIFORM_COLOR);
  BLI_assert(uniform_loc != -1);
  float data[4] = {r, g, b, a};
  GPU_shader_uniform_vector(imm->shader, uniform_loc, 4, 1, data);
  /* For wide Line workaround. */
  copy_v4_v4(imm->uniform_color, data);
}

void immUniformColor4fv(const float rgba[4])
{
  immUniformColor4f(rgba[0], rgba[1], rgba[2], rgba[3]);
}

void immUniformColor3f(float r, float g, float b)
{
  immUniformColor4f(r, g, b, 1.0f);
}

void immUniformColor3fv(const float rgb[3])
{
  immUniformColor4f(rgb[0], rgb[1], rgb[2], 1.0f);
}

void immUniformColor3fvAlpha(const float rgb[3], float a)
{
  immUniformColor4f(rgb[0], rgb[1], rgb[2], a);
}

void immUniformColor3ub(uchar r, uchar g, uchar b)
{
  const float scale = 1.0f / 255.0f;
  immUniformColor4f(scale * r, scale * g, scale * b, 1.0f);
}

void immUniformColor4ub(uchar r, uchar g, uchar b, uchar a)
{
  const float scale = 1.0f / 255.0f;
  immUniformColor4f(scale * r, scale * g, scale * b, scale * a);
}

void immUniformColor3ubv(const uchar rgb[3])
{
  immUniformColor3ub(rgb[0], rgb[1], rgb[2]);
}

void immUniformColor3ubvAlpha(const uchar rgb[3], uchar alpha)
{
  immUniformColor4ub(rgb[0], rgb[1], rgb[2], alpha);
}

void immUniformColor4ubv(const uchar rgba[4])
{
  immUniformColor4ub(rgba[0], rgba[1], rgba[2], rgba[3]);
}

#ifndef GPU_STANDALONE

void immUniformThemeColor(int color_id)
{
  float color[4];
  UI_GetThemeColor4fv(color_id, color);
  immUniformColor4fv(color);
}

void immUniformThemeColorAlpha(int color_id, float a)
{
  float color[4];
  UI_GetThemeColor3fv(color_id, color);
  color[3] = a;
  immUniformColor4fv(color);
}

void immUniformThemeColor3(int color_id)
{
  float color[3];
  UI_GetThemeColor3fv(color_id, color);
  immUniformColor3fv(color);
}

void immUniformThemeColorShade(int color_id, int offset)
{
  float color[4];
  UI_GetThemeColorShade4fv(color_id, offset, color);
  immUniformColor4fv(color);
}

void immUniformThemeColorShadeAlpha(int color_id, int color_offset, int alpha_offset)
{
  float color[4];
  UI_GetThemeColorShadeAlpha4fv(color_id, color_offset, alpha_offset, color);
  immUniformColor4fv(color);
}

void immUniformThemeColorBlendShade(int color_id1, int color_id2, float fac, int offset)
{
  float color[4];
  UI_GetThemeColorBlendShade4fv(color_id1, color_id2, fac, offset, color);
  immUniformColor4fv(color);
}

void immUniformThemeColorBlend(int color_id1, int color_id2, float fac)
{
  uint8_t color[3];
  UI_GetThemeColorBlend3ubv(color_id1, color_id2, fac, color);
  immUniformColor3ubv(color);
}

void immThemeColorShadeAlpha(int colorid, int coloffset, int alphaoffset)
{
  uchar col[4];
  UI_GetThemeColorShadeAlpha4ubv(colorid, coloffset, alphaoffset, col);
  immUniformColor4ub(col[0], col[1], col[2], col[3]);
}

#endif /* GPU_STANDALONE */
