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

/** \file gameengine/Converter/BL_ShapeDeformer.cpp
 *  \ingroup bgeconv
 */

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#include "MEM_guardedalloc.h"
#include "BL_ShapeDeformer.h"
#include <string>
#include "RAS_IPolygonMaterial.h"
#include "RAS_Mesh.h"

#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_action_types.h"
#include "DNA_key_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "BKE_armature.h"
#include "BKE_action.h"
#include "BKE_global.h"
#include "BKE_main.h"
#include "BKE_key.h"
#include "BKE_fcurve.h"
#include "BKE_ipo.h"
#include "BKE_library.h"

extern "C" {
	#include "BKE_lattice.h"
	#include "BKE_animsys.h"
}

#include "BLI_blenlib.h"
#include "BLI_math.h"

BL_ShapeDeformer::BL_ShapeDeformer(KX_GameObject *gameobj,
                                   Object *bmeshobj_old,
                                   Object *bmeshobj_new,
                                   RAS_Mesh *mesh,
                                   BL_ArmatureObject *arma)
	:BL_SkinDeformer(gameobj, bmeshobj_old, bmeshobj_new, mesh, arma),
	m_useShapeDrivers(false),
	m_lastShapeUpdate(-1)
{
	m_key = m_bmesh->key ? BKE_key_copy(G.main, m_bmesh->key) : nullptr;
}

BL_ShapeDeformer::~BL_ShapeDeformer()
{
	if (m_key) {
		BKE_libblock_free(G.main, m_key);
		m_key = nullptr;
	}
}

bool BL_ShapeDeformer::LoadShapeDrivers(KX_GameObject *parent)
{
	// Only load shape drivers if we have a key
	if (!m_key) {
		m_useShapeDrivers = false;
		return false;
	}

	// Fix drivers since BL_ArmatureObject makes copies
	if (m_armobj && m_key->adt) {
		FCurve *fcu;

		for (fcu = (FCurve *)m_key->adt->drivers.first; fcu; fcu = (FCurve *)fcu->next) {

			DriverVar *dvar;
			for (dvar = (DriverVar *)fcu->driver->variables.first; dvar; dvar = (DriverVar *)dvar->next) {
				DRIVER_TARGETS_USED_LOOPER(dvar)
				{
					if (dtar->id) {
						if ((Object *)dtar->id == m_armobj->GetOrigArmatureObject()) {
							dtar->id = (ID *)m_armobj->GetArmatureObject();
						}
					}
				}
				DRIVER_TARGETS_LOOPER_END
			}
		}
	}

	// This used to check if we had drivers from this armature,
	// now we just assume we want to use shape drivers
	// and let the animsys handle things.
	m_useShapeDrivers = true;

	return true;
}

bool BL_ShapeDeformer::ExecuteShapeDrivers()
{
	if (m_useShapeDrivers) {
		// We don't need an actual time, just use 0
		BKE_animsys_evaluate_animdata(nullptr, &m_key->id, m_key->adt, 0.f, ADT_RECALC_DRIVERS);

		m_bDynamic = true;
		return true;
	}
	return false;
}

void BL_ShapeDeformer::Update(unsigned short reason)
{
	bool shapeUpdate = (reason & UPDATE_SHAPE);
	bool skinUpdate = (reason & UPDATE_SKIN);

	if (ExecuteShapeDrivers()) {
		shapeUpdate = true;
	}

	/* See if the object shape has changed */
	if (shapeUpdate) {
		/* the key coefficient have been set already, we just need to blend the keys */
		Object *blendobj = m_gameobj->GetBlenderObject();

		/* we will blend the key directly in m_transverts array: it is used by armature as the start position */
		/* m_key can be nullptr in case of Modifier deformer */
		if (m_key) {
			WeightsArrayCache cache = {0, nullptr};
			float **per_keyblock_weights;

			/* store verts locally */
			VerifyStorage();

			per_keyblock_weights = BKE_keyblock_get_per_block_weights(blendobj, m_key, &cache);
			BKE_key_evaluate_relative(0, m_bmesh->totvert, m_bmesh->totvert, (char *)(float *)m_transverts.data(),
			                          m_key, nullptr, per_keyblock_weights, 0); /* last arg is ignored */
			BKE_keyblock_free_per_block_weights(m_key, per_keyblock_weights, &cache);

			m_bDynamic = true;
		}

		// Don't release the weight array as in Blender, it will most likely be reusable on next frame
		// The weight array are ultimately deleted when the skin mesh is destroyed

		/* Update the current frame */
		m_lastShapeUpdate = m_lastFrame;

		// As we have changed, the mesh, the skin deformer must update as well.
		// This will force the update
		skinUpdate = (m_armobj != nullptr);
	}
	// check for armature deform
	if (skinUpdate) {
		BL_SkinDeformer::UpdateInternal(shapeUpdate && m_bDynamic);
	}
	else if (shapeUpdate && m_bDynamic) {
		UpdateTransverts();
	}
}

unsigned short BL_ShapeDeformer::NeedUpdate() const
{
	return (((m_lastShapeUpdate != m_lastFrame) ? UPDATE_SHAPE : 0) | BL_SkinDeformer::NeedUpdate());
}

Key *BL_ShapeDeformer::GetKey()
{
	return m_key;
}

bool BL_ShapeDeformer::GetShape(std::vector<float> &shape) const
{
	shape.clear();
	// this check is normally superfluous: a shape deformer can only be created if the mesh
	// has relative keys
	if (m_key && m_key->type == KEY_RELATIVE) {
		for (KeyBlock *kb = (KeyBlock *)m_key->block.first; kb; kb = (KeyBlock *)kb->next) {
			shape.push_back(kb->curval);
		}
	}
	return !shape.empty();
}
