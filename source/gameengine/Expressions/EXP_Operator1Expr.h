/*
 * Operator1Expr.h: interface for the COperator1Expr class.
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

/** \file EXP_Operator1Expr.h
 *  \ingroup expressions
 */

#ifndef __EXP_OPERATOR1EXPR_H__
#define __EXP_OPERATOR1EXPR_H__

#include "EXP_Expression.h"

class COperator1Expr : public CExpression
{
public:
	COperator1Expr();
	COperator1Expr(VALUE_OPERATOR op, CExpression *lhs);
	virtual ~COperator1Expr();

	virtual unsigned char GetExpressionID();
	virtual CValue *Calculate();

private:
	VALUE_OPERATOR m_op;
	CExpression *m_lhs;
};

#endif  // __EXP_OPERATOR1EXPR_H__
