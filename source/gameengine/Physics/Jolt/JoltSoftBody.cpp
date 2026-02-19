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

/** \file JoltSoftBody.cpp
 *  \ingroup physjolt
 */

#include "JoltSoftBody.h"

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>

#include "JoltMathUtils.h"
#include "JoltPhysicsController.h"
#include "JoltPhysicsEnvironment.h"

#include <cfloat>

/* -------------------------------------------------------------------- */
/** \name JoltSoftBody — Construction / Destruction
 * \{ */

JoltSoftBody::JoltSoftBody(JoltPhysicsEnvironment *env, JoltPhysicsController *ctrl)
    : m_env(env), m_ctrl(ctrl)
{
}

JoltSoftBody::~JoltSoftBody()
{
  if (!m_bodyID.IsInvalid() && m_env) {
    /* If physics is currently updating, defer body destruction. */
    if (m_env->IsPhysicsUpdating()) {
      JoltDeferredOp op;
      op.type = JoltDeferredOpType::DestroyBody;
      op.bodyID = m_bodyID;
      op.controller = nullptr;
      m_env->QueueDeferredOperation(op);
    }
    else {
      JPH::BodyInterface &bi = m_env->GetBodyInterface();
      bi.RemoveBody(m_bodyID);
      bi.DestroyBody(m_bodyID);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltSoftBody — Create
 * \{ */

bool JoltSoftBody::Create(const float *vertices,
                          int numVerts,
                          const int *triangles,
                          int numTriangles,
                          float mass,
                          const MT_Vector3 &position,
                          float margin,
                          float stiffness,
                          float friction,
                          float damping)
{
  if (!m_env || numVerts < 3 || numTriangles < 1) {
    return false;
  }

  /* Create shared settings for the soft body. */
  JPH::Ref<JPH::SoftBodySharedSettings> settings = new JPH::SoftBodySharedSettings();

  /* Add vertices (convert from Blender Z-up to Jolt Y-up). */
  float invMass = (mass > 0.0f) ? (1.0f / mass) * numVerts : 1.0f;
  settings->mVertices.resize(numVerts);
  m_originalPositions.resize(numVerts);

  for (int i = 0; i < numVerts; ++i) {
    float bx = vertices[i * 3 + 0];
    float by = vertices[i * 3 + 1];
    float bz = vertices[i * 3 + 2];

    /* Blender (X,Y,Z) → Jolt (X,Z,-Y) */
    settings->mVertices[i].mPosition = JPH::Float3(bx, bz, -by);
    settings->mVertices[i].mInvMass = invMass;

    m_originalPositions[i] = MT_Vector3(bx, by, bz);
  }

  /* Add faces. */
  settings->mFaces.resize(numTriangles);
  for (int i = 0; i < numTriangles; ++i) {
    settings->mFaces[i] = JPH::SoftBodySharedSettings::Face(
        (JPH::uint32)triangles[i * 3 + 0],
        (JPH::uint32)triangles[i * 3 + 1],
        (JPH::uint32)triangles[i * 3 + 2]);
  }

  /* Create edge, shear, and bend constraints automatically from faces.
   * Convert stiffness to compliance: compliance = 1/stiffness.
   * Stiffness of 0 means fully rigid (compliance = 0). */
  float compliance = (stiffness > 0.0f) ? (1.0f / stiffness) : 0.0f;
  JPH::SoftBodySharedSettings::VertexAttributes vertexAttribs(
      compliance,       /* edge compliance */
      compliance,       /* shear compliance */
      compliance * 10.0f /* bend compliance (softer bending) */
  );

  settings->CreateConstraints(&vertexAttribs, 1,
                               JPH::SoftBodySharedSettings::EBendType::Distance);

  /* Optimize constraints for parallel execution. */
  settings->Optimize();

  /* Create the soft body in the physics system. */
  JPH::RVec3 posJolt = JoltMath::ToJolt(position);

  JPH::SoftBodyCreationSettings creationSettings(
      settings.GetPtr(), posJolt, JPH::Quat::sIdentity(),
      JoltMakeObjectLayer(m_ctrl->GetCollisionGroup(), m_ctrl->GetCollisionMask(), JOLT_BP_DYNAMIC));
  creationSettings.mFriction = friction;
  creationSettings.mLinearDamping = damping;
  creationSettings.mRestitution = 0.0f;
  creationSettings.mPressure = 0.0f;
  creationSettings.mVertexRadius = margin;
  creationSettings.mGravityFactor = 1.0f;
  creationSettings.mNumIterations = 5;

  JPH::BodyInterface &bi = m_env->GetBodyInterface();
  JPH::Body *body = bi.CreateSoftBody(creationSettings);
  if (!body) {
    return false;
  }

  m_bodyID = body->GetID();
  bi.AddBody(m_bodyID, JPH::EActivation::Activate);

  /* Build 1:1 vertex map (soft body vertex index == mesh vertex index). */
  m_vertexMap.resize(numVerts);
  for (int i = 0; i < numVerts; ++i) {
    m_vertexMap[i] = i;
  }

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name JoltSoftBody — Sync Vertices
 * \{ */

void JoltSoftBody::SyncVertices()
{
  if (m_bodyID.IsInvalid() || !m_env) {
    return;
  }

  /* Lock the body to read soft body vertex positions. */
  const JPH::BodyLockInterface &lockInterface = m_env->GetPhysicsSystem()->GetBodyLockInterface();
  JPH::BodyLockRead lock(lockInterface, m_bodyID);
  if (!lock.Succeeded()) {
    return;
  }

  const JPH::Body &body = lock.GetBody();
  if (!body.IsSoftBody()) {
    return;
  }

  const JPH::SoftBodyMotionProperties *mp =
      static_cast<const JPH::SoftBodyMotionProperties *>(body.GetMotionProperties());

  const JPH::Array<JPH::SoftBodyVertex> &joltVerts = mp->GetVertices();

  /* Update original positions array with deformed positions (convert back to Blender Z-up). */
  int numVerts = (int)m_originalPositions.size();
  for (int i = 0; i < numVerts && i < (int)joltVerts.size(); ++i) {
    const JPH::Vec3 &jp = joltVerts[i].mPosition;
    /* Jolt (X,Y,Z) → Blender (X,-Z,Y) */
    m_originalPositions[i] = MT_Vector3(jp.GetX(), -jp.GetZ(), jp.GetY());
  }
}

MT_Vector3 JoltSoftBody::GetVertexPosition(int index) const
{
  if (index < 0 || index >= (int)m_originalPositions.size()) {
    return MT_Vector3(0, 0, 0);
  }
  return m_originalPositions[index];
}

/** \} */
