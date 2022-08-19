/*
 * IfExpr.h: interface for the EXP_IfExpr class.
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

/** \file EXP_IfExpr.h
 *  \ingroup expressions
 */

#pragma once

#include "EXP_Expression.h"

class EXP_IfExpr : public EXP_Expression {
 private:
  EXP_Expression *m_guard;
  EXP_Expression *m_e1;
  EXP_Expression *m_e2;

 public:
  EXP_IfExpr();
  EXP_IfExpr(EXP_Expression *guard, EXP_Expression *e1, EXP_Expression *e2);
  virtual ~EXP_IfExpr();

  virtual unsigned char GetExpressionID();
  virtual EXP_Value *Calculate();
};
