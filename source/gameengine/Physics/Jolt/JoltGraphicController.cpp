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

/** \file JoltGraphicController.cpp
 *  \ingroup physjolt
 */

#include "JoltGraphicController.h"

#include "JoltPhysicsEnvironment.h"
#include "PHY_IMotionState.h"
#include "PHY_IPhysicsEnvironment.h"

JoltGraphicController::JoltGraphicController(JoltPhysicsEnvironment *env)
    : m_physicsEnv(env),
      m_localAabbMin(0.0f, 0.0f, 0.0f),
      m_localAabbMax(0.0f, 0.0f, 0.0f)
{
}

JoltGraphicController::~JoltGraphicController()
{
  if (m_physicsEnv && m_active) {
    m_physicsEnv->RemoveGraphicController(this);
  }
}

bool JoltGraphicController::SetGraphicTransform()
{
  /* The graphic controller's AABB is managed by the environment's
   * m_graphicControllers set. The CullingTest iterates this set
   * and calls back for each active controller. The transform is
   * implicitly updated because the client info (KX_GameObject)
   * always has the current world transform. */
  return true;
}

void JoltGraphicController::Activate(bool active)
{
  if (m_active == active) {
    return;
  }
  m_active = active;
  if (m_physicsEnv) {
    if (active) {
      m_physicsEnv->AddGraphicController(this);
    }
    else {
      m_physicsEnv->RemoveGraphicController(this);
    }
  }
}

void JoltGraphicController::SetLocalAabb(const MT_Vector3 &aabbMin, const MT_Vector3 &aabbMax)
{
  m_localAabbMin = aabbMin;
  m_localAabbMax = aabbMax;
}

PHY_IGraphicController *JoltGraphicController::GetReplica(PHY_IMotionState *motionstate)
{
  JoltGraphicController *replica = new JoltGraphicController(*this);
  replica->m_active = false;
  return replica;
}

void *JoltGraphicController::GetNewClientInfo()
{
  return m_newClientInfo;
}

void JoltGraphicController::SetNewClientInfo(void *clientinfo)
{
  m_newClientInfo = clientinfo;
}

void JoltGraphicController::SetPhysicsEnvironment(PHY_IPhysicsEnvironment *env)
{
  m_physicsEnv = static_cast<JoltPhysicsEnvironment *>(env);
}
