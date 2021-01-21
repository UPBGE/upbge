/*
 * EmptyValue.h: interface for the EXP_EmptyValue class.
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

/** \file EXP_EmptyValue.h
 *  \ingroup expressions
 */

#pragma once

#include "EXP_Value.h"

class EXP_EmptyValue : public EXP_PropValue {
 public:
  EXP_EmptyValue();
  virtual ~EXP_EmptyValue();

  virtual std::string GetText();
  virtual double GetNumber();
  virtual int GetValueType();
  virtual EXP_Value *Calc(VALUE_OPERATOR op, EXP_Value *val);
  virtual EXP_Value *CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, EXP_Value *val);
  virtual EXP_Value *GetReplica();
};
