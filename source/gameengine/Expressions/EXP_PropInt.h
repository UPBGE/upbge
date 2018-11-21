/*
 * IntValue.h: interface for the EXP_PropInt class.
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

/** \file EXP_PropInt.h
 *  \ingroup expressions
 */

#ifndef __EXP_INTVALUE_H__
#define __EXP_INTVALUE_H__


#include "EXP_PropValue.h"

class EXP_PropInt : public EXP_PropValue
{
public:
	EXP_PropInt(long long innie);

	virtual std::string GetText() const;
	virtual DataType GetValueType() const;

	long long GetValue() const;
	void SetValue(long long value);

	virtual EXP_PropValue *GetReplica();

#ifdef WITH_PYTHON
	virtual PyObject *ConvertValueToPython();
#endif

private:
	long long m_value;
};

#endif  // __EXP_INTVALUE_H__
