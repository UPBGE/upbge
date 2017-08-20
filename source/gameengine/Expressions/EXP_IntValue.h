/*
 * IntValue.h: interface for the CIntValue class.
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

/** \file EXP_IntValue.h
 *  \ingroup expressions
 */

#ifndef __EXP_INTVALUE_H__
#define __EXP_INTVALUE_H__


#include "EXP_Value.h"

typedef long long cInt;

class CIntValue : public CPropValue
{
public:
	CIntValue();
	CIntValue(cInt innie);
	CIntValue(cInt innie, const std::string& name);
	virtual ~CIntValue();

	virtual std::string GetText();
	virtual double GetNumber();
	virtual int GetValueType();

	cInt GetInt();

	virtual CValue *Calc(VALUE_OPERATOR op, CValue *val);
	virtual CValue *CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, CValue *val);

	virtual void SetValue(CValue *newval);

	virtual CValue *GetReplica();

#ifdef WITH_PYTHON
	virtual PyObject *ConvertValueToPython();
#endif

private:
	cInt m_int;
};

#endif  // __EXP_INTVALUE_H__
