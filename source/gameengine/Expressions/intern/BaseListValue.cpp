/** \file gameengine/Expressions/BaseListValue.cpp
 *  \ingroup expressions
 */
// ListValue.cpp: implementation of the EXP_BaseListValue class.
//
//////////////////////////////////////////////////////////////////////
/*
 * Copyright (c) 1996-2000 Erwin Coumans <coockie@acm.org>
 *
 * Permission to use, copy, modify, distribute and sell this software
 * and its documentation for any purpose is hereby granted without fee,
 * provided that the above copyright notice appear in all copies and
 * that both that copyright notice and this permission notice appear
 * in supporting documentation.  Erwin Coumans makes no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 */

#include <stdio.h>
#include <regex>

#include "EXP_ListValue.h"
#include "EXP_PropString.h"
#include <algorithm>
#include "EXP_PropBool.h"

#include "BLI_sys_types.h" // For intptr_t support.

EXP_BaseListValue::EXP_BaseListValue()
{
}

EXP_BaseListValue::~EXP_BaseListValue()
{
}

void EXP_BaseListValue::SetValue(int i, EXP_Value *val)
{
	m_valueArray[i] = val;
}

EXP_Value *EXP_BaseListValue::GetValue(int i) const
{
	return m_valueArray[i];
}

EXP_Value *EXP_BaseListValue::FindValue(const std::string& name) const
{
	const VectorTypeConstIterator it = std::find_if(m_valueArray.begin(), m_valueArray.end(),
	                                                [&name](EXP_Value *item) {
		return item->GetName() == name;
	});

	if (it != m_valueArray.end()) {
		return *it;
	}
	return nullptr;
}

bool EXP_BaseListValue::SearchValue(EXP_Value *val) const
{
	return (std::find(m_valueArray.begin(), m_valueArray.end(), val) != m_valueArray.end());
}

void EXP_BaseListValue::Add(EXP_Value *value)
{
	m_valueArray.push_back(value);
}

void EXP_BaseListValue::Insert(unsigned int i, EXP_Value *value)
{
	m_valueArray.insert(m_valueArray.begin() + i, value);
}

bool EXP_BaseListValue::RemoveValue(EXP_Value *val)
{
	bool result = false;
	for (VectorTypeIterator it = m_valueArray.begin(); it != m_valueArray.end(); ) {
		if (*it == val) {
			it = m_valueArray.erase(it);
			result = true;
		}
		else {
			++it;
		}
	}
	return result;
}

void EXP_BaseListValue::MergeList(EXP_BaseListValue& other)
{
	const unsigned int otherSize = other.GetCount();
	const unsigned int size = m_valueArray.size();

	m_valueArray.resize(size + otherSize);

	for (unsigned int i = 0; i < otherSize; ++i) {
		m_valueArray[i + size] = other.GetValue(i);
	}

	other.Clear();
}

std::string EXP_BaseListValue::GetName() const
{
	return "EXP_ListValue";
}

std::string EXP_BaseListValue::GetText() const
{
	std::string strListRep = "[";
	std::string commastr = "";

	for (EXP_Value *item : m_valueArray) {
		strListRep += commastr;
		strListRep += item->GetText();
		commastr = ", ";
	}
	strListRep += "]";

	return strListRep;
}

void EXP_BaseListValue::Clear()
{
	m_valueArray.clear();
}

void EXP_BaseListValue::Remove(int i)
{
	m_valueArray.erase(m_valueArray.begin() + i);
}

void EXP_BaseListValue::Resize(int num)
{
	m_valueArray.resize(num);
}

int EXP_BaseListValue::GetCount() const
{
	return m_valueArray.size();
}

bool EXP_BaseListValue::Empty() const
{
	return m_valueArray.empty();
}

#ifdef WITH_PYTHON

/* --------------------------------------------------------------------- */
/* Python interface ---------------------------------------------------- */
/* --------------------------------------------------------------------- */

Py_ssize_t EXP_BaseListValue::bufferlen(PyObject *self)
{
	EXP_BaseListValue *list = static_cast<EXP_BaseListValue *>(EXP_PROXY_REF(self));
	if (list == nullptr) {
		return 0;
	}

	return (Py_ssize_t)list->GetCount();
}

PyObject *EXP_BaseListValue::buffer_item(PyObject *self, Py_ssize_t index)
{
	EXP_BaseListValue *list = static_cast<EXP_BaseListValue *>(EXP_PROXY_REF(self));

	if (list == nullptr) {
		PyErr_SetString(PyExc_SystemError, "val = list[i], " EXP_PROXY_ERROR_MSG);
		return nullptr;
	}

	int count = list->GetCount();

	if (index < 0) {
		index = count + index;
	}

	if (index < 0 || index >= count) {
		PyErr_SetString(PyExc_IndexError, "list[i]: Python ListIndex out of range in EXP_ValueList");
		return nullptr;
	}

	EXP_Value *cval = list->GetValue(index);
	return cval->GetProxy();
}

// Just slice it into a python list...
PyObject *EXP_BaseListValue::buffer_slice(EXP_BaseListValue *list, Py_ssize_t start, Py_ssize_t stop)
{
	PyObject *newlist = PyList_New(stop - start);
	if (!newlist) {
		return nullptr;
	}

	for (Py_ssize_t i = start, j = 0; i < stop; i++, j++) {
		PyObject *pyobj = list->GetValue(i)->GetProxy();
		PyList_SET_ITEM(newlist, j, pyobj);
	}
	return newlist;
}


PyObject *EXP_BaseListValue::mapping_subscript(PyObject *self, PyObject *key)
{
	EXP_BaseListValue *list = static_cast<EXP_BaseListValue *>(EXP_PROXY_REF(self));
	if (list == nullptr) {
		PyErr_SetString(PyExc_SystemError, "value = list[i], " EXP_PROXY_ERROR_MSG);
		return nullptr;
	}

	if (PyUnicode_Check(key)) {
		EXP_Value *item = list->FindValue(_PyUnicode_AsString(key));
		if (item) {
			return item->GetProxy();
		}
	}
	else if (PyIndex_Check(key)) {
		Py_ssize_t index = PyLong_AsSsize_t(key);
		return buffer_item(self, index); // Wont add a ref.
	}
	else if (PySlice_Check(key)) {
		Py_ssize_t start, stop, step, slicelength;

		if (PySlice_GetIndicesEx(key, list->GetCount(), &start, &stop, &step, &slicelength) < 0) {
			return nullptr;
		}

		if (slicelength <= 0) {
			return PyList_New(0);
		}
		else if (step == 1) {
			return buffer_slice(list, start, stop);
		}
		else {
			PyErr_SetString(PyExc_TypeError, "list[slice]: slice steps not supported");
			return nullptr;
		}
	}

	PyErr_Format(PyExc_KeyError, "list[key]: '%R' key not in list", key);
	return nullptr;
}

int EXP_BaseListValue::buffer_contains(PyObject *self_v, PyObject *value)
{
	EXP_BaseListValue *self = static_cast<EXP_BaseListValue *>(EXP_PROXY_REF(self_v));

	if (self == nullptr) {
		PyErr_SetString(PyExc_SystemError, "val in list, " EXP_PROXY_ERROR_MSG);
		return -1;
	}

	if (PyUnicode_Check(value)) {
		if (self->FindValue(_PyUnicode_AsString(value))) {
			return 1;
		}
	}
	// Not dict like at all but this worked before __contains__ was used.
	else if (PyObject_TypeCheck(value, &EXP_Value::Type)) {
		EXP_Value *item = static_cast<EXP_Value *>(EXP_PROXY_REF(value));
		for (int i = 0; i < self->GetCount(); i++) {
			if (self->GetValue(i) == item) {
				return 1;
			}
		}
	}

	return 0;
}

PySequenceMethods EXP_BaseListValue::as_sequence = {
	bufferlen, //(inquiry)buffer_length, /*sq_length*/
	nullptr, /*sq_concat*/
	nullptr, /*sq_repeat*/
	buffer_item, /*sq_item*/
// TODO, slicing in py3
	nullptr, // buffer_slice, /*sq_slice*/
	nullptr, /*sq_ass_item*/
	nullptr, /*sq_ass_slice*/
	(objobjproc)buffer_contains,  /* sq_contains */
	(binaryfunc)nullptr,  /* sq_inplace_concat */
	(ssizeargfunc)nullptr,  /* sq_inplace_repeat */
};

// Is this one used ?
PyMappingMethods EXP_BaseListValue::instance_as_mapping = {
	bufferlen, /*mp_length*/
	mapping_subscript, /*mp_subscript*/
	nullptr /*mp_ass_subscript*/
};

PyTypeObject EXP_BaseListValue::Type = {
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

PyMethodDef EXP_BaseListValue::Methods[] = {
	{"index", (PyCFunction)EXP_BaseListValue::sPyindex, METH_O},
	{"count", (PyCFunction)EXP_BaseListValue::sPycount, METH_O},

	// Dict style access.
	{"get", (PyCFunction)EXP_BaseListValue::sPyget, METH_VARARGS},
	{"filter", (PyCFunction)EXP_BaseListValue::sPyfilter, METH_VARARGS},

	// Own cvalue funcs.
	{"from_id", (PyCFunction)EXP_BaseListValue::sPyfrom_id, METH_O},

	{nullptr, nullptr} // Sentinel
};

PyAttributeDef EXP_BaseListValue::Attributes[] = {
	EXP_PYATTRIBUTE_NULL // Sentinel
};

PyObject *EXP_BaseListValue::Pyindex(PyObject *pykey)
{
	for (unsigned short i = 0, size = m_valueArray.size(); i < size; ++i) {
		if (PyObject_RichCompareBool(m_valueArray[i]->GetProxy(), pykey, Py_EQ) == 1) {
			return PyLong_FromLong(i);
		}
	}

	PyErr_SetString(PyExc_ValueError, "list.index(x): x not in EXP_BaseListValue");
	return nullptr;
}

PyObject *EXP_BaseListValue::Pycount(PyObject *pykey)
{
	int numfound = 0;

	for (EXP_Value *value : m_valueArray) {
		numfound += PyObject_RichCompareBool(value->GetProxy(), pykey, Py_EQ) == 1 ? 1 : 0;
	}

	return PyLong_FromLong(numfound);
}

// Matches python dict.get(key, [default]).
PyObject *EXP_BaseListValue::Pyget(PyObject *args)
{
	char *key;
	PyObject *def = Py_None;

	if (!PyArg_ParseTuple(args, "s|O:get", &key, &def)) {
		return nullptr;
	}

	EXP_Value *item = FindValue(key);
	if (item) {
		return item->GetProxy();
	}

	Py_INCREF(def);
	return def;
}

PyObject *EXP_BaseListValue::Pyfilter(PyObject *args)
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

	for (EXP_Value *item : m_valueArray) {
		if (strlen(namestr) == 0 || std::regex_match(item->GetName(), namereg)) {
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

PyObject *EXP_BaseListValue::Pyfrom_id(PyObject *value)
{
	uintptr_t id = (uintptr_t)PyLong_AsVoidPtr(value);

	if (PyErr_Occurred()) {
		return nullptr;
	}

	int numelem = GetCount();
	for (int i = 0; i < numelem; i++) {
		if (reinterpret_cast<uintptr_t>(m_valueArray[i]->m_proxy) == id) {
			return GetValue(i)->GetProxy();
		}
	}

	PyErr_SetString(PyExc_IndexError, "from_id(#): id not found in EXP_ValueList");
	return nullptr;
}

#endif  // WITH_PYTHON
