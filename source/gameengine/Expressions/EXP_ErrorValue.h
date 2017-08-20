/*
 * ErrorValue.h: interface for the CErrorValue class.
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

/** \file EXP_ErrorValue.h
 *  \ingroup expressions
 */

#ifndef __EXP_ERRORVALUE_H__
#define __EXP_ERRORVALUE_H__

#include "EXP_Value.h"

class CErrorValue : public CPropValue
{
public:
	CErrorValue();
	CErrorValue(const std::string& errmsg);
	virtual ~CErrorValue();

	virtual std::string GetText();
	virtual int GetValueType();
	virtual CValue *Calc(VALUE_OPERATOR op, CValue *val);
	virtual CValue *CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, CValue *val);
	virtual CValue *GetReplica();

private:
	std::string m_strErrorText;
};

#endif  // __EXP_ERRORVALUE_H__
