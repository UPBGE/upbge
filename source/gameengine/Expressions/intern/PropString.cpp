/** \file gameengine/Expressions/StringValue.cpp
 *  \ingroup expressions
 */
// StringValue.cpp: implementation of the EXP_PropString class.
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

#include "EXP_PropString.h"

EXP_PropString::EXP_PropString(const std::string& txt)
	:m_value(txt)
{
}

std::string EXP_PropString::GetText() const
{
	return m_value;
}

EXP_PropValue::DATA_TYPE EXP_PropString::GetValueType() const
{
	return TYPE_STRING;
}

const std::string &EXP_PropString::GetValue() const
{
	return m_value;
}

void EXP_PropString::SetValue(const std::string& value)
{
	m_value = value;
}

EXP_PropValue *EXP_PropString::GetReplica()
{
	EXP_PropString *replica = new EXP_PropString(*this);
	return replica;
}

PyObject * EXP_PropString::ConvertValueToPython()
{
	return PyUnicode_FromStdString(m_value);
}
