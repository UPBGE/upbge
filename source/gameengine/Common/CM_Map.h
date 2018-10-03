/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file CM_List.h
 *  \ingroup common
 */

#ifndef __CM_MAP_H__
#define __CM_MAP_H__

#include <map>
#include <unordered_map>

template <class Item, class Key, class ... Args>
inline const Item CM_MapGetItemNoInsert(const std::map<Key, Item, Args ...>& map, const Key& key, const Item defaultItem = nullptr)
{
	const typename std::map<Key, Item, Args ...>::const_iterator it = map.find(key);
	if (it != map.end()) {
		return it->second;
	}
	return defaultItem;
}

template <class Item, class Key, class ... Args>
inline const Item CM_MapGetItemNoInsert(const std::unordered_map<Key, Item, Args ...>& map, const Key& key, const Item defaultItem = nullptr)
{
	const typename std::map<Key, Item, Args ...>::const_iterator it = map.find(key);
	if (it != map.end()) {
		return it->second;
	}
	return defaultItem;
}

template <class Map, class Item>
inline bool CM_MapRemoveIfItemFound(Map& map, const Item& item)
{
	bool found = false;
	for (typename Map::iterator it = map.begin(); it != map.end();) {
		if (it->second == item) {
			it = map.erase(it);
			found = true;
		}
		else {
			++it;
		}
	}

	return found;
}

#endif  // __CM_LIST_H__
