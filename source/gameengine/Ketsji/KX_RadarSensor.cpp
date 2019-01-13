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

/** \file gameengine/Ketsji/KX_RadarSensor.cpp
 *  \ingroup ketsji
 */


#include "KX_RadarSensor.h"
#include "KX_GameObject.h"
#include "KX_PyMath.h"
#include "PHY_IPhysicsController.h"
#include "PHY_IMotionState.h"
#include "DNA_sensor_types.h"

#include "BLI_math_rotation.h"

/**
 * RadarSensor constructor. Creates a near-sensor derived class, with a cone collision shape.
 */
KX_RadarSensor::KX_RadarSensor(SCA_EventManager *eventmgr,
                               KX_GameObject *gameobj,
                               PHY_IPhysicsController *physCtrl,
                               double coneradius,
                               double coneheight,
                               int axis,
                               double margin,
                               double resetmargin,
                               bool bFindMaterial,
                               const std::string& touchedpropname)

	:KX_NearSensor(
		eventmgr,
		gameobj,
		//DT_NewCone(coneradius,coneheight),
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
	//m_client_info->m_clientobject = gameobj;
	//m_client_info->m_auxilary_info = nullptr;
	//sumoObj->setClientObject(&m_client_info);
}

KX_RadarSensor::~KX_RadarSensor()
{

}

EXP_Value *KX_RadarSensor::GetReplica()
{
	KX_RadarSensor *replica = new KX_RadarSensor(*this);
	replica->ProcessReplica();
	return replica;
}

/**
 * Transforms the collision object. A cone is not correctly centered
 * for usage.  */
void KX_RadarSensor::SynchronizeTransform()
{
	KX_GameObject *obj = static_cast<KX_GameObject *>(GetParent());
	mt::mat3 rot = obj->NodeGetWorldOrientation();
	const mt::vec3& pos = obj->NodeGetWorldPosition();
	// What is the default orientation? pointing in the -y direction?
	// is the geometry correctly converted?

	// a collision cone is oriented
	// center the cone correctly
	// depends on the radar 'axis'
	switch (m_axis) {
		case SENS_RADAR_X_AXIS: // +X Axis
		{
			rot *= mt::mat3(0.0f, 0.0f, M_PI / 2.0f);
			break;
		};
		case SENS_RADAR_Y_AXIS: // +Y Axis
		{
			rot *= mt::mat3(-M_PI, 0.0f, 0.0f);
			break;
		};
		case SENS_RADAR_Z_AXIS: // +Z Axis
		{
			rot *= mt::mat3(-M_PI / 2.0f, 0.0f, 0.0f);
			break;
		};
		case SENS_RADAR_NEG_X_AXIS: // -X Axis
		{
			rot *= mt::mat3(0.0f, 0.0f, -M_PI / 2.0f);
			break;
		};
		case SENS_RADAR_NEG_Y_AXIS: // -Y Axis
		{
			break;
		};
		case SENS_RADAR_NEG_Z_AXIS: // -Z Axis
		{
			rot *= mt::mat3(M_PI / 2.0f, 0.0f, 0.0f);
			break;
		};
		default:
		{
		}
	}

	mt::mat3x4 trans(rot, pos + rot * mt::vec3(0, -m_coneheight / 2.0f, 0));

	m_cone_origin = trans.TranslationVector3D();
	m_cone_target = trans * mt::vec3(0, -m_coneheight/2.0f, 0);

	if (m_physCtrl) {
		PHY_IMotionState *motionState = m_physCtrl->GetMotionState();
		motionState->SetWorldPosition(trans.TranslationVector3D());
		motionState->SetWorldOrientation(trans.RotationMatrix());
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
PyTypeObject KX_RadarSensor::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_RadarSensor",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&KX_NearSensor::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_RadarSensor::Methods[] = {
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef KX_RadarSensor::Attributes[] = {
	EXP_PYATTRIBUTE_VECTOR_RO("coneOrigin", KX_RadarSensor, m_cone_origin, 3),
	EXP_PYATTRIBUTE_VECTOR_RO("coneTarget", KX_RadarSensor, m_cone_target, 3),
	EXP_PYATTRIBUTE_FLOAT_RO("distance", KX_RadarSensor, m_coneheight),
	EXP_PYATTRIBUTE_RO_FUNCTION("angle", KX_RadarSensor, pyattr_get_angle),
	EXP_PYATTRIBUTE_INT_RW("axis", 0, 5, true, KX_RadarSensor, m_axis),
	EXP_PYATTRIBUTE_NULL //Sentinel
};

PyObject *KX_RadarSensor::pyattr_get_angle(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_RadarSensor *self = static_cast<KX_RadarSensor *>(self_v);

	// The original angle from the gui was converted, so we recalculate the value here to maintain
	// consistency between Python and the gui
	return PyFloat_FromDouble(RAD2DEGF(atan(self->m_coneradius / self->m_coneheight)) * 2);

}

#endif // WITH_PYTHON
