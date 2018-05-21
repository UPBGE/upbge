/** \file gameengine/Expressions/BoolValue.cpp
 *  \ingroup expressions
 */

// BoolValue.cpp: implementation of the EXP_BoolValue class.
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

#include "EXP_BoolValue.h"

const std::string EXP_BoolValue::sTrueString  = "TRUE";
const std::string EXP_BoolValue::sFalseString = "FALSE";

EXP_BoolValue::EXP_BoolValue(bool inBool)
	:m_value(inBool)
{
}

int EXP_BoolValue::GetValueType() const
{
	return VALUE_BOOL_TYPE;
}

std::string EXP_BoolValue::GetText() const
{
	return m_value ? sTrueString : sFalseString;
}

bool EXP_BoolValue::Equal(EXP_Value *other) const
{
	if (other->GetValueType() != VALUE_BOOL_TYPE) {
		return false;
	}

	return (m_value == static_cast<EXP_BoolValue *>(other)->GetValue());
}

bool EXP_BoolValue::GetValue() const
{
	return m_value;
}

void EXP_BoolValue::SetValue(bool value)
{
	m_value = value;
}

EXP_Value *EXP_BoolValue::GetReplica()
{
	EXP_BoolValue *replica = new EXP_BoolValue(*this);
	replica->ProcessReplica();

	return replica;
}

#ifdef WITH_PYTHON
PyObject *EXP_BoolValue::ConvertValueToPython()
{
	return PyBool_FromLong(m_value != 0);
}
#endif  // WITH_PYTHON
