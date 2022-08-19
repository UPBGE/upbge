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

/** \file RAS_OpenGLDebugDraw.h
 *  \ingroup bgerastogl
 */

#pragma once

#include "MT_Matrix4x4.h"

class RAS_Rasterizer;
class RAS_ICanvas;
class RAS_DebugDraw;

class RAS_OpenGLDebugDraw {

 private:
  MT_Matrix4x4 m_cameraMatrix;

 public:
  RAS_OpenGLDebugDraw();
  ~RAS_OpenGLDebugDraw();

  void BindVBO(float *mvp, float color[4], float *vertexes, unsigned int ibo);
  void UnbindVBO();

  void Flush(RAS_Rasterizer *rasty, RAS_ICanvas *canvas, RAS_DebugDraw *debugDraw);
};
