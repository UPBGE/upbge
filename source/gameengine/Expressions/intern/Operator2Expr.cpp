/** \file gameengine/Expressions/Operator2Expr.cpp
 *  \ingroup expressions
 */
// Operator2Expr.cpp: implementation of the COperator2Expr class.
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

#include "EXP_Operator2Expr.h"
#include "EXP_StringValue.h"

COperator2Expr::COperator2Expr(VALUE_OPERATOR op, CExpression *lhs, CExpression *rhs)
	:m_rhs(rhs),
	m_lhs(lhs),
	m_op(op)
{
}

COperator2Expr::COperator2Expr() :
	m_rhs(nullptr),
	m_lhs(nullptr)
{
}

COperator2Expr::~COperator2Expr()
{
	if (m_lhs) {
		m_lhs->Release();
	}
	if (m_rhs) {
		m_rhs->Release();
	}
}

unsigned char COperator2Expr::GetExpressionID()
{
	return COPERATOR2EXPRESSIONID;
}

CValue *COperator2Expr::Calculate()
{

	CValue *ffleft = m_lhs->Calculate();
	CValue *ffright = m_rhs->Calculate();

	CValue *calculate = ffleft->Calc(m_op, ffright);

	ffleft->Release();
	ffright->Release();

	return calculate;
}
