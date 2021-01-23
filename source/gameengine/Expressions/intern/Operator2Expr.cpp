/** \file gameengine/Expressions/Operator2Expr.cpp
 *  \ingroup expressions
 */
// Operator2Expr.cpp: implementation of the EXP_Operator2Expr class.
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

EXP_Operator2Expr::EXP_Operator2Expr(VALUE_OPERATOR op, EXP_Expression *lhs, EXP_Expression *rhs)
    : m_rhs(rhs), m_lhs(lhs), m_op(op)
{
}

EXP_Operator2Expr::EXP_Operator2Expr() : m_rhs(nullptr), m_lhs(nullptr)
{
}

EXP_Operator2Expr::~EXP_Operator2Expr()
{
  if (m_lhs) {
    m_lhs->Release();
  }
  if (m_rhs) {
    m_rhs->Release();
  }
}

unsigned char EXP_Operator2Expr::GetExpressionID()
{
  return COPERATOR2EXPRESSIONID;
}

EXP_Value *EXP_Operator2Expr::Calculate()
{

  EXP_Value *ffleft = m_lhs->Calculate();
  EXP_Value *ffright = m_rhs->Calculate();

  EXP_Value *calculate = ffleft->Calc(m_op, ffright);

  ffleft->Release();
  ffright->Release();

  return calculate;
}
