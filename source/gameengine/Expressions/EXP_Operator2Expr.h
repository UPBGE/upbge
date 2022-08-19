/*
 * Operator2Expr.h: interface for the EXP_Operator2Expr class.
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

#pragma once

#include "EXP_Expression.h"
#include "EXP_Value.h"

class EXP_Operator2Expr : public EXP_Expression {
 public:
  EXP_Operator2Expr();
  EXP_Operator2Expr(VALUE_OPERATOR op, EXP_Expression *lhs, EXP_Expression *rhs);
  virtual ~EXP_Operator2Expr();

  virtual unsigned char GetExpressionID();
  virtual EXP_Value *Calculate();

 protected:
  EXP_Expression *m_rhs;
  EXP_Expression *m_lhs;

 private:
  VALUE_OPERATOR m_op;
};
