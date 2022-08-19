/** \file gameengine/Expressions/ConstExpr.cpp
 *  \ingroup expressions
 */
// ConstExpr.cpp: implementation of the EXP_ConstExpr class.

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

#include "EXP_ConstExpr.h"

EXP_ConstExpr::EXP_ConstExpr()
{
}

EXP_ConstExpr::EXP_ConstExpr(EXP_Value *constval)
{
  m_value = constval;
}

EXP_ConstExpr::~EXP_ConstExpr()
{
  if (m_value) {
    m_value->Release();
  }
}

unsigned char EXP_ConstExpr::GetExpressionID()
{
  return CCONSTEXPRESSIONID;
}

EXP_Value *EXP_ConstExpr::Calculate()
{
  return m_value->AddRef();
}

double EXP_ConstExpr::GetNumber()
{
  return -1.0;
}
