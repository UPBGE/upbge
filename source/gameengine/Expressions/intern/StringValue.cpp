/** \file gameengine/Expressions/StringValue.cpp
 *  \ingroup expressions
 */
// StringValue.cpp: implementation of the EXP_StringValue class.
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

#include "EXP_StringValue.h"

#include "EXP_BoolValue.h"
#include "EXP_ErrorValue.h"

EXP_StringValue::EXP_StringValue() : m_strString("[Illegal String constructor call]")
{
}

EXP_StringValue::EXP_StringValue(const std::string &txt, const std::string &name)
    : m_strString(txt)
{
  SetName(name);
}

EXP_Value *EXP_StringValue::Calc(VALUE_OPERATOR op, EXP_Value *val)
{
  return val->CalcFinal(VALUE_STRING_TYPE, op, this);
}

EXP_Value *EXP_StringValue::CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, EXP_Value *val)
{
  EXP_Value *ret;

  if (op == VALUE_ADD_OPERATOR) {
    if (dtype == VALUE_ERROR_TYPE) {
      ret = new EXP_ErrorValue(val->GetText() + op2str(op) + GetText());
    }
    else {
      ret = new EXP_StringValue(val->GetText() + GetText(), "");
    }
  }
  else {
    if (dtype == VALUE_STRING_TYPE || dtype == VALUE_EMPTY_TYPE) {
      switch (op) {
        case VALUE_EQL_OPERATOR: {
          ret = new EXP_BoolValue(val->GetText() == GetText());
          break;
        }
        case VALUE_NEQ_OPERATOR: {
          ret = new EXP_BoolValue(val->GetText() != GetText());
          break;
        }
        case VALUE_GRE_OPERATOR: {
          ret = new EXP_BoolValue(val->GetText() > GetText());
          break;
        }
        case VALUE_LES_OPERATOR: {
          ret = new EXP_BoolValue(val->GetText() < GetText());
          break;
        }
        case VALUE_GEQ_OPERATOR: {
          ret = new EXP_BoolValue(val->GetText() >= GetText());
          break;
        }
        case VALUE_LEQ_OPERATOR: {
          ret = new EXP_BoolValue(val->GetText() <= GetText());
          break;
        }
        default: {
          ret = new EXP_ErrorValue(val->GetText() + op2str(op) +
                                   "[operator not allowed on strings]");
          break;
        }
      }
    }
    else {
      ret = new EXP_ErrorValue(val->GetText() + op2str(op) + "[operator not allowed on strings]");
    }
  }
  return ret;
}

void EXP_StringValue::SetValue(EXP_Value *newval)
{
  m_strString = newval->GetText();
}

double EXP_StringValue::GetNumber()
{
  return -1;
}

int EXP_StringValue::GetValueType()
{
  return VALUE_STRING_TYPE;
}

std::string EXP_StringValue::GetText()
{
  return m_strString;
}

bool EXP_StringValue::IsEqual(const std::string &other)
{
  return (m_strString == other);
}

EXP_Value *EXP_StringValue::GetReplica()
{
  EXP_StringValue *replica = new EXP_StringValue(*this);
  replica->ProcessReplica();
  return replica;
}
