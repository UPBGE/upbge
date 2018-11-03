/*
 * MapValue.h: interface for the EXP_MapValue class.
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

/** \file EXP_MapValue.h
 *  \ingroup expressions
 */

#ifndef __EXP_MAPVALUE_H__
#define __EXP_MAPVALUE_H__

#include "EXP_BaseMapValue.h"

#include <functional>

template <class Item>
class EXP_MapValue : public EXP_BaseMapValue
{
public:
	using _MapType = std::unordered_map<std::string, Item>;
	using _PairType = typename _MapType::value_type;

	class const_iterator
	{
	public:
		MapTypeConstIterator m_it;

		const_iterator(MapTypeConstIterator it)
		: m_it(it)
		{
		}

		inline void operator++()
		{
			++m_it;
		}

		inline _PairType *operator*() const
		{
			return static_cast<_PairType *>(*m_it);
		}
	};

	EXP_MapValue()
	{
	}

	EXP_MapValue(const _MapType& rawMap)
	{
		for (const auto& pair : rawMap) {
			m_map.insert(pair);
		}
	}

	virtual ~EXP_MapValue()
	{
	}

	virtual EXP_MapValue<Item> *GetReplica()
	{
		EXP_MapValue<Item> *replica = new EXP_MapValue<Item>(*this);

		replica->ProcessReplica();

		// Copy all values.
		for (const auto& pair : m_map) {
			replica->m_map.emplace(pair.first, pair.second->GetReplica());
		}

		return replica;
	}

	Item *Find(const std::string& name) const
	{
		return static_cast<Item *>(EXP_BaseMapValue::Find(name));
	}

	bool Contain(Item *value) const
	{
		return EXP_BaseMapValue::Contain(value);
	}

	bool Insert(const std::string& name, Item *value)
	{
		return EXP_BaseMapValue::Insert(name, value);
	}

	bool RemoveValue(Item *value)
	{
		return EXP_BaseMapValue::RemoveValue(value);
	}

	void Merge(EXP_MapValue<Item>& other)
	{
		EXP_BaseMapValue::Merge(other);
	}

	const_iterator begin()
	{
		return const_iterator(m_map.begin());
	}
	const_iterator end()
	{
		return const_iterator(m_map.end());
	}
};

template<class Item>
typename EXP_MapValue<Item>::const_iterator begin(EXP_MapValue<Item> *list)
{
	return list->begin();
}

template<class Item>
typename EXP_MapValue<Item>::const_iterator end(EXP_MapValue<Item> *list)
{
	return list->end();
}

template<class Item>
typename EXP_MapValue<Item>::const_iterator begin(std::unique_ptr<EXP_MapValue<Item> >& list)
{
	return list->begin();
}

template<class Item>
typename EXP_MapValue<Item>::const_iterator end(std::unique_ptr<EXP_MapValue<Item> >& list)
{
	return list->end();
}

template <class Iterator, typename = is_base_of<Iterator, >::value) >
bool operator!=(const Iterator& it1, const Iterator& it2)
{
	return it1.m_it != it2.m_it;
}

#endif  // __EXP_MAPVALUE_H__

