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

/** \file JoltShapeBuilder.cpp
 *  \ingroup physjolt
 */

#include "JoltShapeBuilder.h"

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/EmptyShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

#include "JoltMathUtils.h"

#include "BKE_context.hh"
#include "BKE_object.hh"
#include "DEG_depsgraph_query.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_mesh.hh"

#include "KX_Globals.h"
#include "KX_KetsjiEngine.h"
#include "KX_Scene.h"
#include "RAS_MeshObject.h"

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

#include <algorithm>
#include <cmath>
#include <map>

JoltShapeBuilder::JoltShapeBuilder()
{
}

JoltShapeBuilder::~JoltShapeBuilder()
{
}

void JoltShapeBuilder::SetShapeType(PHY_ShapeType type)
{
  m_shapeType = type;
}

void JoltShapeBuilder::SetRadius(float radius)
{
  m_radius = radius;
}

void JoltShapeBuilder::SetHeight(float height)
{
  m_height = height;
}

void JoltShapeBuilder::SetHalfExtents(float hx, float hy, float hz)
{
  m_halfExtents[0] = hx;
  m_halfExtents[1] = hy;
  m_halfExtents[2] = hz;
}

void JoltShapeBuilder::SetMargin(float margin)
{
  m_margin = margin;
}

bool JoltShapeBuilder::SetMesh(KX_Scene *kxscene, RAS_MeshObject *meshobj, bool polytope)
{
  m_vertexArray.clear();
  m_triFaceArray.clear();
  m_vertRemap.clear();

  if (!meshobj || !meshobj->HasColliderPolygon()) {
    return false;
  }

  blender::bContext *C = KX_GetActiveEngine()->GetContext();
  blender::Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);

  blender::Object *ob_eval = DEG_get_evaluated(depsgraph, meshobj->GetOriginalObject());
  blender::Mesh *me = (blender::Mesh *)ob_eval->data;

  const blender::Span<blender::float3> positions = me->vert_positions();

  if (polytope) {
    m_shapeType = PHY_SHAPE_POLYTOPE;

    std::map<int, int> vert_remap;
    int next_vert = 0;

    for (int vert_idx = 0; vert_idx < positions.size(); ++vert_idx) {
      vert_remap[vert_idx] = next_vert++;
    }

    if (next_vert == 0) {
      return false;
    }

    m_vertexArray.resize(next_vert * 3);
    for (const auto &pair : vert_remap) {
      const float *vtx = &positions[pair.first][0];
      int idx = pair.second;
      m_vertexArray[idx * 3 + 0] = vtx[0];
      m_vertexArray[idx * 3 + 1] = vtx[1];
      m_vertexArray[idx * 3 + 2] = vtx[2];
      m_vertRemap[pair.first] = pair.second;
    }
  }
  else {
    m_shapeType = PHY_SHAPE_MESH;

    const blender::Span<blender::int3> tris = me->corner_tris();
    const blender::Span<int> corner_verts = me->corner_verts();

    std::map<int, int> vert_remap;
    int next_vert = 0;

    for (int t = 0; t < tris.size(); ++t) {
      const blender::int3 &tri = tris[t];
      int tri_indices[3];

      for (int j = 0; j < 3; ++j) {
        int loop_idx = tri[j];
        int vert_idx = corner_verts[loop_idx];

        auto it = vert_remap.find(vert_idx);
        if (it == vert_remap.end()) {
          m_vertexArray.push_back(positions[vert_idx][0]);
          m_vertexArray.push_back(positions[vert_idx][1]);
          m_vertexArray.push_back(positions[vert_idx][2]);
          tri_indices[j] = next_vert;
          vert_remap[vert_idx] = next_vert;
          m_vertRemap[vert_idx] = next_vert;
          next_vert++;
        }
        else {
          tri_indices[j] = it->second;
        }
      }

      m_triFaceArray.push_back(tri_indices[0]);
      m_triFaceArray.push_back(tri_indices[1]);
      m_triFaceArray.push_back(tri_indices[2]);
    }

    if (m_vertexArray.empty() || m_triFaceArray.empty()) {
      return false;
    }
  }

  return true;
}

JPH::RefConst<JPH::Shape> JoltShapeBuilder::Build(bool useGimpact,
                                                    const MT_Vector3 &scale) const
{
  JPH::RefConst<JPH::Shape> shape;

  switch (m_shapeType) {
    case PHY_SHAPE_BOX:
      shape = BuildBox();
      break;
    case PHY_SHAPE_SPHERE:
      shape = BuildSphere();
      break;
    case PHY_SHAPE_CAPSULE:
      shape = BuildCapsule();
      break;
    case PHY_SHAPE_CYLINDER:
      shape = BuildCylinder();
      break;
    case PHY_SHAPE_CONE:
      shape = BuildCone();
      break;
    case PHY_SHAPE_POLYTOPE:
      shape = BuildConvexHull();
      break;
    case PHY_SHAPE_MESH:
      if (useGimpact) {
        /* Dynamic bodies with mesh bounds use convex hull instead (matching Bullet GImpact). */
        shape = BuildConvexHull();
      }
      else {
        shape = BuildMesh();
      }
      break;
    case PHY_SHAPE_EMPTY:
      shape = BuildEmpty();
      break;
    default:
      return nullptr;
  }

  if (!shape) {
    return nullptr;
  }

  return MaybeScale(shape, scale);
}

JPH::RefConst<JPH::Shape> JoltShapeBuilder::BuildCompoundChild(
    const MT_Vector3 &localPos,
    const MT_Matrix3x3 &localRot,
    const MT_Vector3 &localScale) const
{
  JPH::RefConst<JPH::Shape> baseShape = Build(false, localScale);
  if (!baseShape) {
    return nullptr;
  }

  /* Wrap in RotatedTranslatedShape if the child has a non-identity local transform. */
  JPH::Vec3 joltPos = JoltMath::ToJolt(localPos);
  MT_Quaternion quat = localRot.getRotation();
  JPH::Quat joltRot = JoltMath::ToJolt(quat);

  if (joltPos.IsNearZero() && joltRot.IsClose(JPH::Quat::sIdentity())) {
    return baseShape;
  }

  return new JPH::RotatedTranslatedShape(joltPos, joltRot, baseShape.GetPtr());
}

/* -------------------------------------------------------------------- */
/** \name Shape Builders
 * \{ */

JPH::RefConst<JPH::Shape> JoltShapeBuilder::BuildBox() const
{
  /* Blender half-extents are in Z-up. Convert to Jolt Y-up: (hx, hz, hy). */
  float hx = std::max(m_halfExtents[0], 0.001f);
  float hy = std::max(m_halfExtents[1], 0.001f);
  float hz = std::max(m_halfExtents[2], 0.001f);

  /* Jolt BoxShape takes half-extents. Convex radius is subtracted from shape. */
  float convexRadius = std::min(m_margin, std::min({hx, hy, hz}));

  JPH::BoxShapeSettings settings(JPH::Vec3(hx, hz, hy), convexRadius);
  JPH::Shape::ShapeResult result = settings.Create();
  if (result.HasError()) {
    return nullptr;
  }
  return result.Get();
}

JPH::RefConst<JPH::Shape> JoltShapeBuilder::BuildSphere() const
{
  float r = std::max(m_radius, 0.001f);
  JPH::SphereShapeSettings settings(r);
  JPH::Shape::ShapeResult result = settings.Create();
  if (result.HasError()) {
    return nullptr;
  }
  return result.Get();
}

JPH::RefConst<JPH::Shape> JoltShapeBuilder::BuildCapsule() const
{
  /* Blender capsule: radius and total cylinder height (not including caps).
   * Jolt CapsuleShape: half height of cylinder + radius for caps.
   * Blender capsule is along Z axis. Jolt capsule is along Y axis.
   * We use a RotatedTranslatedShape to rotate 90 degrees. */
  float r = std::max(m_radius, 0.001f);
  float halfHeight = std::max(m_height * 0.5f, 0.001f);

  JPH::CapsuleShapeSettings settings(halfHeight, r);
  JPH::Shape::ShapeResult result = settings.Create();
  if (result.HasError()) {
    return nullptr;
  }

  /* Rotate capsule from Jolt Y-axis to Blender Z-axis (in Jolt coords this means no rotation
   * needed since our coordinate conversion already handles Y-up to Z-up). */
  return result.Get();
}

JPH::RefConst<JPH::Shape> JoltShapeBuilder::BuildCylinder() const
{
  /* Blender cylinder: radius and half-height along Z.
   * Jolt cylinder: half-height along Y. Coordinate swap handles this. */
  float r = std::max(m_halfExtents[0], 0.001f);
  float halfHeight = std::max(m_halfExtents[2], 0.001f);
  float convexRadius = std::min(m_margin, std::min(r, halfHeight));

  JPH::CylinderShapeSettings settings(halfHeight, r, convexRadius);
  JPH::Shape::ShapeResult result = settings.Create();
  if (result.HasError()) {
    return nullptr;
  }
  return result.Get();
}

JPH::RefConst<JPH::Shape> JoltShapeBuilder::BuildCone() const
{
  /* Jolt has no ConeShape. Approximate with a ConvexHull.
   * Generate vertices for a cone along the Y axis (Jolt's up). */
  const int numSegments = 16;
  JPH::Array<JPH::Vec3> points;
  points.reserve(numSegments + 1);

  float r = std::max(m_radius, 0.001f);
  float h = std::max(m_height, 0.001f);

  /* Tip of the cone at +Y. */
  points.push_back(JPH::Vec3(0.0f, h * 0.5f, 0.0f));

  /* Base circle at -Y. */
  for (int i = 0; i < numSegments; ++i) {
    float angle = (2.0f * JPH::JPH_PI * i) / numSegments;
    float x = r * std::cos(angle);
    float z = r * std::sin(angle);
    points.push_back(JPH::Vec3(x, -h * 0.5f, z));
  }

  float convexRadius = std::min(m_margin, std::min(r, h) * 0.5f);
  JPH::ConvexHullShapeSettings settings(points, convexRadius);
  JPH::Shape::ShapeResult result = settings.Create();
  if (result.HasError()) {
    return nullptr;
  }
  return result.Get();
}

JPH::RefConst<JPH::Shape> JoltShapeBuilder::BuildConvexHull() const
{
  if (m_vertexArray.empty()) {
    return nullptr;
  }

  int numVerts = (int)m_vertexArray.size() / 3;
  JPH::Array<JPH::Vec3> points;
  points.reserve(numVerts);

  for (int i = 0; i < numVerts; ++i) {
    float bx = m_vertexArray[i * 3 + 0];
    float by = m_vertexArray[i * 3 + 1];
    float bz = m_vertexArray[i * 3 + 2];
    /* Convert Blender local space (Z-up) to Jolt (Y-up). */
    points.push_back(JPH::Vec3(bx, bz, -by));
  }

  float convexRadius = std::max(m_margin, 0.001f);
  JPH::ConvexHullShapeSettings settings(points, convexRadius);
  JPH::Shape::ShapeResult result = settings.Create();
  if (result.HasError()) {
    return nullptr;
  }
  return result.Get();
}

JPH::RefConst<JPH::Shape> JoltShapeBuilder::BuildMesh() const
{
  if (m_vertexArray.empty() || m_triFaceArray.empty()) {
    return nullptr;
  }

  int numVerts = (int)m_vertexArray.size() / 3;
  int numTris = (int)m_triFaceArray.size() / 3;

  JPH::VertexList vertices;
  vertices.reserve(numVerts);
  for (int i = 0; i < numVerts; ++i) {
    float bx = m_vertexArray[i * 3 + 0];
    float by = m_vertexArray[i * 3 + 1];
    float bz = m_vertexArray[i * 3 + 2];
    /* Convert Blender local space (Z-up) to Jolt (Y-up). */
    vertices.push_back(JPH::Float3(bx, bz, -by));
  }

  JPH::IndexedTriangleList triangles;
  triangles.reserve(numTris);
  for (int i = 0; i < numTris; ++i) {
    int i0 = m_triFaceArray[i * 3 + 0];
    int i1 = m_triFaceArray[i * 3 + 1];
    int i2 = m_triFaceArray[i * 3 + 2];
    triangles.push_back(JPH::IndexedTriangle(i0, i1, i2));
  }

  JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
  JPH::Shape::ShapeResult result = settings.Create();
  if (result.HasError()) {
    return nullptr;
  }
  return result.Get();
}

JPH::RefConst<JPH::Shape> JoltShapeBuilder::BuildEmpty() const
{
  JPH::EmptyShapeSettings settings;
  JPH::Shape::ShapeResult result = settings.Create();
  if (result.HasError()) {
    return nullptr;
  }
  return result.Get();
}

/** \} */

JPH::RefConst<JPH::Shape> JoltShapeBuilder::MaybeScale(JPH::RefConst<JPH::Shape> shape,
                                                         const MT_Vector3 &scale) const
{
  bool needsScale = (std::abs(scale[0] - 1.0f) > 0.0001f ||
                     std::abs(scale[1] - 1.0f) > 0.0001f ||
                     std::abs(scale[2] - 1.0f) > 0.0001f);

  if (!needsScale) {
    return shape;
  }

  /* Convert Blender scale (X,Y,Z) to Jolt scale (X,Z,Y) — same coord swap. */
  JPH::Vec3 joltScale(scale[0], scale[2], scale[1]);

  /* Use Shape::ScaleShape for safe scaling that handles rotated compound sub-shapes. */
  JPH::Shape::ShapeResult result = shape->ScaleShape(joltScale);
  if (result.HasError()) {
    /* Fallback: try ScaledShape directly. */
    return new JPH::ScaledShape(shape.GetPtr(), joltScale);
  }
  return result.Get();
}
