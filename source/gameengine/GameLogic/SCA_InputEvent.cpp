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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/GameLogic/SCA_InputEvent.cpp
 *  \ingroup gamelogic
 */


#include "SCA_InputEvent.h"

#include "EXP_ListWrapper.h"

#include <algorithm>

SCA_InputEvent::SCA_InputEvent()
	:m_unicode(0)
{
	m_status.push_back(NONE);
	m_values.push_back(0);
}

SCA_InputEvent::SCA_InputEvent(int type)
	:m_unicode(0),
	m_type(type)
{
	m_status.push_back(NONE);
	m_values.push_back(0);
}

std::string SCA_InputEvent::GetName() const
{
	return "SCA_InputEvent";
}

void SCA_InputEvent::Clear()
{
	SCA_EnumInputs status = m_status[m_status.size() - 1];
	m_status.clear();
	m_status.push_back(status);

	int value = m_values[m_values.size() - 1];
	m_values.clear();
	m_values.push_back(value);

	m_queue.clear();
}

bool SCA_InputEvent::Find(SCA_EnumInputs inputenum) const
{
	if (inputenum == NONE || inputenum == ACTIVE) {
		return std::count(m_status.begin(), m_status.end(), inputenum);
	}
	else {
		return std::count(m_queue.begin(), m_queue.end(), inputenum);
	}
}

bool SCA_InputEvent::End(SCA_EnumInputs inputenum) const
{
	if (inputenum == NONE || inputenum == ACTIVE) {
		return m_status[m_status.size() - 1] == inputenum;
	}
	else {
		if (m_queue.empty()) {
			return false;
		}
		return m_queue[m_queue.size() - 1] == inputenum;
	}
}

#ifdef WITH_PYTHON

PyTypeObject SCA_InputEvent::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"SCA_InputEvent",
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
	0, 0, 0,
	0,
	0, 0, 0,
	Methods,
	0,
	0,
	&EXP_PyObjectPlus::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef SCA_InputEvent::Methods[] = {
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef SCA_InputEvent::Attributes[] = {
	EXP_PYATTRIBUTE_RO_FUNCTION("status", SCA_InputEvent, pyattr_get_status),
	EXP_PYATTRIBUTE_RO_FUNCTION("queue", SCA_InputEvent, pyattr_get_queue),
	EXP_PYATTRIBUTE_RO_FUNCTION("values", SCA_InputEvent, pyattr_get_values),
	EXP_PYATTRIBUTE_RO_FUNCTION("inactive", SCA_InputEvent, pyattr_get_inactive),
	EXP_PYATTRIBUTE_RO_FUNCTION("active", SCA_InputEvent, pyattr_get_active),
	EXP_PYATTRIBUTE_RO_FUNCTION("activated", SCA_InputEvent, pyattr_get_activated),
	EXP_PYATTRIBUTE_RO_FUNCTION("released", SCA_InputEvent, pyattr_get_released),
	EXP_PYATTRIBUTE_INT_RO("type", SCA_InputEvent, m_type),
	EXP_PYATTRIBUTE_NULL //Sentinel
};

unsigned int SCA_InputEvent::get_status_size()
{
	return m_status.size();
}

PyObject *SCA_InputEvent::get_status_item(unsigned int index)
{
	return PyLong_FromLong(m_status[index]);
}

PyObject *SCA_InputEvent::pyattr_get_status(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	return (new EXP_ListWrapper<SCA_InputEvent, &SCA_InputEvent::get_status_size, &SCA_InputEvent::get_status_item>
		(self_v, EXP_BaseListWrapper::FLAG_FIND_VALUE))->NewProxy(true);
}

unsigned int SCA_InputEvent::get_queue_size()
{
	return m_queue.size();
}

PyObject *SCA_InputEvent::get_queue_item(unsigned int index)
{
	return PyLong_FromLong(m_queue[index]);
}

PyObject *SCA_InputEvent::pyattr_get_queue(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	return (new EXP_ListWrapper<SCA_InputEvent, &SCA_InputEvent::get_queue_size, &SCA_InputEvent::get_queue_item>
		(self_v, EXP_BaseListWrapper::FLAG_FIND_VALUE))->NewProxy(true);
}

unsigned int SCA_InputEvent::get_values_size()
{
	return m_values.size();
}

PyObject *SCA_InputEvent::get_values_item(unsigned int index)
{
	return PyLong_FromLong(m_values[index]);
}

PyObject *SCA_InputEvent::pyattr_get_values(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	return (new EXP_ListWrapper<SCA_InputEvent, &SCA_InputEvent::get_values_size, &SCA_InputEvent::get_values_item>
		(self_v, EXP_BaseListWrapper::FLAG_FIND_VALUE))->NewProxy(true);
}

PyObject *SCA_InputEvent::pyattr_get_inactive(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	SCA_InputEvent *self = static_cast<SCA_InputEvent *>(self_v);

	return PyBool_FromLong(self->Find(SCA_InputEvent::NONE));
}

PyObject *SCA_InputEvent::pyattr_get_active(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	SCA_InputEvent *self = static_cast<SCA_InputEvent *>(self_v);

	return PyBool_FromLong(self->Find(SCA_InputEvent::ACTIVE));
}

PyObject *SCA_InputEvent::pyattr_get_activated(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	SCA_InputEvent *self = static_cast<SCA_InputEvent *>(self_v);

	return PyBool_FromLong(self->Find(SCA_InputEvent::JUSTACTIVATED));
}

PyObject *SCA_InputEvent::pyattr_get_released(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	SCA_InputEvent *self = static_cast<SCA_InputEvent *>(self_v);

	return PyBool_FromLong(self->Find(SCA_InputEvent::JUSTRELEASED));
}

#endif  // WITH_PYTHON
