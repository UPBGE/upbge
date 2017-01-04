/** \file gameengine/Expressions/IfExpr.cpp
 *  \ingroup expressions
 */
// IfExpr.cpp: implementation of the CIfExpr class.
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

#include "EXP_IfExpr.h"
#include "EXP_EmptyValue.h"
#include "EXP_ErrorValue.h"
#include "EXP_BoolValue.h"

CIfExpr::CIfExpr()
{
}

CIfExpr::CIfExpr(CExpression *guard, CExpression *e1, CExpression *e2)
	:m_guard(guard),
	m_e1(e1),
	m_e2(e2)
{
}

CIfExpr::~CIfExpr()
{
	if (m_guard) {
		m_guard->Release();
	}

	if (m_e1) {
		m_e1->Release();
	}

	if (m_e2) {
		m_e2->Release();
	}
}

CValue *CIfExpr::Calculate()
{
	CValue *guardval = m_guard->Calculate();
	const std::string& text = guardval->GetText();
	guardval->Release();

	if (text == CBoolValue::sTrueString) {
		return m_e1->Calculate();
	}
	else if (text == CBoolValue::sFalseString) {
		return m_e2->Calculate();
	}
	else {
		return new CErrorValue("Guard should be of boolean type");
	}
}

unsigned char CIfExpr::GetExpressionID()
{
	return CIFEXPRESSIONID;
}
