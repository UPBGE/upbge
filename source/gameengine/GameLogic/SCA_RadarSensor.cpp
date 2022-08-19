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
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/SCA_RadarSensor.cpp
 *  \ingroup ketsji
 */

#include "SCA_RadarSensor.h"

#include "DNA_sensor_types.h"

#include "KX_GameObject.h"
#include "PHY_IMotionState.h"
#include "PHY_IPhysicsController.h"

/**
 * 	RadarSensor constructor. Creates a near-sensor derived class, with a cone collision shape.
 */
SCA_RadarSensor::SCA_RadarSensor(SCA_EventManager *eventmgr,
                                 KX_GameObject *gameobj,
                                 PHY_IPhysicsController *physCtrl,
                                 double coneradius,
                                 double coneheight,
                                 int axis,
                                 double margin,
                                 double resetmargin,
                                 bool bFindMaterial,
                                 const std::string &touchedpropname)

    : SCA_NearSensor(eventmgr,
                     gameobj,
                     // DT_NewCone(coneradius,coneheight),
                     margin,
                     resetmargin,
                     bFindMaterial,
                     touchedpropname,
                     physCtrl),

      m_coneradius(coneradius),
      m_coneheight(coneheight),
      m_axis(axis)
{
  m_client_info->m_type = KX_ClientObjectInfo::SENSOR;
  // m_client_info->m_clientobject = gameobj;
  // m_client_info->m_auxilary_info = nullptr;
  // sumoObj->setClientObject(&m_client_info);
}

SCA_RadarSensor::~SCA_RadarSensor()
{
}

EXP_Value *SCA_RadarSensor::GetReplica()
{
  SCA_RadarSensor *replica = new SCA_RadarSensor(*this);
  replica->ProcessReplica();
  return replica;
}

/**
 *	Transforms the collision object. A cone is not correctly centered
 *	for usage.  */
void SCA_RadarSensor::SynchronizeTransform()
{
  // Getting the parent location was commented out. Why?
  MT_Transform trans;
  trans.setOrigin(((KX_GameObject *)GetParent())->NodeGetWorldPosition());
  trans.setBasis(((KX_GameObject *)GetParent())->NodeGetWorldOrientation());
  // What is the default orientation? pointing in the -y direction?
  // is the geometry correctly converted?

  // a collision cone is oriented
  // center the cone correctly
  // depends on the radar 'axis'
  switch (m_axis) {
    case SENS_RADAR_X_AXIS:  // +X Axis
    {
      MT_Quaternion rotquatje(MT_Vector3(0, 0, 1), MT_radians(90));
      trans.rotate(rotquatje);
      trans.translate(MT_Vector3(0, -m_coneheight / 2.0f, 0));
      break;
    };
    case SENS_RADAR_Y_AXIS:  // +Y Axis
    {
      MT_Quaternion rotquatje(MT_Vector3(1, 0, 0), MT_radians(-180));
      trans.rotate(rotquatje);
      trans.translate(MT_Vector3(0, -m_coneheight / 2.0f, 0));
      break;
    };
    case SENS_RADAR_Z_AXIS:  // +Z Axis
    {
      MT_Quaternion rotquatje(MT_Vector3(1, 0, 0), MT_radians(-90));
      trans.rotate(rotquatje);
      trans.translate(MT_Vector3(0, -m_coneheight / 2.0f, 0));
      break;
    };
    case SENS_RADAR_NEG_X_AXIS:  // -X Axis
    {
      MT_Quaternion rotquatje(MT_Vector3(0, 0, 1), MT_radians(-90));
      trans.rotate(rotquatje);
      trans.translate(MT_Vector3(0, -m_coneheight / 2.0f, 0));
      break;
    };
    case SENS_RADAR_NEG_Y_AXIS:  // -Y Axis
    {
      // MT_Quaternion rotquatje(MT_Vector3(1,0,0),MT_radians(-180));
      // trans.rotate(rotquatje);
      trans.translate(MT_Vector3(0, -m_coneheight / 2.0f, 0));
      break;
    };
    case SENS_RADAR_NEG_Z_AXIS:  // -Z Axis
    {
      MT_Quaternion rotquatje(MT_Vector3(1, 0, 0), MT_radians(90));
      trans.rotate(rotquatje);
      trans.translate(MT_Vector3(0, -m_coneheight / 2.0f, 0));
      break;
    };
    default: {
    }
  }

  // Using a temp variable to translate MT_Vector3 to float[3].
  // float[3] works better for the Python interface.
  MT_Vector3 temp = trans.getOrigin();
  m_cone_origin[0] = temp[0];
  m_cone_origin[1] = temp[1];
  m_cone_origin[2] = temp[2];

  temp = trans(MT_Vector3(0, -m_coneheight / 2.0f, 0));
  m_cone_target[0] = temp[0];
  m_cone_target[1] = temp[1];
  m_cone_target[2] = temp[2];

  if (m_physCtrl) {
    PHY_IMotionState *motionState = m_physCtrl->GetMotionState();
    motionState->SetWorldPosition(trans.getOrigin());
    motionState->SetWorldOrientation(trans.getBasis());
    m_physCtrl->WriteMotionStateToDynamics(true);
  }
}

/* ------------------------------------------------------------------------- */
/* Python Functions															 */
/* ------------------------------------------------------------------------- */

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python Integration Hooks                                                  */
/* ------------------------------------------------------------------------- */
PyTypeObject SCA_RadarSensor::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_RadarSensor",
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
                                      &SCA_NearSensor::Type,
                                      0,
                                      0,
                                      0,
                                      0,
                                      0,
                                      0,
                                      py_base_new};

PyMethodDef SCA_RadarSensor::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_RadarSensor::Attributes[] = {
    EXP_PYATTRIBUTE_FLOAT_ARRAY_RO("coneOrigin", SCA_RadarSensor, m_cone_origin, 3),
    EXP_PYATTRIBUTE_FLOAT_ARRAY_RO("coneTarget", SCA_RadarSensor, m_cone_target, 3),
    EXP_PYATTRIBUTE_FLOAT_RO("distance", SCA_RadarSensor, m_coneheight),
    EXP_PYATTRIBUTE_RO_FUNCTION("angle", SCA_RadarSensor, pyattr_get_angle),
    EXP_PYATTRIBUTE_INT_RW("axis", 0, 5, true, SCA_RadarSensor, m_axis),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

PyObject *SCA_RadarSensor::pyattr_get_angle(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_RadarSensor *self = static_cast<SCA_RadarSensor *>(self_v);

  // The original angle from the gui was converted, so we recalculate the value here to maintain
  // consistency between Python and the gui
  return PyFloat_FromDouble(MT_degrees(atan(self->m_coneradius / self->m_coneheight)) * 2);
}

#endif  // WITH_PYTHON
