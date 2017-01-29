/*
 * ConstExpr.h: interface for the CConstExpr class.
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

/** \file EXP_ConstExpr.h
 *  \ingroup expressions
 */

#ifndef __EXP_CONSTEXPR_H__
#define __EXP_CONSTEXPR_H__

#include "EXP_Expression.h"
#include "EXP_Value.h"

class CConstExpr : public CExpression
{
public:
	CConstExpr();
	CConstExpr(CValue *constval);
	virtual ~CConstExpr();

	virtual unsigned char GetExpressionID();
	virtual double GetNumber();
	virtual CValue *Calculate();

private:
	CValue *m_value;
};

#endif  // __EXP_CONSTEXPR_H__
