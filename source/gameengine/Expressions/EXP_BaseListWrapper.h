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
 * Contributor(s): Porteries Tristan.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file EXP_BaseListWrapper.h
 *  \ingroup expressions
 */

#ifdef WITH_PYTHON

#ifndef __EXP_BASELISTWRAPPER_H__
#define __EXP_BASELISTWRAPPER_H__

#include "EXP_Value.h"

class EXP_BaseListWrapper : public EXP_Value
{
	Py_Header
public:

	using GetItemFunction = PyObject *(*)(EXP_PyObjectPlus *, unsigned int);
	using GetItemNameFunction = std::string (*)(EXP_PyObjectPlus *, unsigned int);
	using GetSizeFunction = unsigned int (*)(EXP_PyObjectPlus *);
	using SetItemFunction = bool (*)(EXP_PyObjectPlus *, unsigned int, PyObject *);
private:
	/// The client instance passed as first argument of each callback.
	EXP_PyObjectPlus *m_client;
	/// Weak reference to the client proxy.
	PyObject *m_weakRef;
	/// Returns the list size.
	GetSizeFunction m_getSize;
	/// Returns the list item for the giving index.
	GetItemFunction m_getItem;
	/// Returns name item for the giving index, used for python operator list["name"].
	GetItemNameFunction m_getItemName;
	/// Sets the new item to the index place, return false when failed item conversion.
	SetItemFunction m_setItem;

	/// Flags used to define special behaviours of the list.
	int m_flag;

public:
	enum Flag {
		FLAG_NONE = 0,
		/// Allow iterating on all items and compare the value of it with a research key.
		FLAG_FIND_VALUE = (1 << 0),
		/// Allow no validation using weak ref.
		FLAG_NO_WEAK_REF = (1 << 1)
	};

	EXP_BaseListWrapper(EXP_PyObjectPlus *client,
			GetSizeFunction getSize, GetItemFunction getItem,
			GetItemNameFunction getItemName, SetItemFunction setItem, Flag flag = FLAG_NONE);
	virtual ~EXP_BaseListWrapper();

	/// \section Python Interface
	static bool CheckValid(EXP_BaseListWrapper *list);
	unsigned int GetSize() const;
	PyObject *GetItem(unsigned int index) const;
	std::string GetItemName(unsigned int index) const;
	bool SetItem(int index, PyObject *item);
	bool AllowSetItem() const;
	bool AllowGetItemByName() const;
	bool AllowFindValue() const;

	/// \section EXP_Value Inherited Functions.
	virtual std::string GetName() const;
	virtual std::string GetText() const;

	// Python list operators.
	static PySequenceMethods py_as_sequence;
	// Python dictionnary operators.
	static PyMappingMethods py_as_mapping;

	static Py_ssize_t py_len(PyObject *self);
	static PyObject *py_get_item(PyObject *self, Py_ssize_t index);
	static int py_set_item(PyObject *self, Py_ssize_t index, PyObject *value);
	static PyObject *py_mapping_subscript(PyObject *self, PyObject *key);
	static int py_mapping_ass_subscript(PyObject *self, PyObject *key, PyObject *value);
	static int py_contains(PyObject *self, PyObject *key);

	EXP_PYMETHOD_VARARGS(EXP_BaseListWrapper, Get);
};

#endif // __EXP_BASELISTWRAPPER_H__

#endif // WITH_PYTHON
