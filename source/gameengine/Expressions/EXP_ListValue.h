/*
 * ListValue.h: interface for the EXP_ListValue class.
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

#include "EXP_BaseListValue.h"
#include "EXP_BoolValue.h"

#include <functional>

#include <algorithm>

template <class ItemType>
class EXP_ListValue : public EXP_BaseListValue
{
public:
	class const_iterator
	{
	public:
		VectorTypeConstIterator m_it;

		const_iterator(VectorTypeConstIterator it)
		: m_it(it)
		{
		}

		inline void operator++()
		{
			++m_it;
		}

		inline ItemType *operator*() const
		{
			return static_cast<ItemType *>(*m_it);
		}
	};

	EXP_ListValue()
	{
	}

	EXP_ListValue(const std::vector<ItemType *>& rawList)
	{
		const unsigned int size = rawList.size();
		m_valueArray.resize(size);
		for (unsigned int i = 0; i < size; ++i) {
			m_valueArray[i] = rawList[i];
		}
	}

	virtual ~EXP_ListValue()
	{
	}

	virtual EXP_ListValue<ItemType> *GetReplica()
	{
		EXP_ListValue<ItemType> *replica = new EXP_ListValue<ItemType>(*this);

		replica->ProcessReplica();

		replica->m_bReleaseContents = true; // For copy, complete array is copied for now...
		// Copy all values.
		const int numelements = m_valueArray.size();
		replica->m_valueArray.resize(numelements);
		for (unsigned int i = 0; i < numelements; i++) {
			replica->m_valueArray[i] = m_valueArray[i]->GetReplica();
		}

		return replica;
	}

	void Add(ItemType *value)
	{
		EXP_BaseListValue::Add(value);
	}

	void Insert(unsigned int i, ItemType *value)
	{
		EXP_BaseListValue::Insert(i, value);
	}

	ItemType *FindIf(std::function<bool (ItemType *)> function)
	{
		for (EXP_Value *val : m_valueArray) {
			ItemType *item = static_cast<ItemType *>(val);
			if (function(item)) {
				return item;
			}
		}
		return nullptr;
	}

	void MergeList(EXP_ListValue<ItemType> *otherlist)
	{
		const unsigned int numelements = GetCount();
		const unsigned int numotherelements = otherlist->GetCount();

		Resize(numelements + numotherelements);

		for (int i = 0; i < numotherelements; i++) {
			SetValue(i + numelements, CM_AddRef(otherlist->GetValue(i)));
		}
	}

	bool SearchValue(ItemType *val) const
	{
		return EXP_BaseListValue::SearchValue(val);
	}
	ItemType *FindValue(const std::string& name) const
	{
		return static_cast<ItemType *>(EXP_BaseListValue::FindValue(name));
	}

	/** \note Allow to remove by base class pointer as an upcast from this class type
	 * to the item type could failed if the pointer is dangling, in example when it was
	 * just deleted.
	 */
	bool RemoveValue(EXP_Value *val)
	{
		return EXP_BaseListValue::RemoveValue(val);
	}

	void SetValue(int i, ItemType *val)
	{
		EXP_BaseListValue::SetValue(i, val);
	}
	ItemType *GetValue(int i)
	{
		return static_cast<ItemType *>(EXP_BaseListValue::GetValue(i));
	}

	ItemType *GetFront()
	{
		return static_cast<ItemType *>(m_valueArray.front());
	}
	ItemType *GetBack()
	{
		return static_cast<ItemType *>(m_valueArray.back());
	}

	const_iterator begin()
	{
		return const_iterator(m_valueArray.begin());
	}
	const_iterator end()
	{
		return const_iterator(m_valueArray.end());
	}
};

template<class ItemType>
typename EXP_ListValue<ItemType>::const_iterator begin(EXP_ListValue<ItemType> *list)
{
	return list->begin();
}

template<class ItemType>
typename EXP_ListValue<ItemType>::const_iterator end(EXP_ListValue<ItemType> *list)
{
	return list->end();
}

template <class Iterator, typename = decltype(std::declval<Iterator>().m_it) >
bool operator!=(const Iterator& it1, const Iterator& it2)
{
	return it1.m_it != it2.m_it;
}

#endif  // __EXP_LISTVALUE_H__

