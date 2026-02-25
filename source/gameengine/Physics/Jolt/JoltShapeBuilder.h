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
 * Contributor(s): UPBGE Contributors
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file JoltShapeBuilder.h
 *  \ingroup physjolt
 *  \brief Utility to build Jolt collision shapes from Blender mesh/bounds data.
 */

#pragma once

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Collision/Shape/Shape.h>

#include "MT_Matrix3x3.h"
#include "MT_Vector3.h"
#include "PHY_DynamicTypes.h"

#include <unordered_map>
#include <vector>

class RAS_MeshObject;
class KX_Scene;

/**
 * JoltShapeBuilder creates Jolt collision shapes from Blender object data.
 *
 * Usage:
 *   JoltShapeBuilder builder;
 *   builder.SetBoundsType(OB_BOUND_BOX);
 *   builder.SetHalfExtents(halfX, halfY, halfZ);
 *   builder.SetMargin(margin);
 *   JPH::RefConst<JPH::Shape> shape = builder.Build();
 */
class JoltShapeBuilder {
 public:
  JoltShapeBuilder();
  ~JoltShapeBuilder();

  /* ---- Configuration ---- */

  void SetShapeType(PHY_ShapeType type);
  void SetRadius(float radius);
  void SetHeight(float height);
  void SetHalfExtents(float hx, float hy, float hz);
  void SetMargin(float margin);

  /** Load vertex/triangle data from a Blender mesh.
   *  \param polytope If true, build convex hull from vertices; otherwise triangle mesh. */
  bool SetMesh(KX_Scene *kxscene, RAS_MeshObject *meshobj, bool polytope);

  /** Access raw vertex array (3 floats per vertex, Blender local space). */
  const std::vector<float> &GetVertexArray() const { return m_vertexArray; }
  /** Access raw triangle index array (3 indices per triangle). */
  const std::vector<int> &GetTriangleArray() const { return m_triFaceArray; }
  /** Access vertex remap: Blender original vert index → soft body particle index. */
  const std::unordered_map<int, int> &GetVertexRemap() const { return m_vertRemap; }

  /** Build the Jolt shape from the configured parameters.
   *  Returns nullptr if the configuration is invalid.
   *  \param useGimpact If true and shape is mesh, use convex hull for dynamic bodies.
   *  \param scale World scaling to apply via ScaledShape. */
  JPH::RefConst<JPH::Shape> Build(bool useGimpact = false,
                                   const MT_Vector3 &scale = MT_Vector3(1.0f, 1.0f, 1.0f)) const;

  /** Build a shape for a compound child, including local transform.
   *  Returns nullptr if the shape cannot be created. */
  JPH::RefConst<JPH::Shape> BuildCompoundChild(const MT_Vector3 &localPos,
                                                 const MT_Matrix3x3 &localRot,
                                                 const MT_Vector3 &localScale) const;

 private:
  JPH::RefConst<JPH::Shape> BuildBox() const;
  JPH::RefConst<JPH::Shape> BuildSphere() const;
  JPH::RefConst<JPH::Shape> BuildCapsule() const;
  JPH::RefConst<JPH::Shape> BuildCylinder() const;
  JPH::RefConst<JPH::Shape> BuildCone() const;
  JPH::RefConst<JPH::Shape> BuildConvexHull() const;
  JPH::RefConst<JPH::Shape> BuildMesh() const;
  JPH::RefConst<JPH::Shape> BuildEmpty() const;

  /** Wrap a shape in ScaledShape if scale is non-uniform or != 1. */
  JPH::RefConst<JPH::Shape> MaybeScale(JPH::RefConst<JPH::Shape> shape,
                                         const MT_Vector3 &scale) const;

  PHY_ShapeType m_shapeType = PHY_SHAPE_NONE;
  float m_radius = 1.0f;
  float m_height = 1.0f;
  float m_halfExtents[3] = {0.5f, 0.5f, 0.5f};
  float m_margin = 0.04f;

  /** Vertex data (3 floats per vertex, in Blender local space). */
  std::vector<float> m_vertexArray;
  /** Triangle index data (3 indices per triangle). */
  std::vector<int> m_triFaceArray;
  /** Maps Blender original vertex index to Jolt soft body particle index. */
  std::unordered_map<int, int> m_vertRemap;
};
