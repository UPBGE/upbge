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

#include <fmt/format.h>

#include "CM_Message.h"
#include "EXP_BoolValue.h"
#include "EXP_ErrorValue.h"
#include "EXP_FloatValue.h"
#include "EXP_StringValue.h"

EXP_IntValue::EXP_IntValue()
{
}

EXP_IntValue::EXP_IntValue(cInt innie) : m_int(innie)
{
}

EXP_IntValue::EXP_IntValue(cInt innie, const std::string &name) : m_int(innie)
{
  SetName(name);
}

EXP_IntValue::~EXP_IntValue()
{
}

EXP_Value *EXP_IntValue::Calc(VALUE_OPERATOR op, EXP_Value *val)
{
  switch (op) {
    case VALUE_POS_OPERATOR: {
      return new EXP_IntValue(m_int);
      break;
    }
    case VALUE_NEG_OPERATOR: {
      return new EXP_IntValue(-m_int);
      break;
    }
    case VALUE_NOT_OPERATOR: {
      return new EXP_BoolValue(m_int == 0);
      break;
    }
    case VALUE_AND_OPERATOR:
    case VALUE_OR_OPERATOR: {
      return new EXP_ErrorValue(val->GetText() + op2str(op) + "only allowed on booleans");
      break;
    }
    default: {
      return val->CalcFinal(VALUE_INT_TYPE, op, this);
      break;
    }
  }
}

EXP_Value *EXP_IntValue::CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, EXP_Value *val)
{
  EXP_Value *ret;

  switch (dtype) {
    case VALUE_EMPTY_TYPE:
    case VALUE_INT_TYPE: {
      switch (op) {
        case VALUE_MOD_OPERATOR: {
          ret = new EXP_IntValue(((EXP_IntValue *)val)->GetInt() % m_int);
          break;
        }
        case VALUE_ADD_OPERATOR: {
          ret = new EXP_IntValue(((EXP_IntValue *)val)->GetInt() + m_int);
          break;
        }
        case VALUE_SUB_OPERATOR: {
          ret = new EXP_IntValue(((EXP_IntValue *)val)->GetInt() - m_int);
          break;
        }
        case VALUE_MUL_OPERATOR: {
          ret = new EXP_IntValue(((EXP_IntValue *)val)->GetInt() * m_int);
          break;
        }
        case VALUE_DIV_OPERATOR: {
          if (m_int == 0) {
            if (val->GetNumber() == 0) {
              ret = new EXP_ErrorValue("Not a Number");
            }
            else {
              ret = new EXP_ErrorValue("Division by zero");
            }
          }
          else {
            ret = new EXP_IntValue(((EXP_IntValue *)val)->GetInt() / m_int);
          }
          break;
        }
        case VALUE_EQL_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_IntValue *)val)->GetInt() == m_int);
          break;
        }
        case VALUE_NEQ_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_IntValue *)val)->GetInt() != m_int);
          break;
        }
        case VALUE_GRE_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_IntValue *)val)->GetInt() > m_int);
          break;
        }
        case VALUE_LES_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_IntValue *)val)->GetInt() < m_int);
          break;
        }
        case VALUE_GEQ_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_IntValue *)val)->GetInt() >= m_int);
          break;
        }
        case VALUE_LEQ_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_IntValue *)val)->GetInt() <= m_int);
          break;
        }
        case VALUE_NEG_OPERATOR: {
          ret = new EXP_IntValue(-m_int);
          break;
        }
        case VALUE_POS_OPERATOR: {
          ret = new EXP_IntValue(m_int);
          break;
        }
        case VALUE_NOT_OPERATOR: {
          ret = new EXP_BoolValue(m_int == 0);
          break;
        }
        default: {
          CM_Error("found op: " << op);
          ret = new EXP_ErrorValue("illegal operator. please send a bug report.");
          break;
        }
      }
      break;
    }
    case VALUE_FLOAT_TYPE: {
      switch (op) {
        case VALUE_MOD_OPERATOR: {
          ret = new EXP_FloatValue(fmod(((EXP_FloatValue *)val)->GetFloat(), m_int));
          break;
        }
        case VALUE_ADD_OPERATOR: {
          ret = new EXP_FloatValue(((EXP_FloatValue *)val)->GetFloat() + m_int);
          break;
        }
        case VALUE_SUB_OPERATOR: {
          ret = new EXP_FloatValue(((EXP_FloatValue *)val)->GetFloat() - m_int);
          break;
        }
        case VALUE_MUL_OPERATOR: {
          ret = new EXP_FloatValue(((EXP_FloatValue *)val)->GetFloat() * m_int);
          break;
        }
        case VALUE_DIV_OPERATOR: {
          if (m_int == 0) {
            ret = new EXP_ErrorValue("Division by zero");
          }
          else {
            ret = new EXP_FloatValue(((EXP_FloatValue *)val)->GetFloat() / m_int);
          }
          break;
        }
        case VALUE_EQL_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_FloatValue *)val)->GetFloat() == m_int);
          break;
        }
        case VALUE_NEQ_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_FloatValue *)val)->GetFloat() != m_int);
          break;
        }
        case VALUE_GRE_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_FloatValue *)val)->GetFloat() > m_int);
          break;
        }
        case VALUE_LES_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_FloatValue *)val)->GetFloat() < m_int);
          break;
        }
        case VALUE_GEQ_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_FloatValue *)val)->GetFloat() >= m_int);
          break;
        }
        case VALUE_LEQ_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_FloatValue *)val)->GetFloat() <= m_int);
          break;
        }
        case VALUE_NOT_OPERATOR: {
          ret = new EXP_BoolValue(m_int == 0);
          break;
        }
        default: {
          ret = new EXP_ErrorValue("illegal operator. please send a bug report.");
          break;
        }
      }
      break;
    }
    case VALUE_STRING_TYPE: {
      switch (op) {
        case VALUE_ADD_OPERATOR: {
          ret = new EXP_StringValue(val->GetText() + GetText(), "");
          break;
        }
        case VALUE_EQL_OPERATOR:
        case VALUE_NEQ_OPERATOR:
        case VALUE_GRE_OPERATOR:
        case VALUE_LES_OPERATOR:
        case VALUE_GEQ_OPERATOR:
        case VALUE_LEQ_OPERATOR: {
          ret = new EXP_ErrorValue("[Cannot compare string with integer]" + op2str(op) +
                                   GetText());
          break;
        }
        default: {
          ret = new EXP_ErrorValue("[operator not allowed on strings]" + op2str(op) + GetText());
          break;
        }
      }
      break;
    }
    case VALUE_BOOL_TYPE: {
      ret = new EXP_ErrorValue("[operator not valid on boolean and integer]" + op2str(op) +
                               GetText());
      break;
    }
    case VALUE_ERROR_TYPE: {
      ret = new EXP_ErrorValue(val->GetText() + op2str(op) + GetText());
      break;
    }
    default: {
      ret = new EXP_ErrorValue("illegal type. contact your dealer (if any)");
      break;
    }
  }
  return ret;
}

cInt EXP_IntValue::GetInt()
{
  return m_int;
}

double EXP_IntValue::GetNumber()
{
  return (double)m_int;
}

int EXP_IntValue::GetValueType()
{
  return VALUE_INT_TYPE;
}

std::string EXP_IntValue::GetText()
{
  return fmt::format("{}", m_int);
}

EXP_Value *EXP_IntValue::GetReplica()
{
  EXP_IntValue *replica = new EXP_IntValue(*this);
  replica->ProcessReplica();

  return replica;
}

void EXP_IntValue::SetValue(EXP_Value *newval)
{
  m_int = (cInt)newval->GetNumber();
}

#ifdef WITH_PYTHON
PyObject *EXP_IntValue::ConvertValueToPython()
{
  return PyLong_FromLongLong(m_int);
}
#endif  // WITH_PYTHON
