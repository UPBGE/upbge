/*
 * Expression.h: interface for the CExpression class.
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

#ifndef __EXP_EXPRESSION_H__
#define __EXP_EXPRESSION_H__

#include "EXP_Value.h"

class CExpression : public CM_RefCount<CExpression>
{
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
	virtual ~CExpression() = 0;

public:
	CExpression();

	virtual CValue *Calculate() = 0;
	virtual unsigned char GetExpressionID() = 0;
};

#endif  // __EXP_EXPRESSION_H__
