/** \file gameengine/Expressions/FloatValue.cpp
 *  \ingroup expressions
 */
// FloatValue.cpp: implementation of the CFloatValue class.
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
#include "EXP_StringValue.h"
#include "EXP_BoolValue.h"
#include "EXP_ErrorValue.h"

#include <boost/format.hpp>

CFloatValue::CFloatValue()
{
}

CFloatValue::CFloatValue(float fl)
	:m_float(fl)
{
}

CFloatValue::CFloatValue(float fl, const std::string& name)
	:m_float(fl)
{
	SetName(name);
}

CFloatValue::~CFloatValue()
{
}

CValue *CFloatValue::Calc(VALUE_OPERATOR op, CValue *val)
{
	switch (op) {
		case VALUE_POS_OPERATOR:
		{
			return new CFloatValue(m_float);
			break;
		}
		case VALUE_NEG_OPERATOR:
		{
			return new CFloatValue(-m_float);
			break;
		}
		case VALUE_NOT_OPERATOR:
		{
			return new CBoolValue(m_float == 0.0f);
			break;
		}
		case VALUE_AND_OPERATOR:
		case VALUE_OR_OPERATOR:
		{
			return new CErrorValue(val->GetText() + op2str(op) + "only allowed on booleans");
			break;
		}
		default:
		{
			return val->CalcFinal(VALUE_FLOAT_TYPE, op, this);
			break;
		}
	}
}

CValue *CFloatValue::CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, CValue *val)
{
	CValue *ret;

	switch (dtype) {
		case VALUE_INT_TYPE:
		{
			switch (op) {
				case VALUE_MOD_OPERATOR:
				{
					ret = new CFloatValue(fmod(((CIntValue *)val)->GetInt(), m_float));
					break;
				}
				case VALUE_ADD_OPERATOR:
				{
					ret = new CFloatValue(((CIntValue *)val)->GetInt() + m_float);
					break;
				}
				case VALUE_SUB_OPERATOR:
				{
					ret = new CFloatValue(((CIntValue *)val)->GetInt() - m_float);
					break;
				}
				case VALUE_MUL_OPERATOR:
				{
					ret = new CFloatValue(((CIntValue *)val)->GetInt() * m_float);
					break;
				}
				case VALUE_DIV_OPERATOR:
				{
					if (m_float == 0) {
						ret = new CErrorValue("Division by zero");
					}
					else {
						ret = new CFloatValue(((CIntValue *)val)->GetInt() / m_float);
					}
					break;
				}
				case VALUE_EQL_OPERATOR:
				{
					ret = new CBoolValue(((CIntValue *)val)->GetInt() == m_float);
					break;
				}
				case VALUE_NEQ_OPERATOR:
				{
					ret = new CBoolValue(((CIntValue *)val)->GetInt() != m_float);
					break;
				}
				case VALUE_GRE_OPERATOR:
				{
					ret = new CBoolValue(((CIntValue *)val)->GetInt() > m_float);
					break;
				}
				case VALUE_LES_OPERATOR:
				{
					ret = new CBoolValue(((CIntValue *)val)->GetInt() < m_float);
					break;
				}
				case VALUE_GEQ_OPERATOR:
				{
					ret = new CBoolValue(((CIntValue *)val)->GetInt() >= m_float);
					break;
				}
				case VALUE_LEQ_OPERATOR:
				{
					ret = new CBoolValue(((CIntValue *)val)->GetInt() <= m_float);
					break;
				}
				case VALUE_NOT_OPERATOR:
				{
					ret = new CBoolValue(m_float == 0);
					break;
				}
				default:
				{
					ret = new CErrorValue("illegal operator. please send a bug report.");
					break;
				}
			}
			break;
		}
		case VALUE_EMPTY_TYPE:
		case VALUE_FLOAT_TYPE:
		{
			switch (op) {
				case VALUE_MOD_OPERATOR:
				{
					ret = new CFloatValue(fmod(((CFloatValue *)val)->GetFloat(), m_float));
					break;
				}
				case VALUE_ADD_OPERATOR:
				{
					ret = new CFloatValue(((CFloatValue *)val)->GetFloat() + m_float);
					break;
				}
				case VALUE_SUB_OPERATOR:
				{
					ret = new CFloatValue(((CFloatValue *)val)->GetFloat() - m_float);
					break;
				}
				case VALUE_MUL_OPERATOR:
				{
					ret = new CFloatValue(((CFloatValue *)val)->GetFloat() * m_float);
					break;
				}
				case VALUE_DIV_OPERATOR:
				{
					if (m_float == 0) {
						ret = new CErrorValue("Division by zero");
					}
					else {
						ret = new CFloatValue(((CFloatValue *)val)->GetFloat() / m_float);
					}
					break;
				}
				case VALUE_EQL_OPERATOR:
				{
					ret = new CBoolValue(((CFloatValue *)val)->GetFloat() == m_float);
					break;
				}
				case VALUE_NEQ_OPERATOR:
				{
					ret = new CBoolValue(((CFloatValue *)val)->GetFloat() != m_float);
					break;
				}
				case VALUE_GRE_OPERATOR:
				{
					ret = new CBoolValue(((CFloatValue *)val)->GetFloat() > m_float);
					break;
				}
				case VALUE_LES_OPERATOR:
				{
					ret = new CBoolValue(((CFloatValue *)val)->GetFloat() < m_float);
					break;
				}
				case VALUE_GEQ_OPERATOR:
				{
					ret = new CBoolValue(((CFloatValue *)val)->GetFloat() >= m_float);
					break;
				}
				case VALUE_LEQ_OPERATOR:
				{
					ret = new CBoolValue(((CFloatValue *)val)->GetFloat() <= m_float);
					break;
				}
				case VALUE_NEG_OPERATOR:
				{
					ret = new CFloatValue(-m_float);
					break;
				}
				case VALUE_POS_OPERATOR:
				{
					ret = new CFloatValue(m_float);
					break;
				}
				case VALUE_NOT_OPERATOR:
				{
					ret = new CBoolValue(m_float == 0);
					break;
				}
				default:
				{
					ret = new CErrorValue("illegal operator. please send a bug report.");
					break;
				}
			}
			break;
		}
		case VALUE_STRING_TYPE:
		{
			switch (op) {
				case VALUE_ADD_OPERATOR:
				{
					ret = new CStringValue(val->GetText() + GetText(), "");
					break;
				}
				case VALUE_EQL_OPERATOR:
				case VALUE_NEQ_OPERATOR:
				case VALUE_GRE_OPERATOR:
				case VALUE_LES_OPERATOR:
				case VALUE_GEQ_OPERATOR:
				case VALUE_LEQ_OPERATOR:
				{
					ret = new CErrorValue("[Cannot compare string with float]" + op2str(op) + GetText());
					break;
				}
				default:
				{
					ret =  new CErrorValue("[operator not allowed on strings]" + op2str(op) + GetText());
					break;
				}
			}
			break;
		}
		case VALUE_BOOL_TYPE:
		{
			ret =  new CErrorValue("[operator not valid on boolean and float]" + op2str(op) + GetText());
			break;
		}
		case VALUE_ERROR_TYPE:
		{
			ret = new CErrorValue(val->GetText() + op2str(op) + GetText());
			break;
		}
		default:
		{
			ret = new CErrorValue("illegal type. contact your dealer (if any)");
			break;
		}
	}
	return ret;
}



void CFloatValue::SetFloat(float fl)
{
	m_float = fl;
}

float CFloatValue::GetFloat()
{
	return m_float;
}

double CFloatValue::GetNumber()
{
	return m_float;
}

int CFloatValue::GetValueType()
{
	return VALUE_FLOAT_TYPE;
}

void CFloatValue::SetValue(CValue *newval)
{
	m_float = (float)newval->GetNumber();
}

std::string CFloatValue::GetText()
{
	return std::to_string(m_float);
}

CValue *CFloatValue::GetReplica()
{
	CFloatValue *replica = new CFloatValue(*this);
	replica->ProcessReplica();

	return replica;
}

#ifdef WITH_PYTHON
PyObject *CFloatValue::ConvertValueToPython()
{
	return PyFloat_FromDouble(m_float);
}
#endif  // WITH_PYTHON
