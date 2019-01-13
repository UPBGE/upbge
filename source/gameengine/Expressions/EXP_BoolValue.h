/*
 * BoolValue.h: interface for the EXP_BoolValue class.
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

/** \file EXP_BoolValue.h
 *  \ingroup expressions
 */

#ifndef __EXP_BOOLVALUE_H__
#define __EXP_BOOLVALUE_H__

#include "EXP_Value.h"

/**
 * Smart Boolean Value class.
 * Is used by parser when an expression tree is build containing booleans.
 */
class EXP_BoolValue : public EXP_PropValue
{
public:
	static const std::string sTrueString;
	static const std::string sFalseString;

	EXP_BoolValue();
	EXP_BoolValue(bool inBool);
	EXP_BoolValue(bool innie, const std::string& name);

	virtual std::string GetText();
	virtual double GetNumber();
	virtual int GetValueType();
	bool GetBool();
	virtual void SetValue(EXP_Value *newval);

	virtual EXP_Value *Calc(VALUE_OPERATOR op, EXP_Value *val);
	virtual EXP_Value *CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, EXP_Value *val);

	virtual EXP_Value *GetReplica();
#ifdef WITH_PYTHON
	virtual PyObject *ConvertValueToPython();
#endif

private:
	bool m_bool;
};

#endif  // __EXP_BOOLVALUE_H__
