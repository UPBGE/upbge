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

/** \file JoltGraphicController.h
 *  \ingroup physjolt
 *  \brief Jolt Physics backend implementing PHY_IGraphicController for broadphase culling.
 */

#pragma once

#include "PHY_IGraphicController.h"

class JoltPhysicsEnvironment;

/**
 * JoltGraphicController manages the broadphase AABB for view frustum culling.
 * Implements PHY_IGraphicController interface.
 */
class JoltGraphicController : public PHY_IGraphicController {
 public:
  JoltGraphicController(JoltPhysicsEnvironment *env);
  virtual ~JoltGraphicController();

  /* ---- PHY_IController interface ---- */
  virtual void *GetNewClientInfo() override;
  virtual void SetNewClientInfo(void *clientinfo) override;
  virtual void SetPhysicsEnvironment(class PHY_IPhysicsEnvironment *env) override;

  /* ---- PHY_IGraphicController interface ---- */
  virtual bool SetGraphicTransform() override;
  virtual void Activate(bool active) override;
  virtual void SetLocalAabb(const MT_Vector3 &aabbMin, const MT_Vector3 &aabbMax) override;

  virtual PHY_IGraphicController *GetReplica(class PHY_IMotionState *motionstate) override;

 private:
  JoltPhysicsEnvironment *m_physicsEnv;
  void *m_newClientInfo = nullptr;
  MT_Vector3 m_localAabbMin;
  MT_Vector3 m_localAabbMax;
  bool m_active = false;
};
