/** \file gameengine/Expressions/ListValue.cpp
 *  \ingroup expressions
 */
// ListValue.cpp: implementation of the CListValue class.
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
#include "EXP_StringValue.h"
#include <algorithm>
#include "EXP_BoolValue.h"

#include "BLI_sys_types.h" // For intptr_t support.

CListValue::CListValue()
	:m_bReleaseContents(true)
{
}

CListValue::~CListValue()
{
	if (m_bReleaseContents) {
		for (CValue *item : m_pValueArray) {
			item->Release();
		}
	}
}

const std::string CListValue::GetText()
{
	std::string strListRep = "[";
	std::string commastr = "";

	for (CValue *item : m_pValueArray) {
		strListRep += commastr;
		strListRep += item->GetText();
		commastr = ",";
	}
	strListRep += "]";

	return strListRep;
}

CValue *CListValue::GetReplica()
{
	CListValue *replica = new CListValue(*this);

	replica->ProcessReplica();

	replica->m_bReleaseContents = true; // For copy, complete array is copied for now...
	// Copy all values.
	const int numelements = m_pValueArray.size();
	replica->m_pValueArray.resize(numelements);
	for (unsigned int i = 0; i < numelements; i++) {
		replica->m_pValueArray[i] = m_pValueArray[i]->GetReplica();
	}

	return replica;
}

void CListValue::SetValue(int i, CValue *val)
{
	BLI_assert(i < m_pValueArray.size());
	m_pValueArray[i] = val;
}

CValue *CListValue::GetValue(int i)
{
	BLI_assert(i < m_pValueArray.size());
	return m_pValueArray[i];
}

CValue *CListValue::GetFront()
{
	return m_pValueArray.front();
}

CValue *CListValue::GetBack()
{
	return m_pValueArray.back();
}

int CListValue::GetCount()
{
	return m_pValueArray.size();
}

CListValue::VectorTypeIterator CListValue::GetBegin() const
{
	return m_pValueArray.cbegin();
}

CListValue::VectorTypeIterator CListValue::GetEnd() const
{
	return m_pValueArray.cend();
}

void CListValue::Resize(int num)
{
	m_pValueArray.resize(num);
}

void CListValue::Remove(int i)
{
	BLI_assert(i < m_pValueArray.size());
	m_pValueArray.erase(m_pValueArray.begin() + i);
}

void CListValue::ReleaseAndRemoveAll()
{
	for (CValue *item : m_pValueArray) {
		item->Release();
	}
	m_pValueArray.clear();
}

CValue *CListValue::FindValue(const std::string &name)
{
	VectorTypeIterator it = std::find_if(m_pValueArray.begin(), m_pValueArray.end(),
			[&name](CValue *item) { return item->GetName() == name; });

	if (it != m_pValueArray.end()) {
		return *it;
	}
	return nullptr;
}

bool CListValue::SearchValue(CValue *val)
{
	VectorTypeIterator it = std::find(m_pValueArray.begin(), m_pValueArray.end(), val);
	if (it != m_pValueArray.end()) {
		return true;
	}
	return false;
}

void CListValue::SetReleaseOnDestruct(bool bReleaseContents)
{
	m_bReleaseContents = bReleaseContents;
}

bool CListValue::RemoveValue(CValue *val)
{
	bool result = false;

	for (VectorTypeIterator it = m_pValueArray.begin(); it != m_pValueArray.end();) {
		if (*it == val) {
			it = m_pValueArray.erase(it);
			result = true;
		}
		else {
			++it;
		}
	}

	return result;
}

void CListValue::MergeList(CListValue *otherlist)
{
	int numelements = this->GetCount();
	int numotherelements = otherlist->GetCount();

	Resize(numelements + numotherelements);

	for (int i = 0; i < numotherelements; i++) {
		SetValue(i + numelements, otherlist->GetValue(i)->AddRef());
	}
}

bool CListValue::CheckEqual(CValue *first, CValue *second)
{
	bool result = false;

	CValue *eqval = ((CValue *)first)->Calc(VALUE_EQL_OPERATOR, (CValue *)second);

	if (eqval == nullptr) {
		return false;
	}
	const std::string& text = eqval->GetText();
	if (text == CBoolValue::sTrueString) {
		result = true;
	}
	eqval->Release();
	return result;
}

void CListValue::Add(CValue *value)
{
	m_pValueArray.push_back(value);
}

void CListValue::Insert(unsigned int i, CValue *value)
{
	m_pValueArray.insert(m_pValueArray.begin() + i, value);
}

int CListValue::GetValueType()
{
	return VALUE_LIST_TYPE;
}

#ifdef WITH_PYTHON

/* --------------------------------------------------------------------- */
/* Python interface ---------------------------------------------------- */
/* --------------------------------------------------------------------- */

static Py_ssize_t listvalue_bufferlen(PyObject *self)
{
	CListValue *list = static_cast<CListValue *>(BGE_PROXY_REF(self));
	if (list == nullptr) {
		return 0;
	}

	return (Py_ssize_t)list->GetCount();
}

static PyObject *listvalue_buffer_item(PyObject *self, Py_ssize_t index)
{
	CListValue *list = static_cast<CListValue *>(BGE_PROXY_REF(self));

	if (list == nullptr) {
		PyErr_SetString(PyExc_SystemError, "val = CList[i], " BGE_PROXY_ERROR_MSG);
		return nullptr;
	}

	int count = list->GetCount();

	if (index < 0) {
		index = count + index;
	}

	if (index < 0 || index >= count) {
		PyErr_SetString(PyExc_IndexError, "CList[i]: Python ListIndex out of range in CValueList");
		return nullptr;
	}

	CValue *cval = list->GetValue(index);

	PyObject *pyobj = cval->ConvertValueToPython();
	if (pyobj) {
		return pyobj;
	}
	else {
		return cval->GetProxy();
	}
}

// Just slice it into a python list...
static PyObject *listvalue_buffer_slice(CListValue *list, Py_ssize_t start, Py_ssize_t stop)
{
	PyObject *newlist = PyList_New(stop - start);
	if (!newlist) {
		return nullptr;
	}

	for (Py_ssize_t i = start, j = 0; i < stop; i++, j++) {
		PyObject *pyobj = list->GetValue(i)->ConvertValueToPython();
		if (!pyobj) {
			pyobj = list->GetValue(i)->GetProxy();
		}
		PyList_SET_ITEM(newlist, j, pyobj);
	}
	return newlist;
}


static PyObject *listvalue_mapping_subscript(PyObject *self, PyObject *key)
{
	CListValue *list = static_cast<CListValue *>(BGE_PROXY_REF(self));
	if (list == nullptr) {
		PyErr_SetString(PyExc_SystemError, "value = CList[i], " BGE_PROXY_ERROR_MSG);
		return nullptr;
	}

	if (PyUnicode_Check(key)) {
		CValue *item = ((CListValue *)list)->FindValue(_PyUnicode_AsString(key));
		if (item) {
			PyObject *pyobj = item->ConvertValueToPython();
			if (pyobj) {
				return pyobj;
			}
			else {
				return item->GetProxy();
			}
		}
	}
	else if (PyIndex_Check(key)) {
		Py_ssize_t index = PyLong_AsSsize_t(key);
		return listvalue_buffer_item(self, index); // Wont add a ref.
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
			return listvalue_buffer_slice(list, start, stop);
		}
		else {
			PyErr_SetString(PyExc_TypeError, "CList[slice]: slice steps not supported");
			return nullptr;
		}
	}

	PyErr_Format(PyExc_KeyError, "CList[key]: '%R' key not in list", key);
	return nullptr;
}

// clist + list, return a list that python owns.
static PyObject *listvalue_buffer_concat(PyObject *self, PyObject *other)
{
	CListValue *listval = static_cast<CListValue *>(BGE_PROXY_REF(self));

	if (listval == nullptr) {
		PyErr_SetString(PyExc_SystemError, "CList+other, " BGE_PROXY_ERROR_MSG);
		return nullptr;
	}

	Py_ssize_t numitems_orig = listval->GetCount();

	/* For now, we support CListValue concatenated with items
	 * and CListValue concatenated to Python Lists
	 * and CListValue concatenated with another CListValue.
	 */

	// Shallow copy, don't use listval->GetReplica(), it will screw up with KX_GameObjects.
	CListValue *listval_new = new CListValue();

	if (PyList_Check(other)) {
		Py_ssize_t numitems = PyList_GET_SIZE(other);

		// Copy the first part of the list.
		listval_new->Resize(numitems_orig + numitems);
		for (Py_ssize_t i = 0; i < numitems_orig; i++) {
			listval_new->SetValue(i, listval->GetValue(i)->AddRef());
		}

		for (Py_ssize_t i = 0; i < numitems; i++) {
			CValue *listitemval = listval->ConvertPythonToValue(PyList_GET_ITEM(other, i), true, "cList + pyList: CListValue, ");

			if (listitemval) {
				listval_new->SetValue(i + numitems_orig, listitemval);
			}
			else {
				listval_new->Resize(numitems_orig + i); // Resize so we don't try release nullptr pointers.
				listval_new->Release();
				return nullptr; // ConvertPythonToValue above sets the error.
			}
		}
	}
	else if (PyObject_TypeCheck(other, &CListValue::Type)) {
		// Add items from otherlist to this list.
		CListValue *otherval = static_cast<CListValue *>(BGE_PROXY_REF(other));
		if (otherval == nullptr) {
			listval_new->Release();
			PyErr_SetString(PyExc_SystemError, "CList+other, " BGE_PROXY_ERROR_MSG);
			return nullptr;
		}

		Py_ssize_t numitems = otherval->GetCount();

		// Copy the first part of the list.
		listval_new->Resize(numitems_orig + numitems); // Resize so we don't try release nullptr pointers.
		for (Py_ssize_t i = 0; i < numitems_orig; i++) {
			listval_new->SetValue(i, listval->GetValue(i)->AddRef());
		}

		// Now copy the other part of the list.
		for (Py_ssize_t i = 0; i < numitems; i++) {
			listval_new->SetValue(i + numitems_orig, otherval->GetValue(i)->AddRef());
		}

	}
	return listval_new->NewProxy(true); // Python owns this list.
}

static int listvalue_buffer_contains(PyObject *self_v, PyObject *value)
{
	CListValue *self = static_cast<CListValue *>(BGE_PROXY_REF(self_v));

	if (self == nullptr) {
		PyErr_SetString(PyExc_SystemError, "val in CList, " BGE_PROXY_ERROR_MSG);
		return -1;
	}

	if (PyUnicode_Check(value)) {
		if (self->FindValue(_PyUnicode_AsString(value))) {
			return 1;
		}
	}
	// Not dict like at all but this worked before __contains__ was used.
	else if (PyObject_TypeCheck(value, &CValue::Type)) {
		CValue *item = static_cast<CValue *>(BGE_PROXY_REF(value));
		for (int i = 0; i < self->GetCount(); i++) {
			if (self->GetValue(i) == item) {
				return 1;
			}
		}
	}

	return 0;
}

static PySequenceMethods listvalue_as_sequence = {
	listvalue_bufferlen, //(inquiry)buffer_length, /*sq_length*/
	listvalue_buffer_concat, /*sq_concat*/
	nullptr, /*sq_repeat*/
	listvalue_buffer_item, /*sq_item*/
// TODO, slicing in py3
	nullptr, // listvalue_buffer_slice, /*sq_slice*/
	nullptr, /*sq_ass_item*/
	nullptr, /*sq_ass_slice*/
	(objobjproc)listvalue_buffer_contains,  /* sq_contains */
	(binaryfunc)nullptr,  /* sq_inplace_concat */
	(ssizeargfunc)nullptr,  /* sq_inplace_repeat */
};

// Is this one used ?
static PyMappingMethods instance_as_mapping = {
	listvalue_bufferlen, /*mp_length*/
	listvalue_mapping_subscript, /*mp_subscript*/
	nullptr /*mp_ass_subscript*/
};

PyTypeObject CListValue::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"CListValue",           /*tp_name*/
	sizeof(PyObjectPlus_Proxy), /*tp_basicsize*/
	0,              /*tp_itemsize*/
	/* methods */
	py_base_dealloc,            /*tp_dealloc*/
	0,              /*tp_print*/
	0,          /*tp_getattr*/
	0,          /*tp_setattr*/
	0,          /*tp_compare*/
	py_base_repr, /*tp_repr*/
	0,                  /*tp_as_number*/
	&listvalue_as_sequence, /*tp_as_sequence*/
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
	&CValue::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef CListValue::Methods[] = {
	// List style access.
	{"append", (PyCFunction)CListValue::sPyappend, METH_O},
	{"reverse", (PyCFunction)CListValue::sPyreverse, METH_NOARGS},
	{"index", (PyCFunction)CListValue::sPyindex, METH_O},
	{"count", (PyCFunction)CListValue::sPycount, METH_O},

	// Dict style access.
	{"get", (PyCFunction)CListValue::sPyget, METH_VARARGS},
	{"filter", (PyCFunction)CListValue::sPyfilter, METH_VARARGS},

	// Own cvalue funcs.
	{"from_id", (PyCFunction)CListValue::sPyfrom_id, METH_O},

	{nullptr, nullptr} // Sentinel
};

PyAttributeDef CListValue::Attributes[] = {
	KX_PYATTRIBUTE_NULL // Sentinel
};

PyObject *CListValue::Pyappend(PyObject *value)
{
	CValue *objval = ConvertPythonToValue(value, true, "CList.append(i): CValueList, ");

	if (!objval) {
		// ConvertPythonToValue sets the error.
		return nullptr;
	}

	if (!BGE_PROXY_PYOWNS(m_proxy)) {
		PyErr_SetString(PyExc_TypeError, "CList.append(i): internal values can't be modified");
		return nullptr;
	}

	Add(objval);

	Py_RETURN_NONE;
}

PyObject *CListValue::Pyreverse()
{
	if (!BGE_PROXY_PYOWNS(m_proxy)) {
		PyErr_SetString(PyExc_TypeError, "CList.reverse(): internal values can't be modified");
		return nullptr;
	}

	std::reverse(m_pValueArray.begin(), m_pValueArray.end());
	Py_RETURN_NONE;
}

PyObject *CListValue::Pyindex(PyObject *value)
{
	PyObject *result = nullptr;

	CValue *checkobj = ConvertPythonToValue(value, true, "val = cList[i]: CValueList, ");
	if (checkobj == nullptr) {
		// ConvertPythonToValue sets the error.
		return nullptr;
	}
	int numelem = GetCount();
	for (int i = 0; i < numelem; i++) {
		CValue *elem = GetValue(i);
		if (checkobj == elem || CheckEqual(checkobj, elem)) {
			result = PyLong_FromLong(i);
			break;
		}
	}
	checkobj->Release();

	if (result == nullptr) {
		PyErr_SetString(PyExc_ValueError, "CList.index(x): x not in CListValue");
	}
	return result;
}

PyObject *CListValue::Pycount(PyObject *value)
{
	int numfound = 0;

	CValue *checkobj = ConvertPythonToValue(value, false, ""); // Error ignored.

	// in this case just return that there are no items in the list.
	if (checkobj == nullptr) {
		PyErr_Clear();
		return PyLong_FromLong(0);
	}

	int numelem = GetCount();
	for (int i = 0; i < numelem; i++) {
		CValue *elem = GetValue(i);
		if (checkobj == elem || CheckEqual(checkobj, elem)) {
			numfound++;
		}
	}
	checkobj->Release();

	return PyLong_FromLong(numfound);
}

// Matches python dict.get(key, [default]).
PyObject *CListValue::Pyget(PyObject *args)
{
	char *key;
	PyObject *def = Py_None;

	if (!PyArg_ParseTuple(args, "s|O:get", &key, &def)) {
		return nullptr;
	}

	CValue *item = FindValue(key);
	if (item) {
		PyObject *pyobj = item->ConvertValueToPython();
		if (pyobj) {
			return pyobj;
		}
		else {
			return item->GetProxy();
		}
	}

	Py_INCREF(def);
	return def;
}

PyObject *CListValue::Pyfilter(PyObject *args)
{
	const char *namestr = "";
	const char *propstr = "";

	if (!PyArg_ParseTuple(args, "s|s:filter", &namestr, &propstr)) {
		return nullptr;
	}

	if (strlen(namestr) == 0 && strlen(propstr) == 0) {
		PyErr_SetString(PyExc_ValueError, "CList.filter(name, prop): empty expressions.");
		return nullptr;
	}

	std::regex namereg;
	std::regex propreg;
	try {
		namereg = std::regex(namestr);
		propreg = std::regex(propstr);
	}
	catch (const std::regex_error& error) {
		PyErr_Format(PyExc_ValueError, "CList.filter(name, prop): invalid expression: %s.", error.what());
		return nullptr;
	}

	CListValue *result = new CListValue();
	result->SetReleaseOnDestruct(false);

	for (CValue *item : m_pValueArray) {
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

PyObject *CListValue::Pyfrom_id(PyObject *value)
{
	uintptr_t id = (uintptr_t)PyLong_AsVoidPtr(value);

	if (PyErr_Occurred()) {
		return nullptr;
	}

	int numelem = GetCount();
	for (int i = 0; i < numelem; i++) {
		if (reinterpret_cast<uintptr_t>(m_pValueArray[i]->m_proxy) == id) {
			return GetValue(i)->GetProxy();
		}
	}

	PyErr_SetString(PyExc_IndexError, "from_id(#): id not found in CValueList");
	return nullptr;
}

#endif  // WITH_PYTHON
