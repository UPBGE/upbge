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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file EXP_PythonValue.h
 *  \ingroup expressions
 */

#ifndef __EXP_PYTHONVALUE_H__
#define __EXP_PYTHONVALUE_H__


#include "EXP_Value.h"

/** \brief Property holding a python object, used for properties when no defined
 * Expression class fit the type.
 * Note: This file is compiled only when enabling python.
 */
class EXP_PythonValue : public EXP_PropValue
{
public:
	EXP_PythonValue(PyObject *object);
	virtual ~EXP_PythonValue();

	virtual std::string GetText() const;
	virtual DATA_TYPE GetValueType() const;

	PyObject *GetValue() const;

	virtual EXP_PropValue *GetReplica();
	void ProcessReplica();

	virtual PyObject *ConvertValueToPython();

private:
	PyObject *m_value;
};

#endif  // __EXP_PYTHONVALUE_H__
