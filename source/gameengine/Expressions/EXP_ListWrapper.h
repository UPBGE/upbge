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
 * Contributor(s): Porteries Tristan.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file EXP_ListWrapper.h
 *  \ingroup expressions
 */

#ifdef WITH_PYTHON

#ifndef __EXP_LISTWRAPPER_H__
#define __EXP_LISTWRAPPER_H__

#include "EXP_BaseListWrapper.h"

// TODO ne pas utiliser PyObject.
template <class Object,
	unsigned int (Object::*GetSizeFunc)(),
	PyObject *(Object::*GetItemFunc)(unsigned int),
	bool (Object::*SetItemFunc)(unsigned int, PyObject *) = nullptr,
	std::string (Object::*GetItemNameFunc)(unsigned int) = nullptr>
class EXP_ListWrapper : public EXP_BaseListWrapper
{
private:
	static PyObject *GetItem(EXP_PyObjectPlus *self, unsigned int index)
	{
		return (static_cast<Object *>(self)->*GetItemFunc)(index);
	}

	static std::string GetItemName(EXP_PyObjectPlus *self, unsigned int index)
	{
		return (static_cast<Object *>(self)->*GetItemNameFunc)(index);
	}

	static unsigned int GetSize(EXP_PyObjectPlus *self)
	{
		return (static_cast<Object *>(self)->*GetSizeFunc)();
	}

	static bool SetItem(EXP_PyObjectPlus *self, unsigned int index, PyObject *item)
	{
		return (static_cast<Object *>(self)->*SetItemFunc)(index, item);
	}

public:
	EXP_ListWrapper(EXP_PyObjectPlus *client, Flag flag = FLAG_NONE)
		:EXP_BaseListWrapper(client, GetSize, GetItem,
				GetItemNameFunc ? GetItemName : nullptr,
				SetItemFunc ? SetItem : nullptr, flag)
	{
	}
	virtual ~EXP_ListWrapper()
	{
	}
};

#endif // __EXP_LISTWRAPPER_H__

#endif // WITH_PYTHON
