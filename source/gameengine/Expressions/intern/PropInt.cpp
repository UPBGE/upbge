/** \file gameengine/Expressions/IntValue.cpp
 *  \ingroup expressions
 */
// IntValue.cpp: implementation of the EXP_PropInt class.
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

#include "EXP_PropInt.h"

EXP_PropInt::EXP_PropInt(long long innie)
	:m_value(innie)
{
}

std::string EXP_PropInt::GetText() const
{
	return std::to_string(m_value);
}

EXP_PropValue::DataType EXP_PropInt::GetValueType() const
{
	return TYPE_INT;
}

EXP_PropValue *EXP_PropInt::GetReplica()
{
	EXP_PropInt *replica = new EXP_PropInt(*this);
	return replica;
}

long long EXP_PropInt::GetValue() const
{
	return m_value;
}

void EXP_PropInt::SetValue(long long value)
{
	m_value = value;
}

#ifdef WITH_PYTHON
PyObject *EXP_PropInt::ConvertValueToPython()
{
	return PyLong_FromLongLong(m_value);
}
#endif  // WITH_PYTHON
