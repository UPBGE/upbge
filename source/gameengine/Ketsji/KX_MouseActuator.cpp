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
 * Contributor(s): Geoffrey Gollmer, Jorge Bernal
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#include "KX_MouseActuator.h"
#include "KX_KetsjiEngine.h"
#include "KX_PyMath.h"
#include "SCA_MouseManager.h"
#include "SCA_IInputDevice.h"
#include "RAS_ICanvas.h"
#include "KX_GameObject.h"
#include "KX_PyMath.h"
#include "BLI_utildefines.h"
#include "limits.h"

#include "BLI_math.h"

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

KX_MouseActuator::KX_MouseActuator(SCA_IObject* gameobj, KX_KetsjiEngine* ketsjiEngine, SCA_MouseManager* eventmgr,
		int acttype, bool visible, const bool use_axis[2], const mt::vec2& threshold, const bool reset[2],
		const int object_axis[2], const bool local[2], const mt::vec2& sensitivity, const mt::vec2 limit[2])
	:SCA_IActuator(gameobj, KX_ACT_MOUSE),
	m_ketsji(ketsjiEngine),
	m_eventmgr(eventmgr),
	m_type(acttype),
	m_visible(visible),
	m_threshold(threshold),
	m_sensitivity(sensitivity),
	m_initialSkipping(true),
	m_oldPosition(mt::zero2),
	m_angle(mt::zero2)
{
	for (unsigned short i = 0; i < 2; ++i) {
		m_use_axis[i] = use_axis[i];
		m_reset[i] = reset[i];
		m_object_axis[i] = object_axis[i];
		m_local[i] = local[i];
		m_limit[i] = limit[i];
	}

	m_canvas = m_ketsji->GetCanvas();
}

KX_MouseActuator::~KX_MouseActuator()
{
}

bool KX_MouseActuator::Update()
{
	bool bNegativeEvent = IsNegativeEvent();
	RemoveAllEvents();

	if (bNegativeEvent) {
		// Reset initial skipping check on negative events.
		m_initialSkipping = true;
		return false;
	}

	KX_GameObject *parent = static_cast<KX_GameObject *>(GetParent());

	m_mouse = ((SCA_MouseManager *)m_eventmgr)->GetInputDevice();

	switch (m_type) {
		case KX_ACT_MOUSE_VISIBILITY:
		{
			if (m_visible) {
				if (m_canvas) {
					m_canvas->SetMouseState(RAS_ICanvas::MOUSE_NORMAL);
				}
			}
			else {
				if (m_canvas) {
					m_canvas->SetMouseState(RAS_ICanvas::MOUSE_INVISIBLE);
				}
			}
			break;
		}
		case KX_ACT_MOUSE_LOOK:
		{
			if (m_mouse) {

				const mt::vec2 position = GetMousePosition();
				mt::vec2 movement = position;
				mt::vec3 rotation;
				mt::vec2 setposition = mt::zero2;
				
				mt::vec2 center(0.5f, 0.5f);

				//preventing undesired drifting when resolution is odd
				if ((m_canvas->GetWidth() % 2) == 0) {
					center.x = float(m_canvas->GetMaxX() / 2) / m_canvas->GetMaxX();
				}
				if ((m_canvas->GetHeight() % 2) == 0) {
					center.y = float(m_canvas->GetMaxY() / 2) / m_canvas->GetMaxY();
				}

				//preventing initial skipping.
				if (m_initialSkipping) {
					for (unsigned short i = 0; i < 2; ++i) {
						if (m_reset[i]) {
							m_oldPosition[i] = center[i];
						}
						else {
							m_oldPosition[i] = position[i];
						}
					}

					SetMousePosition(m_oldPosition);
					m_initialSkipping = false;
					break;
				}

				for (unsigned short i = 0; i < 2; ++i) {
					if (m_use_axis[i]) {
						if (m_reset[i]) {
							setposition[i] = center[i];
							movement[i] -= center[i];
						}
						else {
							setposition[i] = position[i];
							movement[i] -= m_oldPosition[i];
						}

						movement[i] *= -1.0f;

						// Don't apply the rotation when we are under a certain threshold for mouse movement.
						if (((movement[i] > (m_threshold[i] / 10.0f)) ||
							((movement[i] * (-1.0f)) > (m_threshold[i] / 10.0f)))) {

							movement[i] *= m_sensitivity[i];

							if ((m_limit[i][0] != 0.0f) && ((m_angle[i] + movement[i]) <= m_limit[i][0])) {
								movement[i] = m_limit[i][0] - m_angle[i];
							}

							if ((m_limit[i][1] != 0.0f) && ((m_angle[i] + movement[i]) >= m_limit[i][1])) {
								movement[i] = m_limit[i][1] - m_angle[i];
							}

							m_angle[i] += movement[i];

							switch (m_object_axis[i]) {
								case KX_ACT_MOUSE_OBJECT_AXIS_X:
								{
									rotation = mt::vec3(movement[i], 0.0f, 0.0f);
									break;
								}
								case KX_ACT_MOUSE_OBJECT_AXIS_Y:
								{
									rotation = mt::vec3(0.0f, movement[i], 0.0f);
									break;
								}
								case KX_ACT_MOUSE_OBJECT_AXIS_Z:
								{
									rotation = mt::vec3(0.0f, 0.0f, movement[i]);
									break;
								}
								default:
									break;
							}
							parent->ApplyRotation(rotation, m_local[i]);
						}
					}
					else {
						setposition[i] = center[i];
					}
				}

				// only trigger mouse event when it is necessary
				if (m_oldPosition != position) {
					SetMousePosition(setposition);
				}

				m_oldPosition = position;
			}
			break;
		}
		default:
		{
			break;
		}
	}
	return true;
}

EXP_Value *KX_MouseActuator::GetReplica()
{
	KX_MouseActuator *replica = new KX_MouseActuator(*this);

	replica->ProcessReplica();
	return replica;
}

void KX_MouseActuator::ProcessReplica()
{
	SCA_IActuator::ProcessReplica();
}

void KX_MouseActuator::Replace_IScene(SCA_IScene *scene)
{
	/* Changes the event manager when the scene changes in case of lib loading.
	 * Using an event manager in an actuator is not a regular behaviour which is
	 * to avoid if it is possible.
	 */
	SCA_LogicManager *logicmgr = ((KX_Scene *)scene)->GetLogicManager();
	m_eventmgr = (SCA_MouseManager *)logicmgr->FindEventManager(m_eventmgr->GetType());
}

mt::vec2 KX_MouseActuator::GetMousePosition() const
{
	BLI_assert(m_mouse);
	const SCA_InputEvent & xevent = m_mouse->GetInput(SCA_IInputDevice::MOUSEX);
	const SCA_InputEvent & yevent = m_mouse->GetInput(SCA_IInputDevice::MOUSEY);

	return mt::vec2(m_canvas->GetMouseNormalizedX(xevent.m_values[xevent.m_values.size() - 1]),
					m_canvas->GetMouseNormalizedY(yevent.m_values[yevent.m_values.size() - 1]));
}

void KX_MouseActuator::SetMousePosition(const mt::vec2& pos)
{
	int x, y;

	x = (int)(pos.x * m_canvas->GetMaxX());
	y = (int)(pos.y * m_canvas->GetMaxY());

	m_canvas->SetMousePosition(x, y);
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject KX_MouseActuator::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_MouseActuator",
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
	&SCA_IActuator::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_MouseActuator::Methods[] = {
	{"reset", (PyCFunction)KX_MouseActuator::sPyReset, METH_NOARGS, "reset() : undo rotation caused by actuator\n"},
	{nullptr, nullptr} //Sentinel
};



PyAttributeDef KX_MouseActuator::Attributes[] = {
	EXP_PYATTRIBUTE_BOOL_RW("visible", KX_MouseActuator, m_visible),
	EXP_PYATTRIBUTE_BOOL_RW("use_axis_x", KX_MouseActuator, m_use_axis[0]),
	EXP_PYATTRIBUTE_BOOL_RW("use_axis_y", KX_MouseActuator, m_use_axis[1]),
	EXP_PYATTRIBUTE_VECTOR_RW("threshold", 0.0f, 0.5f, KX_MouseActuator, m_threshold, 2),
	EXP_PYATTRIBUTE_BOOL_RW("reset_x", KX_MouseActuator, m_reset[0]),
	EXP_PYATTRIBUTE_BOOL_RW("reset_y", KX_MouseActuator, m_reset[1]),
	EXP_PYATTRIBUTE_INT_ARRAY_RW("object_axis", 0, 2, 1, KX_MouseActuator, m_object_axis, 2),
	EXP_PYATTRIBUTE_BOOL_RW("local_x", KX_MouseActuator, m_local[0]),
	EXP_PYATTRIBUTE_BOOL_RW("local_y", KX_MouseActuator, m_local[1]),
	EXP_PYATTRIBUTE_VECTOR_RW("sensitivity", -FLT_MAX, FLT_MAX, KX_MouseActuator, m_sensitivity, 2),
	EXP_PYATTRIBUTE_RW_FUNCTION("limit_x", KX_MouseActuator, pyattr_get_limit_x, pyattr_set_limit_x),
	EXP_PYATTRIBUTE_RW_FUNCTION("limit_y", KX_MouseActuator, pyattr_get_limit_y, pyattr_set_limit_y),
	EXP_PYATTRIBUTE_RW_FUNCTION("angle", KX_MouseActuator, pyattr_get_angle, pyattr_set_angle),
	EXP_PYATTRIBUTE_NULL    //Sentinel
};

PyObject *KX_MouseActuator::pyattr_get_limit_x(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_MouseActuator *self = static_cast<KX_MouseActuator *>(self_v);
	return PyObjectFrom(mt::vec2(self->m_limit[0]) / (float)M_PI * 180.0f);
}

int KX_MouseActuator::pyattr_set_limit_x(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_MouseActuator *self = static_cast<KX_MouseActuator *>(self_v);

	mt::vec2 vec;
	if (!PyVecTo(value, vec)) {
		return PY_SET_ATTR_FAIL;
	}

	self->m_limit[0] = vec * ((float)M_PI / 180.0f);

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_MouseActuator::pyattr_get_limit_y(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_MouseActuator *self = static_cast<KX_MouseActuator *>(self_v);
	return PyObjectFrom(mt::vec2(self->m_limit[1]) / (float)M_PI * 180.0f);
}

int KX_MouseActuator::pyattr_set_limit_y(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_MouseActuator *self = static_cast<KX_MouseActuator *>(self_v);

	mt::vec2 vec;
	if (!PyVecTo(value, vec)) {
		return PY_SET_ATTR_FAIL;
	}

	self->m_limit[1] = vec * ((float)M_PI / 180.0f);

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_MouseActuator::pyattr_get_angle(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_MouseActuator *self = static_cast<KX_MouseActuator *>(self_v);
	return PyObjectFrom(mt::vec2(self->m_angle) / (float)M_PI * 180.0f);
}

int KX_MouseActuator::pyattr_set_angle(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_MouseActuator *self = static_cast<KX_MouseActuator *>(self_v);

	mt::vec2 vec;
	if (!PyVecTo(value, vec)) {
		return PY_SET_ATTR_FAIL;
	}

	self->m_angle = vec * ((float)M_PI / 180.0f);

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_MouseActuator::PyReset()
{
	mt::vec3 rotation;
	KX_GameObject *parent = static_cast<KX_GameObject *>(GetParent());

	for (unsigned short i = 0; i < 2; ++i) {
		switch (m_object_axis[i]) {
			case KX_ACT_MOUSE_OBJECT_AXIS_X:
			{
				rotation = mt::vec3(-1.0f * m_angle[i], 0.0f, 0.0f);
				break;
			}
			case KX_ACT_MOUSE_OBJECT_AXIS_Y:
			{
				rotation = mt::vec3(0.0f, -1.0f * m_angle[i], 0.0f);
				break;
			}
			case KX_ACT_MOUSE_OBJECT_AXIS_Z:
			{
				rotation = mt::vec3(0.0f, 0.0f, -1.0f * m_angle[i]);
				break;
			}
			default:
				break;
		}
		parent->ApplyRotation(rotation, m_local[i]);
	}

	m_angle = mt::zero2;

	Py_RETURN_NONE;
}

#endif  /* WITH_PYTHON */
