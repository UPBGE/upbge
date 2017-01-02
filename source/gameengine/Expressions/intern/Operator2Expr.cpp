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
// 31 dec 1998 - big update: try to use the cached data for updating, instead of
// rebuilding completely it from left and right node. Modified flags and bounding boxes
// have to do the trick
// when expression is cached, there will be a call to UpdateCalc() instead of Calc()

#include "EXP_Operator2Expr.h"
#include "EXP_StringValue.h"

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

COperator2Expr::COperator2Expr(VALUE_OPERATOR op, CExpression *lhs, CExpression *rhs)
: 	
m_rhs(rhs),
m_lhs(lhs),
m_op(op)
/*
pre:
effect: constucts a COperator2Expr with op, lhs and rhs in it
*/
{

}

COperator2Expr::COperator2Expr():
m_rhs(NULL),
m_lhs(NULL)

/*
pre:
effect: constucts an empty COperator2Expr
*/
{
	
}

COperator2Expr::~COperator2Expr()
/*
pre:
effect: deletes the object
*/
{
	if (m_lhs)
		m_lhs->Release();
	if (m_rhs)
		m_rhs->Release();
	
}
CValue* COperator2Expr::Calculate()
/*
pre:
ret: a new object containing the result of applying operator m_op to m_lhs
and m_rhs
*/
{

	CValue* ffleft = m_lhs->Calculate();
	CValue* ffright = m_rhs->Calculate();

	CValue *calculate = ffleft->Calc(m_op,ffright);

	ffleft->Release();
	ffright->Release();

	return calculate;
}
