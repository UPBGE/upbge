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

/** \file EXP_BaseMapValue.h
 *  \ingroup expressions
 */

#ifndef __EXP_BASEMAPVALUE_H__
#define __EXP_BASEMAPVALUE_H__

#include "EXP_Value.h"

#include <unordered_map>

class EXP_BaseMapValue : public EXP_Value
{
	Py_Header

public:
	using MapType = std::unordered_map<std::string, EXP_Value *>;
	using MapTypeIterator = MapType::iterator;
	using MapTypeConstIterator = MapType::const_iterator;

protected:
	MapType m_map;

	EXP_Value *Find(const std::string& name) const;
	bool Contain(EXP_Value *value) const;
	bool Insert(const std::string& name, EXP_Value *value);
	bool RemoveValue(EXP_Value *value);
	void Merge(EXP_BaseMapValue& other);

public:
	EXP_BaseMapValue();
	virtual ~EXP_BaseMapValue();

	virtual std::string GetName() const;
	virtual std::string GetText() const;

	bool Contain(const std::string& name) const;
	bool Remove(const std::string& name);

	void Clear();
	int GetCount() const;
	bool Empty() const;

#ifdef WITH_PYTHON

	EXP_PYMETHOD_O(EXP_BaseMapValue, count);
	EXP_PYMETHOD_VARARGS(EXP_BaseMapValue, get);
	EXP_PYMETHOD_VARARGS(EXP_BaseMapValue, filter);

	static Py_ssize_t bufferlen(PyObject *self);
	static PyObject *mapping_subscript(PyObject *self, PyObject *key);
	static int buffer_contains(PyObject *self_v, PyObject *value);

	static PySequenceMethods as_sequence;
	static PyMappingMethods instance_as_mapping;
#endif
};

#endif  // __EXP_BASEMAPVALUE_H__

