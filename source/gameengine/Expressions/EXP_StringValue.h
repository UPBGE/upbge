/*
 * StringValue.h: interface for the CStringValue class.
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

/** \file EXP_StringValue.h
 *  \ingroup expressions
 */

#ifndef __EXP_STRINGVALUE_H__
#define __EXP_STRINGVALUE_H__

#include "EXP_Value.h"

class CStringValue : public CPropValue
{
public:
	CStringValue();
	CStringValue(const std::string& txt, const std::string& name);
	virtual ~CStringValue()
	{
	}

	/// CValue implementation
	virtual bool IsEqual(const std::string & other);
	virtual std::string GetText();
	virtual double GetNumber();
	virtual int GetValueType();

	virtual CValue *Calc(VALUE_OPERATOR op, CValue *val);
	virtual CValue *CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, CValue *val);
	virtual void SetValue(CValue *newval);
	virtual CValue *GetReplica();

#ifdef WITH_PYTHON
	virtual PyObject *ConvertValueToPython()
	{
		return PyUnicode_FromStdString(m_strString);
	}
#endif  // WITH_PYTHON

private:
	/// Data member.
	std::string m_strString;
};

#endif  // __EXP_STRINGVALUE_H__
