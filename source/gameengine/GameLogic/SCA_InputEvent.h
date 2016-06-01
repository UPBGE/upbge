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

/** \file SCA_InputEvent.h
 *  \ingroup gamelogic
 *  \brief Interface for input devices. The defines for keyboard/system/mouse events
 *   here are for internal use in the KX module.
 *
 */

#ifndef __SCA_INPUTEVENT_H__
#define __SCA_INPUTEVENT_H__

#include "EXP_PyObjectPlus.h"

#include <vector>

class SCA_InputEvent : public PyObjectPlus
{
Py_Header
public:
	enum SCA_EnumInputs {
		KX_NONE = 0,
		KX_JUSTACTIVATED,
		KX_ACTIVE,
		KX_JUSTRELEASED,
	};

	SCA_InputEvent();

	/// Clear status, values and queue but keep status and value from before.
	void Clear();

	/// Find an exisiting event or status.
	bool Find(SCA_EnumInputs inputenum) const;

	std::vector<SCA_EnumInputs> m_status;
	std::vector<SCA_EnumInputs> m_queue;
	std::vector<int> m_values;
	unsigned int m_unicode;

#ifdef WITH_PYTHON
	static int get_status_size_cb(void *self_v);
	static PyObject *get_status_item_cb(void *self_v, int index);
	static int get_queue_size_cb(void *self_v);
	static PyObject *get_queue_item_cb(void *self_v, int index);
	static int get_values_size_cb(void *self_v);
	static PyObject *get_values_item_cb(void *self_v, int index);

	static PyObject *pyattr_get_status(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_queue(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_values(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);

	static PyObject *tp_richcompare(PyObject *a, PyObject *b, int op);
#endif
};

#endif  // __SCA_IINPUTDEVICE_H__

