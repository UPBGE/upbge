/*
 * 'Xor' together all inputs
 *
 *
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
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/GameLogic/SCA_XORController.cpp
 *  \ingroup gamelogic
 */

#include "SCA_XORController.h"

#include "SCA_ISensor.h"
#include "SCA_LogicManager.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

SCA_XORController::SCA_XORController(SCA_IObject *gameobj) : SCA_IController(gameobj)
{
}

SCA_XORController::~SCA_XORController()
{
}

void SCA_XORController::Trigger(SCA_LogicManager *logicmgr)
{

  bool sensorresult = false;

  for (SCA_ISensor *sensor : m_linkedsensors) {
    if (sensor->GetState()) {
      if (sensorresult == true) {
        sensorresult = false;
        break;
      }
      sensorresult = true;
    }
  }

  for (SCA_IActuator *actuator : m_linkedactuators) {
    logicmgr->AddActiveActuator(actuator, sensorresult);
  }
}

EXP_Value *SCA_XORController::GetReplica()
{
  EXP_Value *replica = new SCA_XORController(*this);
  // this will copy properties and so on...
  replica->ProcessReplica();

  return replica;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_XORController::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_XORController",
                                        sizeof(EXP_PyObjectPlus_Proxy),
                                        0,
                                        py_base_dealloc,
                                        0,
                                        0,
                                        0,
                                        0,
                                        py_base_repr,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        Methods,
                                        0,
                                        0,
                                        &SCA_IController::Type,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        py_base_new};

PyMethodDef SCA_XORController::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_XORController::Attributes[] = {
    EXP_PYATTRIBUTE_NULL  // Sentinel
};
#endif  // WITH_PYTHON

/* eof */
