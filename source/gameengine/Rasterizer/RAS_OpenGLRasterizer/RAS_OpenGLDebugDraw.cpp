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

#include "RAS_OpenGLDebugDraw.h"

#include "BLF_api.h"
#include "DRW_render.h"
#include "GPU_immediate.h"
#include "GPU_matrix.h"

#include "KX_Camera.h"
#include "KX_Globals.h"
#include "RAS_ICanvas.h"

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
  //// Draw aabbs USE imm API
  //for (const RAS_DebugDraw::Aabb &aabb : debugDraw->m_aabbs) {

  //  const MT_Matrix3x3 &rot = aabb.m_rot;
  //  const MT_Vector3 &pos = aabb.m_pos;
  //  float mat[16] = {rot[0][0],
  //                   rot[1][0],
  //                   rot[2][0],
  //                   0.0,
  //                   rot[0][1],
  //                   rot[1][1],
  //                   rot[2][1],
  //                   0.0,
  //                   rot[0][2],
  //                   rot[1][2],
  //                   rot[2][2],
  //                   0.0,
  //                   pos[0],
  //                   pos[1],
  //                   pos[2],
  //                   1.0};

  //  const MT_Vector3 &min = aabb.m_min;
  //  const MT_Vector3 &max = aabb.m_max;

  //  GLfloat vertexes[24] = {
  //      (float)min[0], (float)min[1], (float)min[2], (float)max[0], (float)min[1], (float)min[2],
  //      (float)max[0], (float)max[1], (float)min[2], (float)min[0], (float)max[1], (float)min[2],
  //      (float)min[0], (float)min[1], (float)max[2], (float)max[0], (float)min[1], (float)max[2],
  //      (float)max[0], (float)max[1], (float)max[2], (float)min[0], (float)max[1], (float)max[2]};

  //  // rasty->PushMatrix();
  //  // rasty->MultMatrix(mat);

  //  float c[4];
  //  aabb.m_color.getValue(c);

  //  float mvp[16];
  //  MT_Matrix4x4 obmat(mat);
  //  MT_Matrix4x4 m(m_cameraMatrix * obmat);
  //  m.getValue(mvp);

  //  /*BindVBO(mvp, c, vertexes, m_wireibo);
  //  glDrawElements(GL_LINES, 24, GL_UNSIGNED_BYTE, 0);
  //  UnbindVBO();*/
  //}

  //// Draw boxes. USE imm API
  //for (const RAS_DebugDraw::SolidBox &box : debugDraw->m_solidBoxes) {
  //  GLfloat vertexes[24];
  //  int k = 0;
  //  for (int i = 0; i < 8; i++) {
  //    for (int j = 0; j < 3; j++) {
  //      vertexes[k] = box.m_vertices[i][j];
  //      k++;
  //    }
  //  }
  //  float c[4];
  //  box.m_color.getValue(c);
  //  /*BindVBO(mvp, c, vertexes, m_wireibo);
  //  glDrawElements(GL_LINES, 24, GL_UNSIGNED_BYTE, 0);
  //  UnbindVBO();*/

  //  //rasty->SetFrontFace(false);
  //  //GPU_front_facing(bool invert)
  //  box.m_insideColor.getValue(c);
  //  /*BindVBO(mvp, c, vertexes, m_solidibo);
  //  glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_BYTE, 0);
  //  UnbindVBO();*/

  //  //rasty->SetFrontFace(true);
  //  //GPU_front_facing(bool invert);
  //  box.m_outsideColor.getValue(c);
  //  /*BindVBO(mvp, c, vertexes, m_solidibo);
  //  glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_BYTE, 0);
  //  UnbindVBO();*/
  //}

  /* Draw Debug lines */

  if (debugDraw->m_lines.size()) {
    GPUVertFormat *format = immVertexFormat();
    uint pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
    uint col = GPU_vertformat_attr_add(format, "color", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);
    immBindBuiltinProgram(GPU_SHADER_3D_FLAT_COLOR);

    bContext *C = KX_GetActiveEngine()->GetContext();
    RegionView3D *rv3d = CTX_wm_region_view3d(C);
    GPU_matrix_push();
    GPU_matrix_push_projection();
    GPU_matrix_projection_set(rv3d->winmat);
    GPU_matrix_set(rv3d->viewmat);

    GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
    GPU_line_smooth(true);
    GPU_line_width(1.0f);
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

    /* Reset defaults */
    GPU_line_smooth(false);
    GPU_matrix_pop();
    GPU_matrix_pop_projection();
    GPU_depth_test(GPU_DEPTH_ALWAYS);
    GPU_face_culling(GPU_CULL_NONE);

    immUnbindProgram();
  }

  /* The Performances profiler */

  if (debugDraw->m_boxes2D.size()) {
    GPU_depth_test(GPU_DEPTH_NONE);
    // rasty->Disable(RAS_Rasterizer::RAS_DEPTH_TEST);
    /* Warning: I didn't find the equivalent in GPU_ API */
    // rasty->DisableForText();
    GPU_face_culling(GPU_CULL_BACK);

    const unsigned int width = canvas->GetWidth();
    const unsigned int height = canvas->GetHeight();
    GPU_matrix_ortho_set(0, width, 0, height, -100, 100);

    GPUVertFormat *format = immVertexFormat();
    uint pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);

    immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);
    for (const RAS_DebugDraw::Box2D &box2d : debugDraw->m_boxes2D) {
      const float xco = box2d.m_pos.x();
      const float yco = height - box2d.m_pos.y();
      const float xsize = box2d.m_size.x();
      const float ysize = box2d.m_size.y();

      immUniformColor4fv(box2d.m_color.getValue());
      immRectf(pos, xco + 1 + xsize, yco + ysize, xco, yco);
    }
    immUnbindProgram();

    DRW_state_reset();

    BLF_size(blf_mono_font, 11, 72);

    BLF_enable(blf_mono_font, BLF_SHADOW);

    static float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    BLF_shadow(blf_mono_font, 1, black);
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
    GPU_depth_test(GPU_DEPTH_ALWAYS);
    GPU_face_culling(GPU_CULL_NONE);
  }
}
