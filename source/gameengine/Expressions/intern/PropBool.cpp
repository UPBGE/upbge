/** \file gameengine/Expressions/BoolValue.cpp
 *  \ingroup expressions
 */

// BoolValue.cpp: implementation of the EXP_PropBool class.
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

#include "EXP_PropBool.h"

EXP_PropBool::EXP_PropBool(bool inBool)
	:m_value(inBool)
{
}

std::string EXP_PropBool::GetText() const
{
	return m_value ? "TRUE" : "FALSE";
}

EXP_PropValue::DataType EXP_PropBool::GetValueType() const
{
	return TYPE_BOOL;
}

bool EXP_PropBool::GetValue() const
{
	return m_value;
}

void EXP_PropBool::SetValue(bool value)
{
	m_value = value;
}

EXP_PropValue *EXP_PropBool::GetReplica()
{
	EXP_PropBool *replica = new EXP_PropBool(*this);
	return replica;
}

#ifdef WITH_PYTHON
PyObject *EXP_PropBool::ConvertValueToPython()
{
	return PyBool_FromLong(m_value != 0);
}
#endif  // WITH_PYTHON
