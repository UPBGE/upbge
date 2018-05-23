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

/** Smart Boolean Value class.
 * Is used by parser when an expression tree is build containing booleans.
 */
class EXP_BoolValue : public EXP_PropValue
{
public:
	EXP_BoolValue(bool inBool);

	virtual std::string GetText() const;
	virtual DATA_TYPE GetValueType() const;

	bool GetValue() const;
	void SetValue(bool value);

	virtual EXP_PropValue *GetReplica();
#ifdef WITH_PYTHON
	virtual PyObject *ConvertValueToPython();
#endif

private:
	bool m_value;
};

#endif  // __EXP_BOOLVALUE_H__
