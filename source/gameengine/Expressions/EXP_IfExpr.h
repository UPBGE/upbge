/*
 * IfExpr.h: interface for the CIfExpr class.
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

/** \file EXP_IfExpr.h
 *  \ingroup expressions
 */

#ifndef __EXP_IFEXPR_H__
#define __EXP_IFEXPR_H__

#include "EXP_Expression.h"

class CIfExpr : public CExpression
{
private:
	CExpression *m_guard;
	CExpression *m_e1;
	CExpression *m_e2;

public:
	CIfExpr();
	CIfExpr(CExpression *guard, CExpression *e1, CExpression *e2);
	virtual ~CIfExpr();

	virtual unsigned char GetExpressionID();
	virtual CValue *Calculate();
};

#endif  // __EXP_IFEXPR_H__
