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

/** \file SCA_InputEvent.h
 *  \ingroup gamelogic
 */

#ifndef __SCA_INPUTEVENT_H__
#define __SCA_INPUTEVENT_H__

#include "EXP_Value.h"

#include <vector>

class SCA_InputEvent : public EXP_Value
{
Py_Header
public:
	enum SCA_EnumInputs {
		NONE = 0,
		JUSTACTIVATED,
		ACTIVE,
		JUSTRELEASED,
	};

	SCA_InputEvent();
	SCA_InputEvent(int type);

	virtual std::string GetName() const;

	/// Clear status, values and queue but keep status and value from before.
	void Clear();

	/// Find an exisiting event or status.
	bool Find(SCA_EnumInputs inputenum) const;

	/// Compare the last event or status with passed value.
	bool End(SCA_EnumInputs inputenum) const;

	/// All recorded status during a frame, always contains one value.
	std::vector<SCA_EnumInputs> m_status;
	/// All recorded event for this input during a frame, can contain none value.
	std::vector<SCA_EnumInputs> m_queue;
	/// All recorded values of this input (used for mouse), always contains one value.
	std::vector<int> m_values;
	/// Keyboard unicode value.
	unsigned int m_unicode;
	/// Event type.
	int m_type;

#ifdef WITH_PYTHON
	unsigned int get_status_size();
	PyObject *get_status_item(unsigned int index);
	unsigned int get_queue_size();
	PyObject *get_queue_item(unsigned int index);
	unsigned int get_values_size();
	PyObject *get_values_item(unsigned int index);

	static PyObject *pyattr_get_status(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_queue(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_values(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_inactive(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_active(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_activated(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_released(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
#endif
};

#endif  // __SCA_IINPUTDEVICE_H__

