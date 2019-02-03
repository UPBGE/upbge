/*
 * Set or remove an objects parent
 *
 *
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

 /** \file gameengine/Ketsji/KX_ModifierActuator.cpp
  *  \ingroup ketsji
  */


#include "KX_ModifierActuator.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"

#include "EXP_PyObjectPlus.h"

extern "C" {
#  include "BKE_modifier.h"
#  include "depsgraph/DEG_depsgraph.h"
#  include "DNA_modifier_types.h"
#  include "windowmanager/WM_types.h"
}

  /* ------------------------------------------------------------------------- */
  /* Native functions                                                          */
  /* ------------------------------------------------------------------------- */

KX_ModifierActuator::KX_ModifierActuator(SCA_IObject *gameobj,
	bool activated)
	: SCA_IActuator(gameobj, KX_ACT_MODIFIER),
	m_activated(activated)
{
}



KX_ModifierActuator::~KX_ModifierActuator()
{
}



CValue* KX_ModifierActuator::GetReplica()
{
	KX_ModifierActuator* replica = new KX_ModifierActuator(*this);
	// replication just copy the m_base pointer => common random generator
	replica->ProcessReplica();
	return replica;
}

void KX_ModifierActuator::ProcessReplica()
{
	SCA_IActuator::ProcessReplica();
}

bool KX_ModifierActuator::Update()
{
	bool bNegativeEvent = IsNegativeEvent();
	RemoveAllEvents();

	if (bNegativeEvent)
		return false; // do nothing on negative events

	KX_GameObject *gameobj = (KX_GameObject *)GetParent();
	if (!gameobj->IsStatic()) {
		Object *ob = gameobj->GetBlenderObject();
		for (ModifierData *md = (ModifierData *)ob->modifiers.first; md; md = (ModifierData *)md->next) {
			if ((ModifierType)md->type == eModifierType_Curve) {
				DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
			}
		}
	}

	return m_activated;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject KX_ModifierActuator::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_ModifierActuator",
	sizeof(PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0,0,0,0,0,0,0,0,0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0,0,0,0,0,0,0,
	Methods,
	0,
	0,
	&SCA_IActuator::Type,
	0,0,0,0,0,0,
	py_base_new
};

PyMethodDef KX_ModifierActuator::Methods[] = {
	{nullptr,nullptr} //Sentinel
};

PyAttributeDef KX_ModifierActuator::Attributes[] = {
	KX_PYATTRIBUTE_BOOL_RW("activated", KX_ModifierActuator, m_activated),
	KX_PYATTRIBUTE_NULL	//Sentinel
};

#endif // WITH_PYTHON

/* eof */
