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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Rasterizer/RAS_OpenGLRasterizer/RAS_OpenGLDebugDraw.cpp
 *  \ingroup bgerastogl
 */

#include "RAS_DebugDraw.h"

#include "BLF_api.hh"
#include "../draw/intern/draw_command.hh"
#include "DRW_render.hh"
#include "GPU_immediate.hh"
#include "GPU_matrix.hh"

#include "KX_Camera.h"
#include "KX_Globals.h"
#include "KX_KetsjiEngine.h"
#include "RAS_ICanvas.h"
#include "RAS_OpenGLDebugDraw.h"

RAS_OpenGLDebugDraw::RAS_OpenGLDebugDraw()
{
}

RAS_OpenGLDebugDraw::~RAS_OpenGLDebugDraw()
{
}

void RAS_OpenGLDebugDraw::BindVBO(float *mvp, float color[4], float *vertexes, unsigned int ibo)
{
}

void RAS_OpenGLDebugDraw::UnbindVBO()
{
}

void RAS_OpenGLDebugDraw::Flush(RAS_Rasterizer *rasty,
                                RAS_ICanvas *canvas,
                                RAS_DebugDraw *debugDraw)
{
  if (KX_GetActiveEngine()->UseViewportRender()) {
    /* Draw Debug lines */
    if (!debugDraw->m_lines.empty()) {
      for (int i = 0; i < debugDraw->m_lines.size(); i++) {
        RAS_DebugDraw::Line l = debugDraw->m_lines[i];
        DRW_debug_line_bge(l.m_from.getValue(), l.m_to.getValue(), l.m_color.getValue());
      }
    }
    /* The Performances profiler */
    const unsigned int left = canvas->GetViewportArea().GetLeft();
    const unsigned int top = canvas->GetWindowArea().GetTop() -
                             canvas->GetViewportArea().GetBottom();
    if (!debugDraw->m_boxes2D.empty()) {
      for (const RAS_DebugDraw::Box2D &b : debugDraw->m_boxes2D) {
        DRW_debug_box_2D_bge(left + b.m_pos[0], top - b.m_pos[1], b.m_size[0], b.m_size[1]);
      }
    }
    if (!debugDraw->m_texts2D.empty()) {
      for (const RAS_DebugDraw::Text2D &t : debugDraw->m_texts2D) {
        DRW_debug_text_2D_bge(left + t.m_pos[0], top - t.m_pos[1], t.m_text.c_str());
      }
    }
  }
  else {  // Non viewport render pipeline
    if (!debugDraw->m_lines.empty()) {
      KX_Scene *scene = KX_GetActiveScene();
      KX_Camera *cam = scene->GetActiveCamera();
      float proj[4][4];
      float view[4][4];
      cam->GetProjectionMatrix().getValue(&proj[0][0]);
      cam->GetModelviewMatrix().getValue(&view[0][0]);
      GPU_matrix_push();
      GPU_matrix_push_projection();
      GPU_matrix_projection_set(proj);
      GPU_matrix_set(view);

      GPU_line_smooth(true);
      GPU_line_width(1.0f);

      GPUVertFormat *format = immVertexFormat();
      uint pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
      uint col = GPU_vertformat_attr_add(format, "color", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);
      immBindBuiltinProgram(GPU_SHADER_3D_FLAT_COLOR);

      immBegin(GPU_PRIM_LINES, 2 * debugDraw->m_lines.size());
      for (int i = 0; i < debugDraw->m_lines.size(); i++) {
        RAS_DebugDraw::Line line = debugDraw->m_lines[i];
        const MT_Vector3 &min = line.m_from;
        const MT_Vector3 &max = line.m_to;
        immAttr4fv(col, line.m_color.getValue());
        immVertex3fv(pos, min.getValue());
        immVertex3fv(pos, max.getValue());
      }
      immEnd();
      immUnbindProgram();

      /* Reset defaults */
      GPU_line_smooth(false);
      GPU_matrix_pop();
      GPU_matrix_pop_projection();
    }

#ifdef WITH_PYTHON
    KX_GetActiveScene()->RunDrawingCallbacks(KX_Scene::POST_DRAW, nullptr);
#endif

    /* Restore default states + depth always (default bge depth test)
     * (Post processing draw callbacks can have modify gpu states) */
    blender::draw::command::StateSet::set();
    GPU_depth_test(GPU_DEPTH_ALWAYS);

    /* The Performances profiler */
    const unsigned int height = canvas->GetHeight();

    if (!debugDraw->m_boxes2D.empty()) {
      GPUVertFormat *format = immVertexFormat();
      uint pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);

      immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
      for (const RAS_DebugDraw::Box2D &box2d : debugDraw->m_boxes2D) {
        const float xco = box2d.m_pos.x();
        const float yco = height - box2d.m_pos.y();
        const float xsize = box2d.m_size.x();
        const float ysize = box2d.m_size.y();

        immUniformColor4fv(box2d.m_color.getValue());
        immRectf(pos, xco + 1 + xsize, yco + ysize, xco, yco);
      }
      immUnbindProgram();
    }

    if (!debugDraw->m_texts2D.empty()) {
      short profile_size = KX_GetActiveScene()->GetBlenderScene()->gm.profileSize;
      int fontSize = 10;
      switch (profile_size) {
        case 0: // Don't change defaut size
          break;
        case 1: {
          fontSize = 15;
          break;
        }
        case 2: {
          fontSize = 20;
          break;
        }
        default:
          break;
      }

      BLF_size(blf_mono_font, fontSize);

      BLF_enable(blf_mono_font, BLF_SHADOW);

      static float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      BLF_shadow(blf_mono_font, FontShadowType::Blur3x3, black);
      BLF_shadow_offset(blf_mono_font, 1, 1);

      for (const RAS_DebugDraw::Text2D &text2d : debugDraw->m_texts2D) {
        std::string text = text2d.m_text;
        const float xco = text2d.m_pos.x();
        const float yco = height - text2d.m_pos.y();
        float col[4];
        text2d.m_color.getValue(col);

        BLF_color4fv(blf_mono_font, col);
        BLF_position(blf_mono_font, xco, yco, 0.0f);
        BLF_draw(blf_mono_font, text.c_str(), text.size());
      }
      BLF_disable(blf_mono_font, BLF_SHADOW);
    }
  }
}
