/** \file gameengine/Expressions/FloatValue.cpp
 *  \ingroup expressions
 */
// FloatValue.cpp: implementation of the EXP_FloatValue class.
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

#include "EXP_FloatValue.h"
#include "EXP_IntValue.h"

EXP_FloatValue::EXP_FloatValue(double fl)
	:m_value(fl)
{
}

std::string EXP_FloatValue::GetText() const
{
	return std::to_string(m_value);
}

int EXP_FloatValue::GetValueType() const
{
	return VALUE_FLOAT_TYPE;
}

bool EXP_FloatValue::Equal(EXP_Value *other) const
{
	switch (other->GetValueType()) {
		case VALUE_INT_TYPE:
		{
			return (m_value == ((double)static_cast<EXP_IntValue *>(other)->GetValue()));
		}
		case VALUE_FLOAT_TYPE:
		{
			return (m_value == static_cast<EXP_FloatValue *>(other)->GetValue());
		}
		default:
		{
			return false;
		}
	}
}

double EXP_FloatValue::GetValue() const
{
	return m_value;
}

void EXP_FloatValue::SetValue(double value)
{
	m_value = value;
}

EXP_Value *EXP_FloatValue::GetReplica()
{
	EXP_FloatValue *replica = new EXP_FloatValue(*this);
	replica->ProcessReplica();

	return replica;
}

#ifdef WITH_PYTHON
PyObject *EXP_FloatValue::ConvertValueToPython()
{
	return PyFloat_FromDouble(m_value);
}
#endif  // WITH_PYTHON
