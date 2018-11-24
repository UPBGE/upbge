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

void EXP_Value::DestructFromPython()
{
#ifdef WITH_PYTHON
	// Avoid decrefing freed proxy in destructor.
	m_proxy = nullptr;
	delete this;
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

bool EXP_Value::IsDictionary() const
{
	return false;
}
