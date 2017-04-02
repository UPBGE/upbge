/*
 * ListValue.h: interface for the CListValue class.
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

/** \file EXP_ListValue.h
 *  \ingroup expressions
 */

#ifndef __EXP_LISTVALUE_H__
#define __EXP_LISTVALUE_H__

#include "EXP_Value.h"

#include <functional>

class CListValue : public CPropValue
{
	Py_Header

public:
	typedef std::vector<CValue *> VectorType;
	typedef VectorType::const_iterator VectorTypeIterator;

private:
	VectorType m_pValueArray;
	bool m_bReleaseContents;

public:
	CListValue();
	virtual ~CListValue();

	void Add(CValue *value);
	void Insert(unsigned int i, CValue *value);

	virtual int GetValueType();
	virtual CValue *GetReplica();

	template <class T>
	class iterator
	{
private:
		VectorTypeIterator m_it;

public:
		iterator(VectorTypeIterator it)
			:m_it(it)
		{
		}

		inline void operator++()
		{
			++m_it;
		}

		inline T *operator*() const
		{
			return static_cast<T *>(*m_it);
		}

		template <class U>
		friend inline bool operator!=(const iterator<U>& it1, const iterator<U>& it2);
	};

	void MergeList(CListValue *otherlist);
	bool CheckEqual(CValue *first, CValue *second);
	bool RemoveValue(CValue *val);
	void SetReleaseOnDestruct(bool bReleaseContents);
	bool SearchValue(CValue *val);

	CValue *FindValue(const std::string& name);

	template <class ItemType>
	ItemType *FindIf(std::function<bool (ItemType *)> function)
	{
		for (CValue *val : m_pValueArray) {
			if (function(static_cast<ItemType *>(val))) {
				return static_cast<ItemType *>(val);
			}
		}
		return nullptr;
	}

	void ReleaseAndRemoveAll();
	void Remove(int i);
	void Resize(int num);
	void SetValue(int i, CValue *val);
	CValue *GetValue(int i);
	CValue *GetFront();
	CValue *GetBack();
	int GetCount();
	VectorTypeIterator GetBegin() const;
	VectorTypeIterator GetEnd() const;
	virtual const std::string GetText();

#ifdef WITH_PYTHON
	virtual PyObject *py_repr()
	{
		PyObject *py_proxy = this->GetProxy();
		PyObject *py_list = PySequence_List(py_proxy);
		PyObject *py_string = PyObject_Repr(py_list);
		Py_DECREF(py_list);
		Py_DECREF(py_proxy);
		return py_string;
	}

	KX_PYMETHOD_O(CListValue, append);
	KX_PYMETHOD_NOARGS(CListValue, reverse);
	KX_PYMETHOD_O(CListValue, index);
	KX_PYMETHOD_O(CListValue, count);
	KX_PYMETHOD_VARARGS(CListValue, get);
	KX_PYMETHOD_VARARGS(CListValue, filter);
	KX_PYMETHOD_O(CListValue, from_id);
#endif
};

template <class T>
inline bool operator!=(const CListValue::iterator<T>& it1, const CListValue::iterator<T>& it2)
{
	return it1.m_it != it2.m_it;
}

#endif  // __EXP_LISTVALUE_H__

