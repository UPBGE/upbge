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

/** \file BL_ActionActuator.h
 *  \ingroup bgeconv
 */

#ifndef __BL_ACTIONACTUATOR_H__
#define __BL_ACTIONACTUATOR_H__

#include "SCA_IActuator.h"
#include "DNA_actuator_types.h"
#include "BL_Action.h" // For BL_Action::PlayMode.

class BL_ActionActuator : public SCA_IActuator  
{
public:
	Py_Header
	BL_ActionActuator(SCA_IObject* gameobj,
						const std::string& propname,
						const std::string& framepropname,
						float starttime,
						float endtime,
						const std::string& actionName,
						short	playtype,
						short	blend_mode,
						short	blendin,
						short	priority,
						short	layer,
						float	layer_weight,
						short	ipo_flags,
						short	end_reset);

	virtual ~BL_ActionActuator();
	virtual	bool Update(double curtime);
	virtual EXP_Value* GetReplica();
	virtual void ProcessReplica();
	
	void SetLocalTime(float curtime);
	void ResetStartTime(float curtime);
	
	virtual void DecLink();

	bool Play(KX_GameObject *obj, float start, float end, short mode);

#ifdef WITH_PYTHON

	static PyObject*	pyattr_get_action(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int			pyattr_set_action(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject*	pyattr_get_use_continue(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int			pyattr_set_use_continue(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject*	pyattr_get_frame(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int			pyattr_set_frame(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);

	static int CheckType(EXP_PyObjectPlus *self, const PyAttributeDef*)
	{
		BL_ActionActuator* act = reinterpret_cast<BL_ActionActuator*>(self);

		switch (act->m_playtype) {
			case ACT_ACTION_PLAY:
			case ACT_ACTION_PINGPONG:
			case ACT_ACTION_FLIPPER:
			case ACT_ACTION_LOOP_STOP:
			case ACT_ACTION_LOOP_END:
			case ACT_ACTION_FROM_PROP:
				return 0;
			default:
				PyErr_SetString(PyExc_ValueError, "Action Actuator, invalid play type supplied");
				return 1;
		}
	}
#endif  /* WITH_PYTHON */
	
protected:
	int		m_flag;
	/** The frame this action starts */
	float	m_startframe;
	/** The frame this action ends */
	float	m_endframe;
	/** The current time of the action */
	float	m_localtime;
	float	m_blendin;
	float	m_blendstart;
	float	m_layer_weight;
	short	m_playtype;
	short   m_blendmode;
	short	m_priority;
	short	m_layer;
	short	m_ipo_flags;
	std::string m_actionName;
	std::string	m_propname;
	std::string	m_framepropname;
};

enum {
	ACT_FLAG_ACTIVE		= 1<<1,
	ACT_FLAG_CONTINUE	= 1<<2,
	ACT_FLAG_PLAY_END	= 1<<3,
};

#endif

