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

/** \file RAS_DebugDraw.h
 *  \ingroup bgerast
 */

#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "MT_Matrix4x4.h"
#include "MT_Vector2.h"
#include "MT_Vector3.h"
#include "MT_Vector4.h"

class RAS_Rasterizer;
class RAS_ICanvas;
class RAS_OpenGLDebugDraw;

class RAS_DebugDraw {
  friend RAS_OpenGLDebugDraw;

 private:
  struct Shape {
    Shape(const MT_Vector4 &color);
    MT_Vector4 m_color;
  };

  struct Line : Shape {
    Line(const MT_Vector3 &from, const MT_Vector3 &to, const MT_Vector4 &color);
    MT_Vector3 m_from;
    MT_Vector3 m_to;
  };

  struct Circle : Shape {
    Circle(const MT_Vector3 &center,
           const MT_Vector3 &normal,
           float radius,
           int sector,
           const MT_Vector4 &color);
    MT_Vector3 m_center;
    MT_Vector3 m_normal;
    float m_radius;
    int m_sector;
  };

  struct Aabb : Shape {
    Aabb(const MT_Vector3 &pos,
         const MT_Matrix3x3 &rot,
         const MT_Vector3 &min,
         const MT_Vector3 &max,
         const MT_Vector4 &color);
    MT_Vector3 m_pos;
    MT_Matrix3x3 m_rot;
    MT_Vector3 m_min;
    MT_Vector3 m_max;
  };

  struct Box : Shape {
    Box(const std::array<MT_Vector3, 8> &vertices, const MT_Vector4 &color);
    std::array<MT_Vector3, 8> m_vertices;
  };

  struct SolidBox : Box {
    SolidBox(const MT_Vector4 &insideColor,
             const MT_Vector4 &outsideColor,
             const std::array<MT_Vector3, 8> &vertices,
             const MT_Vector4 &color);
    MT_Vector4 m_insideColor;
    MT_Vector4 m_outsideColor;
  };

  struct Text2D : Shape {
    Text2D(const std::string &text, const MT_Vector2 &pos, const MT_Vector4 &color);
    std::string m_text;
    MT_Vector2 m_pos;
  };

  struct Box2D : Shape {
    Box2D(const MT_Vector2 &pos, const MT_Vector2 &size, const MT_Vector4 &color);
    MT_Vector2 m_pos;
    MT_Vector2 m_size;
  };

  std::vector<Line> m_lines;
  std::vector<Circle> m_circles;
  std::vector<Aabb> m_aabbs;
  std::vector<Box> m_boxes;
  std::vector<SolidBox> m_solidBoxes;
  std::vector<Text2D> m_texts2D;
  std::vector<Box2D> m_boxes2D;

  RAS_OpenGLDebugDraw *m_impl;

 public:
  RAS_DebugDraw();
  ~RAS_DebugDraw();

  void DrawLine(const MT_Vector3 &from, const MT_Vector3 &to, const MT_Vector4 &color);
  void DrawCircle(const MT_Vector3 &center,
                  const MT_Scalar radius,
                  const MT_Vector4 &color,
                  const MT_Vector3 &normal,
                  int nsector);
  /** Draw a box depends on minimal and maximal corner.
   * \param pos The box's position.
   * \param rot The box's orientation.
   * \param min The box's minimal corner.
   * \param max The box's maximal corner.
   * \param color The box's color.
   */
  void DrawAabb(const MT_Vector3 &pos,
                const MT_Matrix3x3 &rot,
                const MT_Vector3 &min,
                const MT_Vector3 &max,
                const MT_Vector4 &color);
  void DrawBox(const std::array<MT_Vector3, 8> &vertices, const MT_Vector4 &color);
  void DrawSolidBox(const std::array<MT_Vector3, 8> &vertices,
                    const MT_Vector4 &insideColor,
                    const MT_Vector4 &outsideColor,
                    const MT_Vector4 &lineColor);
  /** Draw a box representing a camera frustum volume.
   * \param persmat The camera perspective matrix.
   */
  void DrawCameraFrustum(const MT_Matrix4x4 &persmat);

  void RenderBox2D(const MT_Vector2 &pos, const MT_Vector2 &size, const MT_Vector4 &color);

  void RenderText2D(const std::string &text, const MT_Vector2 &pos, const MT_Vector4 &color);

  void Flush(RAS_Rasterizer *rasty, RAS_ICanvas *canvas);
};
