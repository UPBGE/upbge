/** \file gameengine/Expressions/Value.cpp
 *  \ingroup expressions
 */
// Value.cpp: implementation of the EXP_Value class.
// developed at Eindhoven University of Technology, 1997
// by the OOPS team
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
#include "EXP_Value.h"
#include "EXP_BoolValue.h"
#include "EXP_StringValue.h"
#include "EXP_IntValue.h"
#include "EXP_FloatValue.h"
#include "EXP_PythonValue.h"

EXP_PropValue *EXP_PropValue::ConvertPythonToValue(PyObject *pyobj)
{
	// Note: Boolean check should go before Int check [#34677].
	if (PyBool_Check(pyobj)) {
		return new EXP_BoolValue((bool)PyLong_AsLongLong(pyobj));
	}
	else if (PyFloat_Check(pyobj)) {
		const double tval = PyFloat_AsDouble(pyobj);
		return new EXP_FloatValue(tval);
	}
	else if (PyLong_Check(pyobj)) {
		return new EXP_IntValue(PyLong_AsLongLong(pyobj));
	}
	else if (PyUnicode_Check(pyobj)) {
		return new EXP_StringValue(_PyUnicode_AsString(pyobj));
	}

	// Fall down in python property.
	return new EXP_PythonValue(pyobj);
}

EXP_Value::EXP_Value()
{
}

EXP_Value::EXP_Value(const EXP_Value& other)
	:EXP_PyObjectPlus(other),
	CM_RefCount<EXP_Value>(other)
{
	for (const auto& pair : other.m_properties) {
		m_properties.emplace(pair.first, pair.second->GetReplica());
	}
}

EXP_Value::~EXP_Value()
{
	ClearProperties();
}

void EXP_Value::SetProperty(const std::string& name, EXP_PropValue *ioProperty)
{
	// Try to replace property.
	auto it = m_properties.find(name);
	if (it != m_properties.end()) {
		it->second.reset(ioProperty);
	}
	// Add property into the array.
	else {
		m_properties.emplace(name, ioProperty);
	}
}

EXP_PropValue *EXP_Value::GetProperty(const std::string& inName) const
{
	const auto it = m_properties.find(inName);
	if (it != m_properties.end()) {
		return it->second.get();
	}
	return nullptr;
}

bool EXP_Value::RemoveProperty(const std::string& inName)
{
	auto it = m_properties.find(inName);
	if (it != m_properties.end()) {
		m_properties.erase(it);
		return true;
	}

	return false;
}

std::vector<std::string> EXP_Value::GetPropertyNames() const
{
	const unsigned short size = m_properties.size();
	std::vector<std::string> result(size);

	unsigned short i = 0;
	for (const auto& pair : m_properties) {
		result[i++] = pair.first;
	}
	return result;
}

void EXP_Value::ClearProperties()
{
	m_properties.clear();
}

EXP_PropValue *EXP_Value::GetProperty(int inIndex) const
{
	int count = 0;
	for (const auto& pair : m_properties) {
		if (count++ == inIndex) {
			return pair.second.get();
		}
	}
	return nullptr;
}

int EXP_Value::GetPropertyCount()
{
	return m_properties.size();
}

void EXP_Value::DestructFromPython()
{
#ifdef WITH_PYTHON
	// Avoid decrefing freed proxy in destructor.
	m_proxy = nullptr;
	Release();
#endif  // WITH_PYTHON
}

void EXP_Value::ProcessReplica()
{
	EXP_PyObjectPlus::ProcessReplica();
}

#ifdef WITH_PYTHON

PyTypeObject EXP_Value::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"EXP_Value",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0,
	0, 0, 0, 0, 0,
	nullptr,
	nullptr,
	0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&EXP_PyObjectPlus::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef EXP_Value::Methods[] = {
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef EXP_Value::Attributes[] = {
	EXP_PYATTRIBUTE_RO_FUNCTION("name",  EXP_Value, pyattr_get_name),
	EXP_PYATTRIBUTE_NULL // Sentinel
};

PyObject *EXP_Value::pyattr_get_name(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	EXP_Value *self = static_cast<EXP_Value *> (self_v);
	return PyUnicode_FromStdString(self->GetName());
}

PyObject *EXP_Value::ConvertKeysToPython()
{
	PyObject *pylist = PyList_New(m_properties.size());

	Py_ssize_t i = 0;
	for (const auto& pair : m_properties) {
		PyList_SET_ITEM(pylist, i++, PyUnicode_FromStdString(pair.first));
	}

	return pylist;
}

#endif  // WITH_PYTHON

std::string EXP_Value::GetText() const
{
	return GetName();
}

void EXP_Value::SetName(const std::string& name)
{
}

EXP_Value *EXP_Value::GetReplica()
{
	return nullptr;
}
