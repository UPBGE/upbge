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

class CListValue : public CPropValue  
{
	Py_Header
	//PLUGIN_DECLARE_SERIAL (CListValue,CValue)

public:
	CListValue();
	virtual ~CListValue();

	void AddConfigurationData(CValue* menuvalue);
	void Configure(CValue* menuvalue);
	void Add(CValue* value);
	void Insert(unsigned int i, CValue* value);

	virtual int GetValueType();
	virtual CValue* GetReplica();

public:
	typedef std::vector<CValue *>::iterator baseIterator;

	template <class T>
	class iterator
	{
	private:
		baseIterator m_it;

	public:
		iterator(baseIterator it)
			:m_it(it)
		{
		}

		inline void operator++()
		{
			++m_it;
		}

		inline T *operator*()
		{
			return (T *)*m_it;
		}

		template <class U>
		friend bool operator!=(const iterator<U>& it1, const iterator<U>& it2);
	};

	void MergeList(CListValue* otherlist);
	bool RemoveValue(CValue* val);
	void SetReleaseOnDestruct(bool bReleaseContents);
	bool SearchValue(CValue* val);
	
	CValue* FindValue(const std::string& name);

	void ReleaseAndRemoveAll();
	virtual void SetModified(bool bModified);
	virtual inline bool IsModified();
	void Remove(int i);
	void Resize(int num);
	void SetValue(int i,CValue* val);
	CValue* GetValue(int i) { BLI_assert(i < m_pValueArray.size()); return m_pValueArray[i]; }
	CValue *GetFront();
	CValue *GetBack();
	int GetCount() { return m_pValueArray.size(); }
	baseIterator GetBegin();
	baseIterator GetEnd();
	virtual const std::string GetText();

	bool CheckEqual(CValue* first,CValue* second);

#ifdef WITH_PYTHON
	virtual PyObject *py_repr(void) {
		PyObject *py_proxy= this->GetProxy();
		PyObject *py_list= PySequence_List(py_proxy);
		PyObject *py_string= PyObject_Repr(py_list);
		Py_DECREF(py_list);
		Py_DECREF(py_proxy);
		return py_string;
	}

	KX_PYMETHOD_O(CListValue,append);
	KX_PYMETHOD_NOARGS(CListValue,reverse);
	KX_PYMETHOD_O(CListValue,index);
	KX_PYMETHOD_O(CListValue,count);
	KX_PYMETHOD_VARARGS(CListValue,get);
	KX_PYMETHOD_O(CListValue,from_id);
#endif
	
private:

	std::vector<CValue*> m_pValueArray;
	bool	m_bReleaseContents;
};

template <class T>
inline bool operator!=(const CListValue::iterator<T>& it1, const CListValue::iterator<T>& it2)
{
	return it1.m_it != it2.m_it;
}

#endif  /* __EXP_LISTVALUE_H__ */

