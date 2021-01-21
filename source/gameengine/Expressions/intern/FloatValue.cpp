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

#include "EXP_BoolValue.h"
#include "EXP_ErrorValue.h"
#include "EXP_IntValue.h"
#include "EXP_StringValue.h"

EXP_FloatValue::EXP_FloatValue()
{
}

EXP_FloatValue::EXP_FloatValue(float fl) : m_float(fl)
{
}

EXP_FloatValue::EXP_FloatValue(float fl, const std::string &name) : m_float(fl)
{
  SetName(name);
}

EXP_FloatValue::~EXP_FloatValue()
{
}

EXP_Value *EXP_FloatValue::Calc(VALUE_OPERATOR op, EXP_Value *val)
{
  switch (op) {
    case VALUE_POS_OPERATOR: {
      return new EXP_FloatValue(m_float);
      break;
    }
    case VALUE_NEG_OPERATOR: {
      return new EXP_FloatValue(-m_float);
      break;
    }
    case VALUE_NOT_OPERATOR: {
      return new EXP_BoolValue(m_float == 0.0f);
      break;
    }
    case VALUE_AND_OPERATOR:
    case VALUE_OR_OPERATOR: {
      return new EXP_ErrorValue(val->GetText() + op2str(op) + "only allowed on booleans");
      break;
    }
    default: {
      return val->CalcFinal(VALUE_FLOAT_TYPE, op, this);
      break;
    }
  }
}

EXP_Value *EXP_FloatValue::CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, EXP_Value *val)
{
  EXP_Value *ret;

  switch (dtype) {
    case VALUE_INT_TYPE: {
      switch (op) {
        case VALUE_MOD_OPERATOR: {
          ret = new EXP_FloatValue(fmod(((EXP_IntValue *)val)->GetInt(), m_float));
          break;
        }
        case VALUE_ADD_OPERATOR: {
          ret = new EXP_FloatValue(((EXP_IntValue *)val)->GetInt() + m_float);
          break;
        }
        case VALUE_SUB_OPERATOR: {
          ret = new EXP_FloatValue(((EXP_IntValue *)val)->GetInt() - m_float);
          break;
        }
        case VALUE_MUL_OPERATOR: {
          ret = new EXP_FloatValue(((EXP_IntValue *)val)->GetInt() * m_float);
          break;
        }
        case VALUE_DIV_OPERATOR: {
          if (m_float == 0) {
            ret = new EXP_ErrorValue("Division by zero");
          }
          else {
            ret = new EXP_FloatValue(((EXP_IntValue *)val)->GetInt() / m_float);
          }
          break;
        }
        case VALUE_EQL_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_IntValue *)val)->GetInt() == m_float);
          break;
        }
        case VALUE_NEQ_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_IntValue *)val)->GetInt() != m_float);
          break;
        }
        case VALUE_GRE_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_IntValue *)val)->GetInt() > m_float);
          break;
        }
        case VALUE_LES_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_IntValue *)val)->GetInt() < m_float);
          break;
        }
        case VALUE_GEQ_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_IntValue *)val)->GetInt() >= m_float);
          break;
        }
        case VALUE_LEQ_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_IntValue *)val)->GetInt() <= m_float);
          break;
        }
        case VALUE_NOT_OPERATOR: {
          ret = new EXP_BoolValue(m_float == 0);
          break;
        }
        default: {
          ret = new EXP_ErrorValue("illegal operator. please send a bug report.");
          break;
        }
      }
      break;
    }
    case VALUE_EMPTY_TYPE:
    case VALUE_FLOAT_TYPE: {
      switch (op) {
        case VALUE_MOD_OPERATOR: {
          ret = new EXP_FloatValue(fmod(((EXP_FloatValue *)val)->GetFloat(), m_float));
          break;
        }
        case VALUE_ADD_OPERATOR: {
          ret = new EXP_FloatValue(((EXP_FloatValue *)val)->GetFloat() + m_float);
          break;
        }
        case VALUE_SUB_OPERATOR: {
          ret = new EXP_FloatValue(((EXP_FloatValue *)val)->GetFloat() - m_float);
          break;
        }
        case VALUE_MUL_OPERATOR: {
          ret = new EXP_FloatValue(((EXP_FloatValue *)val)->GetFloat() * m_float);
          break;
        }
        case VALUE_DIV_OPERATOR: {
          if (m_float == 0) {
            ret = new EXP_ErrorValue("Division by zero");
          }
          else {
            ret = new EXP_FloatValue(((EXP_FloatValue *)val)->GetFloat() / m_float);
          }
          break;
        }
        case VALUE_EQL_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_FloatValue *)val)->GetFloat() == m_float);
          break;
        }
        case VALUE_NEQ_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_FloatValue *)val)->GetFloat() != m_float);
          break;
        }
        case VALUE_GRE_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_FloatValue *)val)->GetFloat() > m_float);
          break;
        }
        case VALUE_LES_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_FloatValue *)val)->GetFloat() < m_float);
          break;
        }
        case VALUE_GEQ_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_FloatValue *)val)->GetFloat() >= m_float);
          break;
        }
        case VALUE_LEQ_OPERATOR: {
          ret = new EXP_BoolValue(((EXP_FloatValue *)val)->GetFloat() <= m_float);
          break;
        }
        case VALUE_NEG_OPERATOR: {
          ret = new EXP_FloatValue(-m_float);
          break;
        }
        case VALUE_POS_OPERATOR: {
          ret = new EXP_FloatValue(m_float);
          break;
        }
        case VALUE_NOT_OPERATOR: {
          ret = new EXP_BoolValue(m_float == 0);
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
          ret = new EXP_ErrorValue("[Cannot compare string with float]" + op2str(op) + GetText());
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
      ret = new EXP_ErrorValue("[operator not valid on boolean and float]" + op2str(op) +
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

void EXP_FloatValue::SetFloat(float fl)
{
  m_float = fl;
}

float EXP_FloatValue::GetFloat()
{
  return m_float;
}

double EXP_FloatValue::GetNumber()
{
  return m_float;
}

int EXP_FloatValue::GetValueType()
{
  return VALUE_FLOAT_TYPE;
}

void EXP_FloatValue::SetValue(EXP_Value *newval)
{
  m_float = (float)newval->GetNumber();
}

std::string EXP_FloatValue::GetText()
{
  return std::to_string(m_float);
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
  return PyFloat_FromDouble(m_float);
}
#endif  // WITH_PYTHON
