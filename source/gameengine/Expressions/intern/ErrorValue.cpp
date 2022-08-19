/** \file gameengine/Expressions/ErrorValue.cpp
 *  \ingroup expressions
 */
// ErrorValue.cpp: implementation of the EXP_ErrorValue class.
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

EXP_ErrorValue::EXP_ErrorValue() : m_strErrorText("Error")
{
}

EXP_ErrorValue::EXP_ErrorValue(const std::string &errmsg) : m_strErrorText("[" + errmsg + "]")
{
}

EXP_ErrorValue::~EXP_ErrorValue()
{
}

EXP_Value *EXP_ErrorValue::Calc(VALUE_OPERATOR op, EXP_Value *val)
{
  EXP_Value *errorval;

  switch (op) {
    case VALUE_POS_OPERATOR:
    case VALUE_NEG_OPERATOR:
    case VALUE_NOT_OPERATOR: {
      errorval = new EXP_ErrorValue(op2str(op) + GetText());
      break;
    }
    default: {
      errorval = val->CalcFinal(VALUE_ERROR_TYPE, op, this);
      break;
    }
  }

  return errorval;
}

EXP_Value *EXP_ErrorValue::CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, EXP_Value *val)
{
  return new EXP_ErrorValue(val->GetText() + op2str(op) + GetText());
}

int EXP_ErrorValue::GetValueType()
{
  return VALUE_ERROR_TYPE;
}

std::string EXP_ErrorValue::GetText()
{
  return m_strErrorText;
}

EXP_Value *EXP_ErrorValue::GetReplica()
{
  // Who would want a copy of an error ?
  BLI_assert(false && "ErrorValue::GetReplica() not implemented yet");

  return nullptr;
}

bool EXP_ErrorValue::IsError() const
{
  return true;
}
