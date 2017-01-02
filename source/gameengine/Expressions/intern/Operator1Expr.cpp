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

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

COperator1Expr::COperator1Expr()
/*
pre:
effect: constucts an empty COperator1Expr
*/
{
	m_lhs = NULL;
}

COperator1Expr::COperator1Expr(VALUE_OPERATOR op, CExpression *lhs)
/*
pre:
effect: constucts a COperator1Expr with op and lhs in it
*/
{
	m_lhs = lhs;
	m_op = op;
}

COperator1Expr::~COperator1Expr()
/*
pre:
effect: deletes the object
*/
{
	if (m_lhs) m_lhs->Release();
}

CValue * COperator1Expr::Calculate()
/*
pre:
ret: a new object containing the result of applying the operator m_op to the
	 value of m_lhs
*/
{
	CValue *ret;
	CValue *temp = m_lhs->Calculate();
	CValue* empty = new CEmptyValue();
	ret = empty->Calc(m_op, temp);
	empty->Release();
	temp->Release();
	
	return ret;
}
