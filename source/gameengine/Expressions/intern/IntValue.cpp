/** \file gameengine/Expressions/IntValue.cpp
 *  \ingroup expressions
 */
// IntValue.cpp: implementation of the EXP_IntValue class.
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

#include "EXP_IntValue.h"

EXP_IntValue::EXP_IntValue(long long innie)
	:m_value(innie)
{
}

std::string EXP_IntValue::GetText() const
{
	return std::to_string(m_value);
}

EXP_PropValue::DATA_TYPE EXP_IntValue::GetValueType() const
{
	return TYPE_INT;
}

EXP_PropValue *EXP_IntValue::GetReplica()
{
	EXP_IntValue *replica = new EXP_IntValue(*this);
	return replica;
}

long long EXP_IntValue::GetValue() const
{
	return m_value;
}

void EXP_IntValue::SetValue(long long value)
{
	m_value = value;
}

#ifdef WITH_PYTHON
PyObject *EXP_IntValue::ConvertValueToPython()
{
	return PyLong_FromLongLong(m_value);
}
#endif  // WITH_PYTHON
