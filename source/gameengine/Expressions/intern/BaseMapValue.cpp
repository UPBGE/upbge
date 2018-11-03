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

/** \file BaseMapValue.h
 *  \ingroup expressions
 */

#include <regex>

#include "EXP_BaseMapValue.h"
#include "EXP_ListValue.h"

#include "CM_Map.h"

EXP_BaseMapValue::EXP_BaseMapValue()
{
}

EXP_BaseMapValue::~EXP_BaseMapValue()
{
}

std::string EXP_BaseMapValue::GetName() const
{
	return "EXP_MapValue";
}

std::string EXP_BaseMapValue::GetText() const
{
	std::string strListRep = "[";
	std::string commastr = "";

	for (const auto& pair : m_map) {
		strListRep += commastr;
		strListRep += pair.second->GetText();
		commastr = ", ";
	}
	strListRep += "]";

	return strListRep;
}

EXP_Value *EXP_BaseMapValue::Find(const std::string& name) const
{
	return CM_MapGetItemNoInsert(m_map, name);
}

bool EXP_BaseMapValue::Contain(const std::string& name) const
{
	return (m_map.find(name) != m_map.end());
}

bool EXP_BaseMapValue::Contain(EXP_Value* value) const
{
	for (const auto& pair : m_map) {
		if (pair.second == value) {
			return true;
		}
	}

	return false;
}

bool EXP_BaseMapValue::Insert(const std::string& name, EXP_Value* value)
{
	const auto status = m_map.emplace(name, value);

	return status.second;
}

bool EXP_BaseMapValue::Remove(const std::string& name)
{
	return (m_map.erase(name) > 0);
}

bool EXP_BaseMapValue::RemoveValue(EXP_Value *value)
{
	return CM_MapRemoveIfItemFound(m_map, value);
}

void EXP_BaseMapValue::Merge(EXP_BaseMapValue& other)
{
	for (const auto& pair : other.m_map) {
		m_map.insert(pair);
	}
}

void EXP_BaseMapValue::Clear()
{
	m_map.clear();
}

int EXP_BaseMapValue::GetCount() const
{
	return m_map.size();
}

bool EXP_BaseMapValue::Empty() const
{
	return m_map.empty();
}

#ifdef WITH_PYTHON

/* --------------------------------------------------------------------- */
/* Python interface ---------------------------------------------------- */
/* --------------------------------------------------------------------- */

Py_ssize_t EXP_BaseMapValue::bufferlen(PyObject *self)
{
	EXP_BaseMapValue *list = static_cast<EXP_BaseMapValue *>(EXP_PROXY_REF(self));
	if (list == nullptr) {
		return 0;
	}

	return (Py_ssize_t)list->GetCount();
}

PyObject *EXP_BaseMapValue::mapping_subscript(PyObject *self, PyObject *key)
{
	EXP_BaseMapValue *list = static_cast<EXP_BaseMapValue *>(EXP_PROXY_REF(self));
	if (list == nullptr) {
		PyErr_SetString(PyExc_SystemError, "value = list[i], " EXP_PROXY_ERROR_MSG);
		return nullptr;
	}

	if (PyUnicode_Check(key)) {
		EXP_Value *item = list->Find(_PyUnicode_AsString(key));
		if (item) {
			return item->GetProxy();
		}
	}

	PyErr_Format(PyExc_KeyError, "list[key]: '%R' key not in list", key);
	return nullptr;
}

int EXP_BaseMapValue::buffer_contains(PyObject *self_v, PyObject *value)
{
	EXP_BaseMapValue *self = static_cast<EXP_BaseMapValue *>(EXP_PROXY_REF(self_v));

	if (self == nullptr) {
		PyErr_SetString(PyExc_SystemError, "val in list, " EXP_PROXY_ERROR_MSG);
		return -1;
	}

	if (PyUnicode_Check(value)) {
		if (self->Contain(_PyUnicode_AsString(value))) {
			return 1;
		}
	}
	// Not dict like at all but this worked before __contains__ was used.
	else if (PyObject_TypeCheck(value, &EXP_Value::Type)) {
		EXP_Value *item = static_cast<EXP_Value *>(EXP_PROXY_REF(value));
		if (self->Contain(item)) {
			return 1;
		}
	}

	return 0;
}

PySequenceMethods EXP_BaseMapValue::as_sequence = {
	bufferlen, //(inquiry)buffer_length, /*sq_length*/
	nullptr, /*sq_concat*/
	nullptr, /*sq_repeat*/
	nullptr, /*sq_item*/
// TODO, slicing in py3
	nullptr, // buffer_slice, /*sq_slice*/
	nullptr, /*sq_ass_item*/
	nullptr, /*sq_ass_slice*/
	(objobjproc)buffer_contains,  /* sq_contains */
	(binaryfunc)nullptr,  /* sq_inplace_concat */
	(ssizeargfunc)nullptr,  /* sq_inplace_repeat */
};

// Is this one used ?
PyMappingMethods EXP_BaseMapValue::instance_as_mapping = {
	bufferlen, /*mp_length*/
	mapping_subscript, /*mp_subscript*/
	nullptr /*mp_ass_subscript*/
};

PyTypeObject EXP_BaseMapValue::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"EXP_ListValue",           /*tp_name*/
	sizeof(EXP_PyObjectPlus_Proxy), /*tp_basicsize*/
	0,              /*tp_itemsize*/
	/* methods */
	py_base_dealloc,            /*tp_dealloc*/
	0,              /*tp_print*/
	0,          /*tp_getattr*/
	0,          /*tp_setattr*/
	0,          /*tp_compare*/
	py_base_repr, /*tp_repr*/
	0,                  /*tp_as_number*/
	&as_sequence, /*tp_as_sequence*/
	&instance_as_mapping,           /*tp_as_mapping*/
	0,                  /*tp_hash*/
	0,              /*tp_call */
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

PyMethodDef EXP_BaseMapValue::Methods[] = {
	{"count", (PyCFunction)EXP_BaseMapValue::sPycount, METH_O},
	{"get", (PyCFunction)EXP_BaseMapValue::sPyget, METH_VARARGS},
	{"filter", (PyCFunction)EXP_BaseMapValue::sPyfilter, METH_VARARGS},
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef EXP_BaseMapValue::Attributes[] = {
	EXP_PYATTRIBUTE_NULL // Sentinel
};

PyObject *EXP_BaseMapValue::Pycount(PyObject *pykey)
{
	int numfound = 0;

	for (auto& pair : m_map) {
		numfound += PyObject_RichCompareBool(pair.second->GetProxy(), pykey, Py_EQ) == 1 ? 1 : 0;
	}

	return PyLong_FromLong(numfound);
}

// Matches python dict.get(key, [default]).
PyObject *EXP_BaseMapValue::Pyget(PyObject *args)
{
	char *key;
	PyObject *def = Py_None;

	if (!PyArg_ParseTuple(args, "s|O:get", &key, &def)) {
		return nullptr;
	}

	EXP_Value *item = Find(key);
	if (item) {
		return item->GetProxy();
	}

	Py_INCREF(def);
	return def;
}

PyObject *EXP_BaseMapValue::Pyfilter(PyObject *args)
{
	const char *namestr = "";
	const char *propstr = "";

	if (!PyArg_ParseTuple(args, "s|s:filter", &namestr, &propstr)) {
		return nullptr;
	}

	if (strlen(namestr) == 0 && strlen(propstr) == 0) {
		PyErr_SetString(PyExc_ValueError, "list.filter(name, prop): empty expressions.");
		return nullptr;
	}

	std::regex namereg;
	std::regex propreg;
	try {
		namereg = std::regex(namestr);
		propreg = std::regex(propstr);
	}
	catch (const std::regex_error& error) {
		PyErr_Format(PyExc_ValueError, "list.filter(name, prop): invalid expression: %s.", error.what());
		return nullptr;
	}

	EXP_ListValue<EXP_Value> *result = new EXP_ListValue<EXP_Value>();

	for (const auto& pair : m_map) {
		EXP_Value *item = pair.second;
		if (strlen(namestr) == 0 || std::regex_match(pair.first, namereg)) {
			if (strlen(propstr) == 0) {
				result->Add(item);
			}
			else {
				const std::vector<std::string> propnames = item->GetPropertyNames();
				for (const std::string& propname : propnames) {
					if (std::regex_match(propname, propreg)) {
						result->Add(item);
						break;
					}
				}
			}
		}
	}

	return result->NewProxy(true);
}

#endif  // WITH_PYTHON
