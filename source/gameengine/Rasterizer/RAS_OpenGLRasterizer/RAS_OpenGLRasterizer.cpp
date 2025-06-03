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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Rasterizer/RAS_OpenGLRasterizer/RAS_OpenGLRasterizer.cpp
 *  \ingroup bgerastogl
 */

#include "RAS_OpenGLRasterizer.h"

#include <epoxy/gl.h>

#include "GPU_context.hh"
#include "GPU_framebuffer.hh"

#include "CM_Message.h"

RAS_OpenGLRasterizer::ScreenPlane::ScreenPlane()
{
}

RAS_OpenGLRasterizer::ScreenPlane::~ScreenPlane()
{
}

inline void RAS_OpenGLRasterizer::ScreenPlane::Render()
{
}

RAS_OpenGLRasterizer::RAS_OpenGLRasterizer(RAS_Rasterizer *rasterizer) : m_rasterizer(rasterizer)
{
}

RAS_OpenGLRasterizer::~RAS_OpenGLRasterizer()
{
}

void RAS_OpenGLRasterizer::BeginFrame()
{
}

unsigned int *RAS_OpenGLRasterizer::MakeScreenshot(int x, int y, int width, int height)
{
  unsigned int *pixeldata = nullptr;

  if (width && height) {
    pixeldata = (unsigned int *)malloc(sizeof(unsigned int) * width * height);
    GPUFrameBuffer *read_fb = GPU_framebuffer_back_get();
    GPU_framebuffer_read_color(read_fb,
                               x,
                               y,
                               width,
                               height,
                               4,
                               0,
                               GPU_DATA_UBYTE,
                               pixeldata);
  }

  return pixeldata;
}

void RAS_OpenGLRasterizer::DrawOverlayPlane()
{
  m_screenPlane.Render();
}

const unsigned char *RAS_OpenGLRasterizer::GetGraphicsCardVendor()
{
  return (unsigned char *)glGetString(GL_VENDOR);
}

void RAS_OpenGLRasterizer::PrintHardwareInfo()
{
  if (GPU_backend_get_type() == GPU_BACKEND_OPENGL) {
    CM_Message("GL_VENDOR: " << glGetString(GL_VENDOR));
    CM_Message("GL_RENDERER: " << glGetString(GL_RENDERER));
    CM_Message("GL_VERSION: " << glGetString(GL_VERSION));
    CM_Message("GL_SHADING_LANGUAGE_VERSION: " << glGetString(GL_SHADING_LANGUAGE_VERSION));
    bool support = 0;
    CM_Message("Supported Extensions...");
    CM_Message(" GL_ARB_shader_objects supported?       "
               << (epoxy_has_gl_extension("GL_ARB_shader_objects") ? "yes." : "no."));
    CM_Message(" GL_ARB_geometry_shader4 supported?     "
               << (epoxy_has_gl_extension("GL_ARB_geometry_shader4") ? "yes." : "no."));

    support = epoxy_has_gl_extension("GL_ARB_vertex_shader");
    CM_Message(" GL_ARB_vertex_shader supported?        " << (support ? "yes." : "no."));
    if (support) {
      CM_Message(" ----------Details----------");
      int max = 0;
      glGetIntegerv(GL_MAX_VERTEX_UNIFORM_COMPONENTS, (GLint *)&max);
      CM_Message("  Max uniform components." << max);

      glGetIntegerv(GL_MAX_VARYING_FLOATS, (GLint *)&max);
      CM_Message("  Max varying floats." << max);

      glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, (GLint *)&max);
      CM_Message("  Max vertex texture units." << max);

      glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, (GLint *)&max);
      CM_Message("  Max vertex attribs." << max);

      glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, (GLint *)&max);
      CM_Message("  Max combined texture units." << max);
      CM_Message("");
    }

    support = epoxy_has_gl_extension("GL_ARB_fragment_shader");
    CM_Message(" GL_ARB_fragment_shader supported?      " << (support ? "yes." : "no."));
    if (support) {
      CM_Message(" ----------Details----------");
      int max = 0;
      glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, (GLint *)&max);
      CM_Message("  Max uniform components." << max);
      CM_Message("");
    }

    support = epoxy_has_gl_extension("GL_ARB_texture_cube_map");
    CM_Message(" GL_ARB_texture_cube_map supported?     " << (support ? "yes." : "no."));
    if (support) {
      CM_Message(" ----------Details----------");
      int size = 0;
      glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE, (GLint *)&size);
      CM_Message("  Max cubemap size." << size);
      CM_Message("");
    }

    support = epoxy_has_gl_extension("GL_ARB_multitexture");
    CM_Message(" GL_ARB_multitexture supported?         " << (support ? "yes." : "no."));
    if (support) {
      CM_Message(" ----------Details----------");
      int units = 0;
      glGetIntegerv(GL_MAX_TEXTURE_UNITS, (GLint *)&units);
      CM_Message("  Max texture units available.  " << units);
      CM_Message("");
    }

    CM_Message(" GL_ARB_texture_env_combine supported?  "
               << (epoxy_has_gl_extension("GL_ARB_texture_env_combine") ? "yes." : "no."));

    CM_Message(" GL_ARB_draw_instanced supported?  "
               << (epoxy_has_gl_extension("GL_ARB_draw_instanced") ? "yes." : "no."));
  }
}
