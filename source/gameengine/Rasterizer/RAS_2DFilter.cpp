/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Pierluigi Grassi, Porteries Tristan.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#include "RAS_2DFilter.h"

#include "GPU_framebuffer.hh"
#include "GPU_immediate.hh"
#include "GPU_state.hh"
#include "GPU_uniform_buffer.hh"

#include "EXP_Value.h"
#include "RAS_2DFilterFrameBuffer.h"
#include "RAS_FrameBuffer.h"
#include "RAS_ICanvas.h"

using namespace blender;

extern "C" {
extern char datatoc_RAS_VertexShader2DFilter_glsl[];
}

static std::string predefinedUniformsName[RAS_2DFilter::MAX_PREDEFINED_UNIFORM_TYPE] = {
    "bgl_RenderedTexture",         // RENDERED_TEXTURE_UNIFORM
    "bgl_DepthTexture",            // DEPTH_TEXTURE_UNIFORM
    "bgl_RenderedTextureWidth",    // RENDERED_TEXTURE_WIDTH_UNIFORM
    "bgl_RenderedTextureHeight",   // RENDERED_TEXTURE_HEIGHT_UNIFORM
    "bgl_TextureCoordinateOffset"  // TEXTURE_COORDINATE_OFFSETS_UNIFORM
};

RAS_2DFilter::RAS_2DFilter(RAS_2DFilterData &data)
    : m_properties(data.propertyNames),
      m_gameObject(data.gameObject),
      m_uniformInitialized(false),
      m_mipmap(data.mipmap)
{
  for (unsigned int i = 0; i < TEXTURE_OFFSETS_SIZE; i++) {
    m_textureOffsets[i] = 0;
  }

  for (unsigned int i = 0; i < MAX_PREDEFINED_UNIFORM_TYPE; ++i) {
    m_predefinedUniforms[i] = -1;
  }

  m_progs[VERTEX_PROGRAM] = std::string(datatoc_RAS_VertexShader2DFilter_glsl);
  m_progs[FRAGMENT_PROGRAM] = data.shaderText;

  LinkProgram();
}

RAS_2DFilter::~RAS_2DFilter()
{
}

bool RAS_2DFilter::GetMipmap() const
{
  return m_mipmap;
}

void RAS_2DFilter::SetMipmap(bool mipmap)
{
  m_mipmap = mipmap;
}

RAS_2DFilterFrameBuffer *RAS_2DFilter::GetFrameBuffer() const
{
  return m_frameBuffer.get();
}

void RAS_2DFilter::SetOffScreen(RAS_2DFilterFrameBuffer *frameBuffer)
{
  m_frameBuffer.reset(frameBuffer);
}

void RAS_2DFilter::Initialize(RAS_ICanvas *canvas)
{
  /* The shader must be initialized at the first frame when the canvas is accesible.
   * to solve this we initialize filter at the frist render frame. */
  if (!m_uniformInitialized) {
    ParseShaderProgram();
    ComputeTextureOffsets(canvas);
    /* Set All ubo variables to 0 including padding variables */
    m_uboData = {};
    m_uboData.width = float(canvas->GetWidth() + 1);
    m_uboData.height = float(canvas->GetHeight() + 1);
    /* Same order than when texture offset are computed */
    for (int i = 0; i < 9; ++i) {
      m_uboData.coo_offset[i][0] = m_textureOffsets[i * 2];
      m_uboData.coo_offset[i][1] = m_textureOffsets[i * 2 + 1];
    }
    /* Update ubo before any framebuffer is bound */
    GPU_uniformbuf_update(m_ubo, &m_uboData);
    m_uniformInitialized = true;
  }
}

RAS_FrameBuffer *RAS_2DFilter::Start(RAS_Rasterizer *rasty,
                                     RAS_ICanvas *canvas,
                                     RAS_FrameBuffer *depthfb,
                                     RAS_FrameBuffer *colorfb,
                                     RAS_FrameBuffer *targetfb)
{
  /* The off screen the filter rendered to. If the filter is invalid or uses a custom
   * off screen the output off screen is the same as the input off screen. */
  RAS_FrameBuffer *outpufb = colorfb;
  if (!Ok()) {
    return outpufb;
  }

  /* The target off screen must be not the color input off screen, it can be the same as depth
   * input screen because depth is unchanged. */
  BLI_assert(targetfb != colorfb);

  /* Compute texture offsets */
  Initialize(canvas);

  if (m_frameBuffer) {
    if (!m_frameBuffer->Update(canvas)) {
      return outpufb;
    }
    m_frameBuffer->Bind(rasty);
  }
  else {
    GPU_framebuffer_bind(targetfb->GetFrameBuffer());
    outpufb = targetfb;
  }

  GPUVertFormat *vert_format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(vert_format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
  uint texco = GPU_vertformat_attr_add(
      vert_format, "texCoord", blender::gpu::VertAttrType::SFLOAT_32_32);

  /* bind shader here */
  SetProg(true);

  /* Bind resources */
  BindTextures(depthfb, colorfb);
  BindUniforms(canvas);
  GPU_uniformbuf_bind(m_ubo, GPU_shader_get_ubo_binding(m_shader, "g_data"));

  Update(rasty, MT_Matrix4x4::Identity());

  ApplyShader();

  /* Fullscreen quad */
  immBegin(GPU_PRIM_TRIS, 3);
  immAttr2f(texco, 0.0f, 0.0f);
  immVertex2f(pos, -1.0f, -1.0f);
  immAttr2f(texco, 2.0f, 0.0f);
  immVertex2f(pos, 3.0f, -1.0f);

  immAttr2f(texco, 0.0f, 2.0f);
  immVertex2f(pos, -1.0f, 3.0f);
  immEnd();

  /* Unbind resources */
  UnbindTextures(depthfb, colorfb);
  GPU_uniformbuf_unbind(m_ubo);

  if (m_frameBuffer) {
    m_frameBuffer->Unbind(rasty, canvas);
  }

  return outpufb;
}

void RAS_2DFilter::End()
{
  if (Ok()) {
    SetProg(false);
  }
}

bool RAS_2DFilter::LinkProgram()
{
  if (!RAS_Shader::LinkProgram()) {
    return false;
  }

  m_uniformInitialized = false;

  return true;
}

void RAS_2DFilter::ParseShaderProgram()
{
  // Parse shader to found used uniforms.
  for (unsigned int i = 0; i < MAX_PREDEFINED_UNIFORM_TYPE; ++i) {
    m_predefinedUniforms[i] = GetUniformLocation(predefinedUniformsName[i], false);
  }

  if (m_gameObject) {
    std::vector<std::string> foundProperties;
    for (const std::string &prop : m_properties) {
      const unsigned int loc = GetUniformLocation(prop, false);
      if (loc != -1) {
        m_propertiesLoc.push_back(loc);
        foundProperties.push_back(prop);
      }
    }
    m_properties = foundProperties;
  }
}

/* Fill the textureOffsets array with values used by the shaders to get texture samples
of nearby fragments. Or vertices or whatever.*/
void RAS_2DFilter::ComputeTextureOffsets(RAS_ICanvas *canvas)
{
  const float texturewidth = (float)canvas->GetWidth() + 1;
  const float textureheight = (float)canvas->GetHeight() + 1;
  const float xInc = 1.0f / texturewidth;
  const float yInc = 1.0f / textureheight;

  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      m_textureOffsets[(((i * 3) + j) * 2) + 0] = (-1.0f * xInc) + ((float)i * xInc);
      m_textureOffsets[(((i * 3) + j) * 2) + 1] = (-1.0f * yInc) + ((float)j * yInc);
    }
  }
}

void RAS_2DFilter::BindTextures(RAS_FrameBuffer *depthfb, RAS_FrameBuffer *colorfb)
{
  if (m_predefinedUniforms[RENDERED_TEXTURE_UNIFORM] != -1) {
    GPU_texture_bind(GPU_framebuffer_color_texture(colorfb->GetFrameBuffer()), GPU_shader_get_sampler_binding(m_shader, "bgl_RenderedTexture"));
    GPU_apply_state();
    if (m_mipmap) {
      GPU_framebuffer_mipmap_texture(colorfb->GetFrameBuffer());
    }
  }
  if (m_predefinedUniforms[DEPTH_TEXTURE_UNIFORM] != -1) {
    GPU_texture_bind(GPU_framebuffer_depth_texture(depthfb->GetFrameBuffer()),
                     GPU_shader_get_sampler_binding(m_shader, "bgl_DepthTexture"));
    GPU_apply_state();
  }
}

void RAS_2DFilter::UnbindTextures(RAS_FrameBuffer *depthfb, RAS_FrameBuffer *colorfb)
{
  if (m_predefinedUniforms[RENDERED_TEXTURE_UNIFORM] != -1) {
    GPU_texture_unbind(GPU_framebuffer_color_texture(colorfb->GetFrameBuffer()));
    if (m_mipmap) {
      GPU_framebuffer_unmipmap_texture(colorfb->GetFrameBuffer());
    }
  }
  if (m_predefinedUniforms[DEPTH_TEXTURE_UNIFORM] != -1) {
    GPU_texture_unbind(GPU_framebuffer_depth_texture(depthfb->GetFrameBuffer()));
  }
}

void RAS_2DFilter::BindUniforms(RAS_ICanvas *canvas)
{
  if (m_predefinedUniforms[RENDERED_TEXTURE_UNIFORM] != -1) {
    SetUniform(m_predefinedUniforms[RENDERED_TEXTURE_UNIFORM], 8);
  }
  if (m_predefinedUniforms[DEPTH_TEXTURE_UNIFORM] != -1) {
    SetUniform(m_predefinedUniforms[DEPTH_TEXTURE_UNIFORM], 9);
  }

  for (unsigned int i = 0, size = m_properties.size(); i < size; ++i) {
    const std::string &prop = m_properties[i];
    unsigned int uniformLoc = m_propertiesLoc[i];

    EXP_Value *property = m_gameObject->GetProperty(prop);

    if (!property) {
      continue;
    }

    switch (property->GetValueType()) {
      case VALUE_INT_TYPE:
        SetUniform(uniformLoc, (int)property->GetNumber());
        break;
      case VALUE_FLOAT_TYPE:
        SetUniform(uniformLoc, (float)property->GetNumber());
        break;
      default:
        break;
    }
  }
}
