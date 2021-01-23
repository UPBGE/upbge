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

/** \file gameengine/Rasterizer/RAS_DebugDraw.cpp
 *  \ingroup bgerastogl
 */

#include "RAS_DebugDraw.h"

#include "MT_Frustum.h"
#include "RAS_OpenGLDebugDraw.h"

RAS_DebugDraw::RAS_DebugDraw()
{
  m_impl = new RAS_OpenGLDebugDraw();
}
RAS_DebugDraw::~RAS_DebugDraw()
{
  delete m_impl;
}

RAS_DebugDraw::Shape::Shape(const MT_Vector4 &color) : m_color(color)
{
}

RAS_DebugDraw::Line::Line(const MT_Vector3 &from, const MT_Vector3 &to, const MT_Vector4 &color)
    : Shape(color), m_from(from), m_to(to)
{
}

RAS_DebugDraw::Circle::Circle(const MT_Vector3 &center,
                              const MT_Vector3 &normal,
                              float radius,
                              int sector,
                              const MT_Vector4 &color)
    : Shape(color), m_center(center), m_normal(normal), m_radius(radius), m_sector(sector)
{
}

RAS_DebugDraw::Aabb::Aabb(const MT_Vector3 &pos,
                          const MT_Matrix3x3 &rot,
                          const MT_Vector3 &min,
                          const MT_Vector3 &max,
                          const MT_Vector4 &color)
    : Shape(color), m_pos(pos), m_rot(rot), m_min(min), m_max(max)
{
}

RAS_DebugDraw::Box::Box(const std::array<MT_Vector3, 8> &vertices, const MT_Vector4 &color)
    : Shape(color), m_vertices(vertices)
{
}

RAS_DebugDraw::SolidBox::SolidBox(const MT_Vector4 &insideColor,
                                  const MT_Vector4 &outsideColor,
                                  const std::array<MT_Vector3, 8> &vertices,
                                  const MT_Vector4 &color)
    : Box(vertices, color), m_insideColor(insideColor), m_outsideColor(outsideColor)
{
}

RAS_DebugDraw::Text2D::Text2D(const std::string &text,
                              const MT_Vector2 &pos,
                              const MT_Vector4 &color)
    : Shape(color), m_text(text), m_pos(pos)
{
}

RAS_DebugDraw::Box2D::Box2D(const MT_Vector2 &pos, const MT_Vector2 &size, const MT_Vector4 &color)
    : Shape(color), m_pos(pos), m_size(size)
{
}

void RAS_DebugDraw::DrawLine(const MT_Vector3 &from, const MT_Vector3 &to, const MT_Vector4 &color)
{
  m_lines.emplace_back(from, to, color);
}

void RAS_DebugDraw::DrawCircle(const MT_Vector3 &center,
                               const MT_Scalar radius,
                               const MT_Vector4 &color,
                               const MT_Vector3 &normal,
                               int nsector)
{
  m_circles.emplace_back(center, normal, radius, nsector, color);
}

void RAS_DebugDraw::DrawAabb(const MT_Vector3 &pos,
                             const MT_Matrix3x3 &rot,
                             const MT_Vector3 &min,
                             const MT_Vector3 &max,
                             const MT_Vector4 &color)
{
  m_aabbs.emplace_back(pos, rot, min, max, color);
}

void RAS_DebugDraw::DrawBox(const std::array<MT_Vector3, 8> &vertices, const MT_Vector4 &color)
{
  m_boxes.emplace_back(vertices, color);
}

void RAS_DebugDraw::DrawSolidBox(const std::array<MT_Vector3, 8> &vertices,
                                 const MT_Vector4 &insideColor,
                                 const MT_Vector4 &outsideColor,
                                 const MT_Vector4 &lineColor)
{
  m_solidBoxes.emplace_back(insideColor, outsideColor, vertices, lineColor);
}

void RAS_DebugDraw::DrawCameraFrustum(const MT_Matrix4x4 &persmat)
{
  std::array<MT_Vector3, 8> box;
  MT_FrustumBox(persmat.inverse(), box);

  DrawSolidBox(box,
               MT_Vector4(0.4f, 0.4f, 0.4f, 0.4f),
               MT_Vector4(0.0f, 0.0f, 0.0f, 0.4f),
               MT_Vector4(0.8f, 0.5f, 0.0f, 1.0f));
}

void RAS_DebugDraw::RenderBox2D(const MT_Vector2 &pos,
                                const MT_Vector2 &size,
                                const MT_Vector4 &color)
{
  m_boxes2D.emplace_back(pos, size, color);
}

void RAS_DebugDraw::RenderText2D(const std::string &text,
                                 const MT_Vector2 &size,
                                 const MT_Vector4 &color)
{
  m_texts2D.emplace_back(text, size, color);
}

void RAS_DebugDraw::Flush(RAS_Rasterizer *rasty, RAS_ICanvas *canvas)
{
  /*if ((m_lines.size() + m_circles.size() + m_aabbs.size() + m_boxes.size() + m_solidBoxes.size()
  + m_texts2D.size() + m_boxes2D.size()) == 0) { return;
  }*/

  m_impl->Flush(rasty, canvas, this);

  m_lines.clear();
  m_circles.clear();
  m_aabbs.clear();
  m_boxes.clear();
  m_solidBoxes.clear();
  m_texts2D.clear();
  m_boxes2D.clear();
}
