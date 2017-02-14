/** \file gameengine/Expressions/Operator1Expr.cpp
 *  \ingroup expressions
 */
// Operator1Expr.cpp: implementation of the COperator1Expr class.
/*
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

#include "EXP_Operator1Expr.h"
#include "EXP_EmptyValue.h"

COperator1Expr::COperator1Expr()
	:m_lhs(nullptr)
{
}

COperator1Expr::COperator1Expr(VALUE_OPERATOR op, CExpression *lhs)
	:m_op(op),
	m_lhs(lhs)
{
}

COperator1Expr::~COperator1Expr()
{
	if (m_lhs) {
		m_lhs->Release();
	}
}

unsigned char COperator1Expr::GetExpressionID()
{
	return COPERATOR1EXPRESSIONID;
}

CValue *COperator1Expr::Calculate()
{
	CValue *temp = m_lhs->Calculate();
	CValue *empty = new CEmptyValue();
	CValue *ret = empty->Calc(m_op, temp);

	empty->Release();
	temp->Release();

	return ret;
}
