/*
 * BoolValue.h: interface for the EXP_PropBool class.
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

/** \file EXP_PropBool.h
 *  \ingroup expressions
 */

#ifndef __EXP_BOOLVALUE_H__
#define __EXP_BOOLVALUE_H__

#include "EXP_PropValue.h"

/** Smart Boolean Value class.
 * Is used by parser when an expression tree is build containing booleans.
 */
class EXP_PropBool : public EXP_PropValue
{
public:
	EXP_PropBool(bool inBool);

	virtual std::string GetText() const;
	virtual DataType GetValueType() const;

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
