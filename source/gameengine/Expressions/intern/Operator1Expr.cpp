/** \file gameengine/Expressions/Operator1Expr.cpp
 *  \ingroup expressions
 */
// Operator1Expr.cpp: implementation of the EXP_Operator1Expr class.
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

EXP_Operator1Expr::EXP_Operator1Expr() : m_lhs(nullptr)
{
}

EXP_Operator1Expr::EXP_Operator1Expr(VALUE_OPERATOR op, EXP_Expression *lhs) : m_op(op), m_lhs(lhs)
{
}

EXP_Operator1Expr::~EXP_Operator1Expr()
{
  if (m_lhs) {
    m_lhs->Release();
  }
}

unsigned char EXP_Operator1Expr::GetExpressionID()
{
  return COPERATOR1EXPRESSIONID;
}

EXP_Value *EXP_Operator1Expr::Calculate()
{
  EXP_Value *temp = m_lhs->Calculate();
  EXP_Value *empty = new EXP_EmptyValue();
  EXP_Value *ret = empty->Calc(m_op, temp);

  empty->Release();
  temp->Release();

  return ret;
}
