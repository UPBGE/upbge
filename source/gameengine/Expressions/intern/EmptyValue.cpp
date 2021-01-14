/** \file gameengine/Expressions/EmptyValue.cpp
 *  \ingroup expressions
 */

// EmptyValue.cpp: implementation of the EXP_EmptyValue class.
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

EXP_EmptyValue::EXP_EmptyValue()
{
}

EXP_EmptyValue::~EXP_EmptyValue()
{
}

EXP_Value *EXP_EmptyValue::Calc(VALUE_OPERATOR op, EXP_Value *val)
{
  return val->CalcFinal(VALUE_EMPTY_TYPE, op, this);
}

EXP_Value *EXP_EmptyValue::CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, EXP_Value *val)
{
  return val->AddRef();
}

double EXP_EmptyValue::GetNumber()
{
  return 0.0;
}

int EXP_EmptyValue::GetValueType()
{
  return VALUE_EMPTY_TYPE;
}

std::string EXP_EmptyValue::GetText()
{
  return "";
}

EXP_Value *EXP_EmptyValue::GetReplica()
{
  EXP_EmptyValue *replica = new EXP_EmptyValue(*this);
  replica->ProcessReplica();
  return replica;
}
