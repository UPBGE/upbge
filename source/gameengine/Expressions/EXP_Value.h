/*
 * Value.h: interface for the EXP_Value class.
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

/** \file EXP_Value.h
 *  \ingroup expressions
 */

#ifndef __EXP_VALUE_H__
#define __EXP_VALUE_H__

#include "EXP_PyObjectPlus.h"

/**
 * Baseclass EXP_Value
 *
 * Base class for all editor functionality, flexible object type that allows
 * calculations.
 *
 */
class EXP_Value : public EXP_PyObjectPlus
{
	Py_Header
public:
	EXP_Value() = default;
	EXP_Value(const EXP_Value& other) = default;
	virtual ~EXP_Value() = default;

	EXP_Value& operator=(const EXP_Value& other) = delete;
	EXP_Value& operator=(EXP_Value&& other) = default;

#ifdef WITH_PYTHON
	virtual PyObject *py_repr(void)
	{
		return PyUnicode_FromStdString(GetText());
	}

	static PyObject *pyattr_get_name(EXP_PyObjectPlus *self, const EXP_PYATTRIBUTE_DEF *attrdef);

#endif  // WITH_PYTHON

	virtual std::string GetText() const;

	/// Retrieve the name of the value.
	virtual std::string GetName() const = 0;
	/// Set the name of the value.
	virtual void SetName(const std::string& name);

	virtual EXP_Value *GetReplica();
	virtual void ProcessReplica();

	/// Return true when the type is based on EXP_Dictionary and have property support.
	virtual bool IsDictionary() const;

protected:
	virtual void DestructFromPython();
};

#endif  // __EXP_VALUE_H__
