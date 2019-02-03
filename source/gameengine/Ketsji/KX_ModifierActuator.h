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

 /** \file KX_ModifierActuator.h
  *  \ingroup ketsji
  *  \brief Set or remove an objects parent
  */

#ifndef __KX_MODIFIERACTUATOR_H__
#define __KX_MODIFIERACTUATOR_H__

#include "SCA_IActuator.h"
#include "SCA_LogicManager.h"

class KX_ModifierActuator : public SCA_IActuator
{
	Py_Header

	bool m_activated;
	/** Object to set as parent */
	SCA_IObject *m_ob;



public:

	KX_ModifierActuator(class SCA_IObject *gameobj,
		bool activated);
	virtual ~KX_ModifierActuator();
	virtual bool Update();

	virtual CValue* GetReplica();
	virtual void ProcessReplica();

#ifdef WITH_PYTHON

	/* --------------------------------------------------------------------- */
	/* Python interface ---------------------------------------------------- */
	/* --------------------------------------------------------------------- */

#endif  /* WITH_PYTHON */

}; /* end of class KX_ModifierActuator : public SCA_IActuator */

#endif  /* __KX_ModifierActuator_H__ */
