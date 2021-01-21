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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Campbell Barton
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file SCA_DynamicActuator.h
 *  \ingroup ketsji
 *  \brief Add object to the game world on action of this actuator
 */

#pragma once

#include "KX_GameObject.h"
#include "SCA_IActuator.h"
#include "SCA_LogicManager.h"
#include "SCA_PropertyActuator.h"

class SCA_DynamicActuator : public SCA_IActuator {
  Py_Header

      // dynamics operation to apply to the game object
      short m_dyn_operation;
  float m_setmass;

  bool m_suspend_children_phys;
  bool m_restore_children_phys;
  bool m_suspend_constraints;

 public:
  SCA_DynamicActuator(SCA_IObject *gameobj,
                      short dyn_operation,
                      float setmass,
                      bool suspend_children_phys,
                      bool restore_children_phys,
                      bool suspend_constraints);

  ~SCA_DynamicActuator();

  EXP_Value *GetReplica();

  virtual bool Update();

  // Python Interface
  enum DynamicOperation {
    KX_DYN_RESTORE_DYNAMICS = 0,
    KX_DYN_DISABLE_DYNAMICS,
    KX_DYN_ENABLE_RIGID_BODY,
    KX_DYN_DISABLE_RIGID_BODY,
    KX_DYN_SET_MASS,
    KX_DYN_RESTORE_PHYSICS,
    KX_DYN_DISABLE_PHYSICS
  };
};
