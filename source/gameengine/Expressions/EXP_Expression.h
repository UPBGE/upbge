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

class CExpression  
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
	virtual				~CExpression() = 0;			//pure virtual
public:
	CExpression();

	
	virtual				CValue* Calculate() = 0;	//pure virtual
	virtual	unsigned char GetExpressionID() = 0;

	virtual CExpression * AddRef() { // please leave multiline, for debugger !!!
		m_refcount++; 
		return this;
	};
	virtual CExpression* Release(CExpression* complicatedtrick=NULL) { 
		if (--m_refcount < 1) 
		{
			delete this;
		} //else
		//	return this;
		return complicatedtrick;
	};
	

protected:

	int m_refcount;


#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:CExpression")
#endif
};

#endif  /* __EXP_EXPRESSION_H__ */
