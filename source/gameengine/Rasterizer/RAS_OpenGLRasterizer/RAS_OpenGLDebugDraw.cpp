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

#include "../draw/intern/draw_command.hh"
#include "BLF_api.hh"
#include "DNA_scene_types.h"
#include "GPU_batch.hh"
#include "GPU_immediate.hh"
#include "GPU_vertex_buffer.hh"

#include "KX_Camera.h"
#include "KX_Globals.h"
#include "RAS_ICanvas.h"
#include "RAS_OpenGLDebugDraw.h"

using namespace blender;

RAS_OpenGLDebugDraw::RAS_OpenGLDebugDraw() {}

RAS_OpenGLDebugDraw::~RAS_OpenGLDebugDraw() {}

void RAS_OpenGLDebugDraw::BindVBO(float *mvp, float color[4], float *vertexes, unsigned int ibo) {}

void RAS_OpenGLDebugDraw::UnbindVBO() {}

void RAS_OpenGLDebugDraw::DrawDebugLinesBatch(const std::vector<RAS_DebugDraw::Line> &lines)
{
  using namespace blender::gpu;

  GPUVertFormat format = {};
  GPU_vertformat_attr_add(&format, "pos", VertAttrType::SFLOAT_32_32_32);
  GPU_vertformat_attr_add(&format, "color", VertAttrType::SFLOAT_32_32_32_32);

  const int vert_count = lines.size() * 2;
  VertBuf *vbo = GPU_vertbuf_create_with_format_ex(format, GPU_USAGE_DYNAMIC);
  GPU_vertbuf_data_alloc(*vbo, vert_count);

  struct Vertex {
    float pos[3];
    float color[4];
  };

  Vertex *verts = reinterpret_cast<Vertex *>(vbo->data<Vertex>().data());

  for (size_t i = 0; i < lines.size(); ++i) {
    const RAS_DebugDraw::Line &line = lines[i];
    memcpy(verts[2 * i + 0].pos, line.m_from.getValue(), sizeof(float) * 3);
    memcpy(verts[2 * i + 1].pos, line.m_to.getValue(), sizeof(float) * 3);
    memcpy(verts[2 * i + 0].color, line.m_color.getValue(), sizeof(float) * 4);
    memcpy(verts[2 * i + 1].color, line.m_color.getValue(), sizeof(float) * 4);
  }

  Batch *batch = GPU_batch_create_ex(GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
  blender::gpu::Shader *shader = GPU_shader_get_builtin_shader(GPU_SHADER_3D_FLAT_COLOR);
  GPU_batch_set_shader(batch, shader);
  GPU_batch_draw(batch);
  GPU_batch_discard(batch);
}

void RAS_OpenGLDebugDraw::Flush(RAS_Rasterizer *rasty,
                                RAS_ICanvas *canvas,
                                RAS_DebugDraw *debugDraw,
                                const RAS_DebugDraw::FixedSnapshotMode fixed_snapshot_mode,
                                const void *fixed_snapshot_context)
{
  const auto for_each_fixed_buffer = [&](auto &&function) {
    switch (fixed_snapshot_mode) {
      case RAS_DebugDraw::FixedSnapshotMode::None:
        break;
      case RAS_DebugDraw::FixedSnapshotMode::Context: {
        const auto found = debugDraw->m_fixedStepPublished.find(fixed_snapshot_context);
        if (found != debugDraw->m_fixedStepPublished.end()) {
          function(found->second);
        }
        break;
      }
      case RAS_DebugDraw::FixedSnapshotMode::All:
        for (const auto &item : debugDraw->m_fixedStepPublished) {
          function(item.second);
        }
        break;
    }
  };

  if (KX_GetActiveEngine()->UseViewportRender()) {
    DRW_start_debug_bge_viewport();

    /* Draw Debug lines */
    for_each_fixed_buffer([&](const RAS_DebugDraw::FixedStepBuffer &buffer) {
      for (const RAS_DebugDraw::Line &line : buffer.lines) {
        DRW_debug_line_bge(line.m_from.getValue(), line.m_to.getValue(), line.m_color.getValue());
      }
    });
    if (!debugDraw->m_lines.empty()) {
      for (const RAS_DebugDraw::Line &line : debugDraw->m_lines) {
        DRW_debug_line_bge(line.m_from.getValue(), line.m_to.getValue(), line.m_color.getValue());
      }
    }
    /* The Performances profiler */
    const unsigned int left = canvas->GetViewportArea().GetLeft();
    const unsigned int top = canvas->GetWindowArea().GetTop() -
                             canvas->GetViewportArea().GetBottom();
    for_each_fixed_buffer([&](const RAS_DebugDraw::FixedStepBuffer &buffer) {
      for (const RAS_DebugDraw::Box2D &box : buffer.boxes_2d) {
        DRW_debug_box_2D_bge(
            left + box.m_pos[0], top - box.m_pos[1], box.m_size[0], box.m_size[1]);
      }
      for (const RAS_DebugDraw::Rect2D &rect : buffer.rects_2d) {
        DRW_debug_rect_2D_bge(left + rect.m_pos[0],
                              top - rect.m_pos[1] - rect.m_size[1],
                              rect.m_size[0],
                              rect.m_size[1],
                              rect.m_color.getValue());
      }
      for (const RAS_DebugDraw::Line2D &line : buffer.lines_2d) {
        DRW_debug_line_2D_bge(left + line.m_from[0],
                              top - line.m_from[1],
                              left + line.m_to[0],
                              top - line.m_to[1],
                              line.m_color.getValue(),
                              line.m_width);
      }
      for (const RAS_DebugDraw::Text2D &text : buffer.texts_2d) {
        DRW_debug_text_2D_bge(left + text.m_pos[0], top - text.m_pos[1], text.m_text.c_str());
      }
    });
    if (!debugDraw->m_boxes2D.empty()) {
      for (const RAS_DebugDraw::Box2D &b : debugDraw->m_boxes2D) {
        DRW_debug_box_2D_bge(left + b.m_pos[0], top - b.m_pos[1], b.m_size[0], b.m_size[1]);
      }
    }
    if (!debugDraw->m_rects2D.empty()) {
      for (const RAS_DebugDraw::Rect2D &r : debugDraw->m_rects2D) {
        DRW_debug_rect_2D_bge(left + r.m_pos[0],
                              top - r.m_pos[1] - r.m_size[1],
                              r.m_size[0],
                              r.m_size[1],
                              r.m_color.getValue());
      }
    }
    if (!debugDraw->m_lines2D.empty()) {
      for (const RAS_DebugDraw::Line2D &l : debugDraw->m_lines2D) {
        DRW_debug_line_2D_bge(left + l.m_from[0],
                              top - l.m_from[1],
                              left + l.m_to[0],
                              top - l.m_to[1],
                              l.m_color.getValue(),
                              l.m_width);
      }
    }
    if (!debugDraw->m_texts2D.empty()) {
      for (const RAS_DebugDraw::Text2D &t : debugDraw->m_texts2D) {
        DRW_debug_text_2D_bge(left + t.m_pos[0], top - t.m_pos[1], t.m_text.c_str());
      }
    }
  }
  else {  // Non viewport render pipeline
    bool has_fixed_lines = false;
    for_each_fixed_buffer([&](const RAS_DebugDraw::FixedStepBuffer &buffer) {
      has_fixed_lines |= !buffer.lines.empty();
    });
    KX_Scene *scene = KX_GetActiveScene();
    /* A scene can lose its active camera (removed; split screen then draws the other players). */
    KX_Camera *cam = scene ? scene->GetActiveCamera() : nullptr;
    if (cam != nullptr && (has_fixed_lines || !debugDraw->m_lines.empty())) {
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

      for_each_fixed_buffer([&](const RAS_DebugDraw::FixedStepBuffer &buffer) {
        if (!buffer.lines.empty()) {
          DrawDebugLinesBatch(buffer.lines);
        }
      });
      if (!debugDraw->m_lines.empty()) {
        DrawDebugLinesBatch(debugDraw->m_lines);
      }

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

    for_each_fixed_buffer([&](const RAS_DebugDraw::FixedStepBuffer &buffer) {
      if (!buffer.boxes_2d.empty()) {
        GPUVertFormat *format = immVertexFormat();
        uint pos = GPU_vertformat_attr_add(
            format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
        immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
        for (const RAS_DebugDraw::Box2D &box : buffer.boxes_2d) {
          const float x = box.m_pos.x();
          const float y = height - box.m_pos.y();
          immUniformColor4fv(box.m_color.getValue());
          immRectf(pos, x + 1 + box.m_size.x(), y + box.m_size.y(), x, y);
        }
        immUnbindProgram();
      }
    });
    if (!debugDraw->m_boxes2D.empty()) {
      GPUVertFormat *format = immVertexFormat();
      uint pos = GPU_vertformat_attr_add(format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);

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

    for_each_fixed_buffer([&](const RAS_DebugDraw::FixedStepBuffer &buffer) {
      if (!buffer.rects_2d.empty()) {
        GPUVertFormat *format = immVertexFormat();
        uint pos = GPU_vertformat_attr_add(
            format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
        immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
        for (const RAS_DebugDraw::Rect2D &rect : buffer.rects_2d) {
          const float x = rect.m_pos.x();
          const float y = height - rect.m_pos.y() - rect.m_size.y();
          immUniformColor4fv(rect.m_color.getValue());
          immRectf(pos, x, y, x + rect.m_size.x(), y + rect.m_size.y());
        }
        immUnbindProgram();
      }
    });
    if (!debugDraw->m_rects2D.empty()) {
      GPUVertFormat *format = immVertexFormat();
      uint pos = GPU_vertformat_attr_add(format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);

      immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
      for (const RAS_DebugDraw::Rect2D &rect2d : debugDraw->m_rects2D) {
        const float xco = rect2d.m_pos.x();
        const float yco = height - rect2d.m_pos.y() - rect2d.m_size.y();
        const float xsize = rect2d.m_size.x();
        const float ysize = rect2d.m_size.y();

        immUniformColor4fv(rect2d.m_color.getValue());
        immRectf(pos, xco, yco, xco + xsize, yco + ysize);
      }
      immUnbindProgram();
    }

    for_each_fixed_buffer([&](const RAS_DebugDraw::FixedStepBuffer &buffer) {
      if (!buffer.lines_2d.empty()) {
        GPUVertFormat *format = immVertexFormat();
        uint pos = GPU_vertformat_attr_add(
            format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32_32);
        uint color = GPU_vertformat_attr_add(
            format, "color", blender::gpu::VertAttrType::SFLOAT_32_32_32_32);
        immBindBuiltinProgram(GPU_SHADER_3D_FLAT_COLOR);
        GPU_line_smooth(false);
        for (const RAS_DebugDraw::Line2D &line : buffer.lines_2d) {
          float color_value[4];
          line.m_color.getValue(color_value);
          GPU_line_width(line.m_width > 1.0f ? line.m_width : 1.0f);
          immBegin(GPU_PRIM_LINES, 2);
          immAttr4fv(color, color_value);
          immVertex3f(pos, line.m_from.x(), height - line.m_from.y(), 0.0f);
          immAttr4fv(color, color_value);
          immVertex3f(pos, line.m_to.x(), height - line.m_to.y(), 0.0f);
          immEnd();
        }
        GPU_line_smooth(false);
        GPU_line_width(1.0f);
        immUnbindProgram();
      }
    });
    if (!debugDraw->m_lines2D.empty()) {
      GPUVertFormat *format = immVertexFormat();
      uint pos = GPU_vertformat_attr_add(
          format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32_32);
      uint color = GPU_vertformat_attr_add(
          format, "color", blender::gpu::VertAttrType::SFLOAT_32_32_32_32);

      immBindBuiltinProgram(GPU_SHADER_3D_FLAT_COLOR);
      GPU_line_smooth(false);
      for (const RAS_DebugDraw::Line2D &line2d : debugDraw->m_lines2D) {
        float color_value[4];
        line2d.m_color.getValue(color_value);
        GPU_line_width(line2d.m_width > 1.0f ? line2d.m_width : 1.0f);
        immBegin(GPU_PRIM_LINES, 2);
        immAttr4fv(color, color_value);
        immVertex3f(pos, line2d.m_from.x(), height - line2d.m_from.y(), 0.0f);
        immAttr4fv(color, color_value);
        immVertex3f(pos, line2d.m_to.x(), height - line2d.m_to.y(), 0.0f);
        immEnd();
      }
      GPU_line_smooth(false);
      GPU_line_width(1.0f);
      immUnbindProgram();
    }

    bool has_fixed_text = false;
    for_each_fixed_buffer([&](const RAS_DebugDraw::FixedStepBuffer &buffer) {
      has_fixed_text |= !buffer.texts_2d.empty();
    });
    if (has_fixed_text || !debugDraw->m_texts2D.empty()) {
      short profile_size = KX_GetActiveScene()->GetBlenderScene()->gm.profileSize;
      int fontSize = 10;
      switch (profile_size) {
        case 0:  // Don't change defaut size
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

      for_each_fixed_buffer([&](const RAS_DebugDraw::FixedStepBuffer &buffer) {
        for (const RAS_DebugDraw::Text2D &text : buffer.texts_2d) {
          float color[4];
          text.m_color.getValue(color);
          BLF_color4fv(blf_mono_font, color);
          BLF_position(blf_mono_font, text.m_pos.x(), height - text.m_pos.y(), 0.0f);
          BLF_draw(blf_mono_font, text.m_text.c_str(), text.m_text.size());
        }
      });
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
