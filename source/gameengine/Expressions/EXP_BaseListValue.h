/*
 * ListValue.h: interface for the CBaseListValue class.
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

/** \file EXP_BaseListValue.h
 *  \ingroup expressions
 */

#ifndef __EXP_BASELISTVALUE_H__
#define __EXP_BASELISTVALUE_H__

#include "EXP_Value.h"

class CBaseListValue : public CPropValue
{
	Py_Header

public:
	typedef std::vector<CValue *> VectorType;
	typedef VectorType::iterator VectorTypeIterator;
	typedef VectorType::const_iterator VectorTypeConstIterator;

protected:
	VectorType m_pValueArray;
	bool m_bReleaseContents;

	void SetValue(int i, CValue *val);
	CValue *GetValue(int i);
	CValue *FindValue(const std::string& name) const;
	bool SearchValue(CValue *val) const;
	void Add(CValue *value);
	void Insert(unsigned int i, CValue *value);
	bool RemoveValue(CValue *val);
	bool CheckEqual(CValue *first, CValue *second);

public:
	CBaseListValue();
	virtual ~CBaseListValue();

	virtual int GetValueType();
	virtual CValue *GetReplica() = 0;
	virtual std::string GetText();

	void SetReleaseOnDestruct(bool bReleaseContents);

	void Remove(int i);
	void Resize(int num);
	void ReleaseAndRemoveAll();
	int GetCount() const;

#ifdef WITH_PYTHON

	KX_PYMETHOD_O(CBaseListValue, append);
	KX_PYMETHOD_NOARGS(CBaseListValue, reverse);
	KX_PYMETHOD_O(CBaseListValue, index);
	KX_PYMETHOD_O(CBaseListValue, count);
	KX_PYMETHOD_VARARGS(CBaseListValue, get);
	KX_PYMETHOD_VARARGS(CBaseListValue, filter);
	KX_PYMETHOD_O(CBaseListValue, from_id);

	static Py_ssize_t bufferlen(PyObject *self);
	static PyObject *buffer_item(PyObject *self, Py_ssize_t index);
	static PyObject *buffer_slice(CBaseListValue *list, Py_ssize_t start, Py_ssize_t stop);
	static PyObject *mapping_subscript(PyObject *self, PyObject *key);
	static PyObject *buffer_concat(PyObject *self, PyObject *other);
	static int buffer_contains(PyObject *self_v, PyObject *value);

	static PySequenceMethods as_sequence;
	static PyMappingMethods instance_as_mapping;
#endif
};

#endif  // __EXP_LISTVALUE_H__

