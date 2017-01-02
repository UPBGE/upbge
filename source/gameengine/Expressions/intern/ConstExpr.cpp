/** \file gameengine/Expressions/ConstExpr.cpp
 *  \ingroup expressions
 */
// ConstExpr.cpp: implementation of the CConstExpr class.

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

#include "EXP_Value.h" // for precompiled header
#include "EXP_ConstExpr.h"

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CConstExpr::CConstExpr()
{
}



CConstExpr::CConstExpr(CValue* constval) 
/*
pre:
effect: constructs a CConstExpr cointing the value constval
*/
{
	m_value = constval;
//	m_bModified=true;
}



CConstExpr::~CConstExpr()
/*
pre:
effect: deletes the object
*/
{
	if (m_value)
		m_value->Release();
}



unsigned char CConstExpr::GetExpressionID()
{
	return CCONSTEXPRESSIONID;
}

CValue* CConstExpr::Calculate()
/*
pre:
ret: a new object containing the value of the stored CValue
*/
{
	return m_value->AddRef();
}

double CConstExpr::GetNumber()
{
	return -1;
}
