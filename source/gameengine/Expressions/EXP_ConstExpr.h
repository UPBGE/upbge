/*
 * ConstExpr.h: interface for the EXP_ConstExpr class.
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

/** \file EXP_ConstExpr.h
 *  \ingroup expressions
 */

#pragma once

#include "EXP_Expression.h"
#include "EXP_Value.h"

class EXP_ConstExpr : public EXP_Expression {
 public:
  EXP_ConstExpr();
  EXP_ConstExpr(EXP_Value *constval);
  virtual ~EXP_ConstExpr();

  virtual unsigned char GetExpressionID();
  virtual double GetNumber();
  virtual EXP_Value *Calculate();

 private:
  EXP_Value *m_value;
};
