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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file SCA_IScene.h
 *  \ingroup gamelogic
 */

#ifndef __SCA_ISCENE_H__
#define __SCA_ISCENE_H__

#include <vector>

#include <string>
#include "RAS_2DFilterData.h"

#define DEBUG_MAX_DISPLAY 100

struct SCA_DebugProp
{
	class CValue*	m_obj;
	std::string 		m_name;
	SCA_DebugProp();
	~SCA_DebugProp();
};

class SCA_IScene 
{
	std::vector<SCA_DebugProp*> m_debugList;
public:
	SCA_IScene();
	virtual ~SCA_IScene();
	virtual class SCA_IObject* AddReplicaObject(class CValue* gameobj,
												class CValue* locationobj,
												float lifespan=0.0f)=0;
	virtual void	RemoveObject(class CValue* gameobj)=0;
	virtual void	DelayedRemoveObject(class CValue* gameobj)=0;
	//virtual void	DelayedReleaseObject(class CValue* gameobj)=0;
	
	virtual void	ReplaceMesh(class CValue* gameobj,
								void* meshobj, bool use_gfx, bool use_phys)=0;
	std::vector<SCA_DebugProp*>& GetDebugProperties();
	bool			PropertyInDebugList(class CValue *gameobj, const std::string &name);
	bool			ObjectInDebugList(class CValue *gameobj);
	void			RemoveAllDebugProperties();
	void			AddDebugProperty(class CValue* debugprop, const std::string &name);
	void			RemoveDebugProperty(class CValue *gameobj, const std::string &name);
	void			RemoveObjectDebugProperties(class CValue* gameobj);
};

#endif  /* __SCA_ISCENE_H__ */
