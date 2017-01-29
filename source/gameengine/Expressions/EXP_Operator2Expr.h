/*
 * Operator2Expr.h: interface for the COperator2Expr class.
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

/** \file EXP_Operator2Expr.h
 *  \ingroup expressions
 */

#ifndef __EXP_OPERATOR2EXPR_H__
#define __EXP_OPERATOR2EXPR_H__

#include "EXP_Expression.h"
#include "EXP_Value.h"

class COperator2Expr : public CExpression
{
public:
	COperator2Expr();
	COperator2Expr(VALUE_OPERATOR op, CExpression *lhs, CExpression *rhs);
	virtual ~COperator2Expr();

	virtual unsigned char GetExpressionID();
	virtual CValue *Calculate();

protected:
	CExpression *m_rhs;
	CExpression *m_lhs;

private:
	VALUE_OPERATOR m_op;
};

#endif  // __EXP_OPERATOR2EXPR_H__

