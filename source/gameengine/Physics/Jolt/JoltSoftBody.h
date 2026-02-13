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

/** \file JoltSoftBody.h
 *  \ingroup physjolt
 *  \brief Jolt Physics soft body wrapper for UPBGE.
 */

#pragma once

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>

#include "MT_Vector3.h"

#include <vector>

class JoltPhysicsController;
class JoltPhysicsEnvironment;
class RAS_MeshObject;

/**
 * JoltSoftBody manages a Jolt soft body and maps its vertices back
 * to the UPBGE graphics mesh for rendering deformation.
 *
 * Jolt soft bodies use particle-based simulation with edge, bend,
 * and volume constraints defined via SoftBodySharedSettings.
 */
class JoltSoftBody {
 public:
  JoltSoftBody(JoltPhysicsEnvironment *env,
               JoltPhysicsController *ctrl);
  ~JoltSoftBody();

  /** Build soft body from mesh data and add to physics system.
   *  @param vertices Array of vertex positions (Blender Z-up space).
   *  @param numVerts Number of vertices.
   *  @param triangles Array of triangle indices (3 per face).
   *  @param numTriangles Number of triangles.
   *  @param mass Total mass of the soft body.
   *  @param position World position offset.
   *  @param margin Collision margin.
   *  @param stiffness Edge stiffness (0 = rigid, higher = more compliant).
   *  @param friction Friction coefficient.
   *  @param damping Linear damping.
   *  @return true if successfully created. */
  bool Create(const float *vertices,
              int numVerts,
              const int *triangles,
              int numTriangles,
              float mass,
              const MT_Vector3 &position,
              float margin,
              float stiffness,
              float friction,
              float damping);

  /** Sync deformed vertex positions back to the game engine.
   *  Called each frame from UpdateSoftBodies(). */
  void SyncVertices();

  /** Get the Jolt BodyID for this soft body. */
  JPH::BodyID GetBodyID() const { return m_bodyID; }

  /** Get number of vertices. */
  int GetNumVertices() const { return (int)m_originalPositions.size(); }

  /** Get deformed vertex position (in Blender Z-up space). */
  MT_Vector3 GetVertexPosition(int index) const;

  /** Get the associated controller. */
  JoltPhysicsController *GetController() { return m_ctrl; }

 private:
  JoltPhysicsEnvironment *m_env;
  JoltPhysicsController *m_ctrl;
  JPH::BodyID m_bodyID;

  /** Original vertex positions (Blender Z-up) for reference. */
  std::vector<MT_Vector3> m_originalPositions;

  /** Mapping from soft body vertex index to mesh vertex index.
   *  Used to map deformed positions back to the rendering mesh. */
  std::vector<int> m_vertexMap;
};
