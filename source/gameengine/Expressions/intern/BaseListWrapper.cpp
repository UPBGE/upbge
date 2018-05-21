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

/** \file ListWrapper.cpp
 *  \ingroup expressions
 */

#ifdef WITH_PYTHON

#include "EXP_BaseListWrapper.h"

EXP_BaseListWrapper::EXP_BaseListWrapper(EXP_PyObjectPlus *client,
			GetSizeFunction getSize, GetItemFunction getItem,
			GetItemNameFunction getItemName, SetItemFunction setItem, Flag flag)
	:m_client(client),
	m_weakRef(nullptr),
	m_getSize(getSize),
	m_getItem(getItem),
	m_getItemName(getItemName),
	m_setItem(setItem),
	m_flag(flag)
{
	if (!(m_flag & FLAG_NO_WEAK_REF)) {
		PyObject *proxy = client->GetProxy();
		m_weakRef = PyWeakref_NewRef(proxy, nullptr);
		Py_DECREF(proxy);
	}
}

EXP_BaseListWrapper::~EXP_BaseListWrapper()
{
	Py_XDECREF(m_weakRef);
}

bool EXP_BaseListWrapper::CheckValid(EXP_BaseListWrapper *list)
{
	if (!list) {
		return false;
	}

	if (list->m_flag & FLAG_NO_WEAK_REF) {
		return true;
	}

	PyObject *proxy = PyWeakref_GET_OBJECT(list->m_weakRef);
	if (proxy == Py_None) {
		return false;
	}

	EXP_PyObjectPlus *ref = EXP_PROXY_REF(proxy);
	if (!ref) {
		return false;
	}

	BLI_assert(ref == list->m_client);

	return true;
}

unsigned int EXP_BaseListWrapper::GetSize() const
{
	return (*m_getSize)(m_client);
}

PyObject *EXP_BaseListWrapper::GetItem(int index) const
{
	return (*m_getItem)(m_client, index);
}

std::string EXP_BaseListWrapper::GetItemName(int index) const
{
	return (*m_getItemName)(m_client, index);
}

bool EXP_BaseListWrapper::SetItem(int index, PyObject *item)
{
	return (*m_setItem)(m_client, index, item);
}

bool EXP_BaseListWrapper::AllowSetItem() const
{
	return m_setItem != nullptr;
}

bool EXP_BaseListWrapper::AllowGetItemByName() const
{
	return m_getItemName != nullptr;
}

bool EXP_BaseListWrapper::AllowFindValue() const
{
	return (m_flag & FLAG_FIND_VALUE);
}

std::string EXP_BaseListWrapper::GetName() const
{
	return "ListWrapper";
}

std::string EXP_BaseListWrapper::GetText() const
{
	std::string strListRep = "[";
	std::string commastr = "";

	for (unsigned int i = 0, size = GetSize(); i < size; ++i) {
		strListRep += commastr;
		strListRep += _PyUnicode_AsString(PyObject_Repr(GetItem(i)));
		commastr = ", ";
	}
	strListRep += "]";

	return strListRep;
}

int EXP_BaseListWrapper::GetValueType() const
{
	return -1;
}

Py_ssize_t EXP_BaseListWrapper::py_len(PyObject *self)
{
	EXP_BaseListWrapper *list = (EXP_BaseListWrapper *)EXP_PROXY_REF(self);
	// Invalid list.
	if (!CheckValid(list)) {
		PyErr_SetString(PyExc_SystemError, "len(EXP_BaseListWrapper), " EXP_PROXY_ERROR_MSG);
		return 0;
	}

	return (Py_ssize_t)list->GetSize();
}

PyObject *EXP_BaseListWrapper::py_get_item(PyObject *self, Py_ssize_t index)
{
	EXP_BaseListWrapper *list = (EXP_BaseListWrapper *)EXP_PROXY_REF(self);
	// Invalid list.
	if (!CheckValid(list)) {
		PyErr_SetString(PyExc_SystemError, "val = EXP_BaseListWrapper[i], " EXP_PROXY_ERROR_MSG);
		return nullptr;
	}

	int size = list->GetSize();

	if (index < 0) {
		index = size + index;
	}
	if (index < 0 || index >= size) {
		PyErr_SetString(PyExc_IndexError, "EXP_BaseListWrapper[i]: List index out of range in EXP_BaseListWrapper");
		return nullptr;
	}

	PyObject *pyobj = list->GetItem(index);

	return pyobj;
}

int EXP_BaseListWrapper::py_set_item(PyObject *self, Py_ssize_t index, PyObject *value)
{
	EXP_BaseListWrapper *list = (EXP_BaseListWrapper *)EXP_PROXY_REF(self);
	// Invalid list.
	if (!CheckValid(list)) {
		PyErr_SetString(PyExc_SystemError, "EXP_BaseListWrapper[i] = val, " EXP_PROXY_ERROR_MSG);
		return -1;
	}

	if (!list->AllowSetItem()) {
		PyErr_SetString(PyExc_TypeError, "EXP_BaseListWrapper's item type doesn't support assignment");
		return -1;
	}

	if (!value) {
		PyErr_SetString(PyExc_TypeError, "EXP_BaseListWrapper doesn't support item deletion");
		return -1;
	}

	int size = list->GetSize();

	if (index < 0) {
		index = size + index;
	}
	if (index < 0 || index >= size) {
		PyErr_SetString(PyExc_IndexError, "EXP_BaseListWrapper[i]: List index out of range in EXP_BaseListWrapper");
		return -1;
	}

	if (!list->SetItem(index, value)) {
		return -1;
	}
	return 0;
}

PyObject *EXP_BaseListWrapper::py_mapping_subscript(PyObject *self, PyObject *key)
{
	EXP_BaseListWrapper *list = (EXP_BaseListWrapper *)EXP_PROXY_REF(self);
	// Invalid list.
	if (!CheckValid(list)) {
		PyErr_SetString(PyExc_SystemError, "val = EXP_BaseListWrapper[key], " EXP_PROXY_ERROR_MSG);
		return nullptr;
	}

	if (PyIndex_Check(key)) {
		Py_ssize_t index = PyLong_AsSsize_t(key);
		return py_get_item(self, index);
	}
	else if (PyUnicode_Check(key)) {
		if (!list->AllowGetItemByName()) {
			PyErr_SetString(PyExc_SystemError, "EXP_BaseListWrapper's item type doesn't support access by key");
			return nullptr;
		}

		const char *name = _PyUnicode_AsString(key);
		int size = list->GetSize();

		for (unsigned int i = 0; i < size; ++i) {
			if (list->GetItemName(i) == name) {
				return list->GetItem(i);
			}
		}

		PyErr_Format(PyExc_KeyError, "requested item \"%s\" does not exist", name);
		return nullptr;
	}

	PyErr_Format(PyExc_KeyError, "EXP_BaseListWrapper[key]: '%R' key not in list", key);
	return nullptr;
}

int EXP_BaseListWrapper::py_mapping_ass_subscript(PyObject *self, PyObject *key, PyObject *value)
{
	EXP_BaseListWrapper *list = (EXP_BaseListWrapper *)EXP_PROXY_REF(self);
	// Invalid list.
	if (!CheckValid(list)) {
		PyErr_SetString(PyExc_SystemError, "val = EXP_BaseListWrapper[key], " EXP_PROXY_ERROR_MSG);
		return -1;
	}

	if (!list->AllowSetItem()) {
		PyErr_SetString(PyExc_TypeError, "EXP_BaseListWrapper's item type doesn't support assignment");
		return -1;
	}

	if (PyIndex_Check(key)) {
		Py_ssize_t index = PyLong_AsSsize_t(key);
		return py_set_item(self, index, value);
	}
	else if (PyUnicode_Check(key)) {
		if (!list->AllowGetItemByName()) {
			PyErr_SetString(PyExc_SystemError, "EXP_BaseListWrapper's item type doesn't support access by key");
			return -1;
		}

		const char *name = _PyUnicode_AsString(key);
		int size = list->GetSize();

		for (unsigned int i = 0; i < size; ++i) {
			if (list->GetItemName(i) == name) {
				if (!list->SetItem(i, value)) {
					return -1;
				}
				return 0;
			}
		}

		PyErr_Format(PyExc_KeyError, "requested item \"%s\" does not exist", name);
		return -1;
	}

	PyErr_Format(PyExc_KeyError, "EXP_BaseListWrapper[key]: '%R' key not in list", key);
	return -1;
}

int EXP_BaseListWrapper::py_contains(PyObject *self, PyObject *key)
{
	EXP_BaseListWrapper *list = (EXP_BaseListWrapper *)EXP_PROXY_REF(self);
	// Invalid list.
	if (!CheckValid(list)) {
		PyErr_SetString(PyExc_SystemError, "val = EXP_BaseListWrapper[i], " EXP_PROXY_ERROR_MSG);
		return -1;
	}

	if (PyUnicode_Check(key)) {
		if (!list->AllowGetItemByName()) {
			PyErr_SetString(PyExc_SystemError, "EXP_BaseListWrapper's item type doesn't support access by key");
			return -1;
		}

		const char *name = _PyUnicode_AsString(key);

		for (unsigned int i = 0, size = list->GetSize(); i < size; ++i) {
			if (list->GetItemName(i) == name) {
				return 1;
			}
		}
	}

	if (list->AllowFindValue()) {
		for (unsigned int i = 0, size = list->GetSize(); i < size; ++i) {
			if (PyObject_RichCompareBool(list->GetItem(i), key, Py_EQ) == 1) {
				return 1;
			}
		}
	}

	return 0;
}

PySequenceMethods EXP_BaseListWrapper::py_as_sequence = {
	py_len, // sq_length
	nullptr, // sq_concat
	nullptr, // sq_repeat
	py_get_item, // sq_item
	nullptr, // sq_slice
	py_set_item, // sq_ass_item
	nullptr, // sq_ass_slice
	(objobjproc)py_contains, // sq_contains
	(binaryfunc)nullptr,  // sq_inplace_concat
	(ssizeargfunc)nullptr,  // sq_inplace_repeat
};

PyMappingMethods EXP_BaseListWrapper::py_as_mapping = {
	py_len, // mp_length
	py_mapping_subscript, // mp_subscript
	py_mapping_ass_subscript // mp_ass_subscript
};

PyTypeObject EXP_BaseListWrapper::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"EXP_ListWrapper", // tp_name
	sizeof(EXP_PyObjectPlus_Proxy), // tp_basicsize
	0, // tp_itemsize
	py_base_dealloc, // tp_dealloc
	0, // tp_print
	0, // tp_getattr
	0, // tp_setattr
	0, // tp_compare
	py_base_repr, // tp_repr
	0, // tp_as_number
	&py_as_sequence, // tp_as_sequence
	&py_as_mapping, // tp_as_mapping
	0, // tp_hash
	0, // tp_call
	0,
	nullptr,
	nullptr,
	0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&EXP_Value::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef EXP_BaseListWrapper::Methods[] = {
	{"get", (PyCFunction)EXP_BaseListWrapper::sPyGet, METH_VARARGS},
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef EXP_BaseListWrapper::Attributes[] = {
	EXP_PYATTRIBUTE_NULL // Sentinel
};

// Matches python dict.get(key, [default]).
PyObject *EXP_BaseListWrapper::PyGet(PyObject *args)
{
	char *name;
	PyObject *def = Py_None;

	// Invalid list.
	if (!CheckValid(this)) {
		PyErr_SetString(PyExc_SystemError, "val = EXP_BaseListWrapper[i], " EXP_PROXY_ERROR_MSG);
		return nullptr;
	}

	if (!AllowGetItemByName()) {
		PyErr_SetString(PyExc_SystemError, "EXP_BaseListWrapper's item type doesn't support access by key");
		return nullptr;
	}

	if (!PyArg_ParseTuple(args, "s|O:get", &name, &def)) {
		return nullptr;
	}

	for (unsigned int i = 0; i < GetSize(); ++i) {
		if (GetItemName(i) == name) {
			return GetItem(i);
		}
	}

	Py_INCREF(def);
	return def;
}

#endif  // WITH_PYTHON
