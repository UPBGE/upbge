/*
 * Operator1Expr.h: interface for the EXP_Operator1Expr class.
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

#pragma once

#include "EXP_Expression.h"

class EXP_Operator1Expr : public EXP_Expression {
 public:
  EXP_Operator1Expr();
  EXP_Operator1Expr(VALUE_OPERATOR op, EXP_Expression *lhs);
  virtual ~EXP_Operator1Expr();

  virtual unsigned char GetExpressionID();
  virtual EXP_Value *Calculate();

 private:
  VALUE_OPERATOR m_op;
  EXP_Expression *m_lhs;
};
