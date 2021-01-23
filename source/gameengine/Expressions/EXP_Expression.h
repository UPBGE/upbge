/*
 * Expression.h: interface for the EXP_Expression class.
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

/** \file EXP_Expression.h
 *  \ingroup expressions
 */

#pragma once

#include "EXP_Value.h"

class EXP_Expression : public CM_RefCount<EXP_Expression> {
 public:
  enum {
    COPERATOR1EXPRESSIONID = 1,
    COPERATOR2EXPRESSIONID = 2,
    CCONSTEXPRESSIONID = 3,
    CIFEXPRESSIONID = 4,
    COPERATORVAREXPRESSIONID = 5,
    CIDENTIFIEREXPRESSIONID = 6
  };

 protected:
  virtual ~EXP_Expression() = 0;

 public:
  EXP_Expression();

  virtual EXP_Value *Calculate() = 0;
  virtual unsigned char GetExpressionID() = 0;
};
