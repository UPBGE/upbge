/** \file gameengine/Expressions/IntValue.cpp
 *  \ingroup expressions
 */
// IntValue.cpp: implementation of the CIntValue class.
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
#include "EXP_ErrorValue.h"
#include "EXP_FloatValue.h"
#include "EXP_BoolValue.h"
#include "EXP_StringValue.h"

#include "CM_Message.h"

#include <boost/format.hpp>

CIntValue::CIntValue()
{
}

CIntValue::CIntValue(cInt innie)
	:m_int(innie)
{
}

CIntValue::CIntValue(cInt innie, const std::string& name)
	:m_int(innie)
{
	SetName(name);
}

CIntValue::~CIntValue()
{
}

CValue *CIntValue::Calc(VALUE_OPERATOR op, CValue *val)
{
	switch (op) {
		case VALUE_POS_OPERATOR:
		{
			return new CIntValue(m_int);
			break;
		}
		case VALUE_NEG_OPERATOR:
		{
			return new CIntValue(-m_int);
			break;
		}
		case VALUE_NOT_OPERATOR:
		{
			return new CBoolValue(m_int == 0);
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
			return val->CalcFinal(VALUE_INT_TYPE, op, this);
			break;
		}
	}
}

CValue *CIntValue::CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, CValue *val)
{
	CValue *ret;

	switch (dtype) {
		case VALUE_EMPTY_TYPE:
		case VALUE_INT_TYPE:
		{
			switch (op) {
				case VALUE_MOD_OPERATOR:
				{
					ret = new CIntValue(((CIntValue *)val)->GetInt() % m_int);
					break;
				}
				case VALUE_ADD_OPERATOR:
				{
					ret = new CIntValue(((CIntValue *)val)->GetInt() + m_int);
					break;
				}
				case VALUE_SUB_OPERATOR:
				{
					ret = new CIntValue(((CIntValue *)val)->GetInt() - m_int);
					break;
				}
				case VALUE_MUL_OPERATOR:
				{
					ret = new CIntValue(((CIntValue *)val)->GetInt() * m_int);
					break;
				}
				case VALUE_DIV_OPERATOR:
				{
					if (m_int == 0) {
						if (val->GetNumber() == 0) {
							ret = new CErrorValue("Not a Number");
						}
						else {
							ret = new CErrorValue("Division by zero");
						}
					}
					else {
						ret = new CIntValue(((CIntValue *)val)->GetInt() / m_int);
					}
					break;
				}
				case VALUE_EQL_OPERATOR:
				{
					ret = new CBoolValue(((CIntValue *)val)->GetInt() == m_int);
					break;
				}
				case VALUE_NEQ_OPERATOR:
				{
					ret = new CBoolValue(((CIntValue *)val)->GetInt() != m_int);
					break;
				}
				case VALUE_GRE_OPERATOR:
				{
					ret = new CBoolValue(((CIntValue *)val)->GetInt() > m_int);
					break;
				}
				case VALUE_LES_OPERATOR:
				{
					ret = new CBoolValue(((CIntValue *)val)->GetInt() < m_int);
					break;
				}
				case VALUE_GEQ_OPERATOR:
				{
					ret = new CBoolValue(((CIntValue *)val)->GetInt() >= m_int);
					break;
				}
				case VALUE_LEQ_OPERATOR:
				{
					ret = new CBoolValue(((CIntValue *)val)->GetInt() <= m_int);
					break;
				}
				case VALUE_NEG_OPERATOR:
				{
					ret = new CIntValue(-m_int);
					break;
				}
				case VALUE_POS_OPERATOR:
				{
					ret = new CIntValue(m_int);
					break;
				}
				case VALUE_NOT_OPERATOR:
				{
					ret = new CBoolValue(m_int == 0);
					break;
				}
				default:
				{
					CM_Error("found op: " << op);
					ret = new CErrorValue("illegal operator. please send a bug report.");
					break;
				}
			}
			break;
		}
		case VALUE_FLOAT_TYPE:
		{
			switch (op) {
				case VALUE_MOD_OPERATOR:
				{
					ret = new CFloatValue(fmod(((CFloatValue *)val)->GetFloat(), m_int));
					break;
				}
				case VALUE_ADD_OPERATOR:
				{
					ret = new CFloatValue(((CFloatValue *)val)->GetFloat() + m_int);
					break;
				}
				case VALUE_SUB_OPERATOR:
				{
					ret = new CFloatValue(((CFloatValue *)val)->GetFloat() - m_int);
					break;
				}
				case VALUE_MUL_OPERATOR:
				{
					ret = new CFloatValue(((CFloatValue *)val)->GetFloat() * m_int);
					break;
				}
				case VALUE_DIV_OPERATOR:
				{
					if (m_int == 0) {
						ret = new CErrorValue("Division by zero");
					}
					else {
						ret = new CFloatValue(((CFloatValue *)val)->GetFloat() / m_int);
					}
					break;
				}
				case VALUE_EQL_OPERATOR:
				{
					ret = new CBoolValue(((CFloatValue *)val)->GetFloat() == m_int);
					break;
				}
				case VALUE_NEQ_OPERATOR:
				{
					ret = new CBoolValue(((CFloatValue *)val)->GetFloat() != m_int);
					break;
				}
				case VALUE_GRE_OPERATOR:
				{
					ret = new CBoolValue(((CFloatValue *)val)->GetFloat() > m_int);
					break;
				}
				case VALUE_LES_OPERATOR:
				{
					ret = new CBoolValue(((CFloatValue *)val)->GetFloat() < m_int);
					break;
				}
				case VALUE_GEQ_OPERATOR:
				{
					ret = new CBoolValue(((CFloatValue *)val)->GetFloat() >= m_int);
					break;
				}
				case VALUE_LEQ_OPERATOR:
				{
					ret = new CBoolValue(((CFloatValue *)val)->GetFloat() <= m_int);
					break;
				}
				case VALUE_NOT_OPERATOR:
				{
					ret = new CBoolValue(m_int == 0);
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
					ret = new CErrorValue("[Cannot compare string with integer]" + op2str(op) + GetText());
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
			ret =  new CErrorValue("[operator not valid on boolean and integer]" + op2str(op) + GetText());
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

cInt CIntValue::GetInt()
{
	return m_int;
}

double CIntValue::GetNumber()
{
	return (double)m_int;
}

int CIntValue::GetValueType()
{
	return VALUE_INT_TYPE;
}

std::string CIntValue::GetText()
{
	return (boost::format("%lld") % m_int).str();
}

CValue *CIntValue::GetReplica()
{
	CIntValue *replica = new CIntValue(*this);
	replica->ProcessReplica();

	return replica;
}

void CIntValue::SetValue(CValue *newval)
{
	m_int = (cInt)newval->GetNumber();
}

#ifdef WITH_PYTHON
PyObject *CIntValue::ConvertValueToPython()
{
	return PyLong_FromLongLong(m_int);
}
#endif  // WITH_PYTHON
