/** \file gameengine/Expressions/EmptyValue.cpp
 *  \ingroup expressions
 */

// EmptyValue.cpp: implementation of the CEmptyValue class.
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

#include "EXP_EmptyValue.h"

CEmptyValue::CEmptyValue()
{
}

CEmptyValue::~CEmptyValue()
{
}

CValue *CEmptyValue::Calc(VALUE_OPERATOR op, CValue *val)
{
	return val->CalcFinal(VALUE_EMPTY_TYPE, op, this);
}

CValue *CEmptyValue::CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, CValue *val)
{
	return val->AddRef();
}

double CEmptyValue::GetNumber()
{
	return 0.0;
}

int CEmptyValue::GetValueType()
{
	return VALUE_EMPTY_TYPE;
}

std::string CEmptyValue::GetText()
{
	return "";
}

CValue *CEmptyValue::GetReplica()
{
	CEmptyValue *replica = new CEmptyValue(*this);
	replica->ProcessReplica();
	return replica;
}

