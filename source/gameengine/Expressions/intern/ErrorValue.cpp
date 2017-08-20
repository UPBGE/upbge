/** \file gameengine/Expressions/ErrorValue.cpp
 *  \ingroup expressions
 */
// ErrorValue.cpp: implementation of the CErrorValue class.
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

#include "EXP_ErrorValue.h"

CErrorValue::CErrorValue()
	:m_strErrorText("Error")
{
	SetError(true);
}

CErrorValue::CErrorValue(const std::string& errmsg)
{
	m_strErrorText = "[";
	m_strErrorText += errmsg;
	m_strErrorText += "]";
	SetError(true);
}

CErrorValue::~CErrorValue()
{
}

CValue *CErrorValue::Calc(VALUE_OPERATOR op, CValue *val)
{
	CValue *errorval;

	switch (op) {
		case VALUE_POS_OPERATOR:
		case VALUE_NEG_OPERATOR:
		case VALUE_NOT_OPERATOR:
		{
			errorval = new CErrorValue(op2str(op) + GetText());
			break;
		}
		default:
		{
			errorval = val->CalcFinal(VALUE_ERROR_TYPE, op, this);
			break;
		}
	}

	return errorval;
}

CValue *CErrorValue::CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, CValue *val)
{
	return new CErrorValue(val->GetText() + op2str(op) + GetText());
}

int CErrorValue::GetValueType()
{
	return VALUE_ERROR_TYPE;
}

std::string CErrorValue::GetText()
{
	return m_strErrorText;
}

CValue *CErrorValue::GetReplica()
{
	// Who would want a copy of an error ?
	BLI_assert(false && "ErrorValue::GetReplica() not implemented yet");

	return nullptr;
}
