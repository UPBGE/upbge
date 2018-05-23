/*
 * StringValue.h: interface for the EXP_StringValue class.
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

class EXP_StringValue : public EXP_PropValue
{
public:
	EXP_StringValue(const std::string& txt);

	virtual std::string GetText() const;
	virtual DATA_TYPE GetValueType() const;

	const std::string& GetValue() const;
	void SetValue(const std::string& value);

	virtual EXP_PropValue *GetReplica();

#ifdef WITH_PYTHON
	virtual PyObject *ConvertValueToPython();
#endif  // WITH_PYTHON

private:
	/// Data member.
	std::string m_value;
};

#endif  // __EXP_STRINGVALUE_H__
