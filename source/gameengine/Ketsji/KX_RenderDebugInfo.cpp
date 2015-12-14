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

/** \file gameengine/Ketsji/KX_RenderDebugInfo.cpp
 *  \ingroup ketsji
 */

#include "KX_RenderDebugInfo.h"
#include "KX_GameObject.h"
#include "KX_Scene.h"

#include "RAS_MeshObject.h"
#include "RAS_Deformer.h"

#include "EXP_ListValue.h"

// Use extern "C" cause of the C++ mangling.
extern "C" {
	#include "BKE_cdderivedmesh.h"
}

const char KX_RenderDebugInfo::m_infoLabels[KX_RenderDebugInfo::INFO_NUM_CATEGORIES][20] = {
	"Polygon Count:", // INFO_POLYGON_COUNT
	"Vertex Count:", // INFO_VERTEX_COUNT
	"Mesh Count:", // INFO_MESH_COUNT
	"Light Count:" // INFO_LIGHT_COUNT
};

KX_RenderDebugInfo::KX_RenderDebugInfo()
{
	ResetAllInfos();
}

KX_RenderDebugInfo::~KX_RenderDebugInfo()
{
}

void KX_RenderDebugInfo::Update(KX_Scene *scene)
{
	CListValue *objectList = scene->GetObjectList();
	for (CListValue::iterator it = objectList->GetBegin(); it != objectList->GetEnd(); ++it) {
		KX_GameObject *gameobj = (KX_GameObject *)*it;

		/* check GetCulled also check GetVisible, cause of a invisble object is 
		 * considerate as culled.
		 */
		if (!gameobj->GetCulled() && gameobj->GetMeshCount() != 0) {
			for (unsigned int m = 0; m < gameobj->GetMeshCount(); ++m) {
				unsigned int numverts;
				unsigned int numpolys;

				/* If the game object has a deformer and this deformer has a
				 * derived mesh, the poly and vert count is take from it.
				 */
				RAS_Deformer *deformer = gameobj->GetDeformer();
				if (deformer && deformer->GetFinalMesh()) {
					DerivedMesh *dm = deformer->GetFinalMesh();
					numpolys = dm->getNumTessFaces(dm);
					numverts = dm->getNumVerts(dm);
				}
				// Else get vert and poly count in the normal way.
				else {
					RAS_MeshObject *meshobj = gameobj->GetMesh(m);
					numpolys = meshobj->NumPolygons();
					numverts = meshobj->m_sharedvertex_map.size();
				}

				m_infos[KX_RenderDebugInfo::INFO_POLYGON_COUNT] += numpolys;
				m_infos[KX_RenderDebugInfo::INFO_VERTEX_COUNT] += numverts;
				m_infos[KX_RenderDebugInfo::INFO_MESH_COUNT]++;
			}
		}
		if (gameobj->GetGameObjectType() == SCA_IObject::OBJ_LIGHT && gameobj->GetVisible()) {
			m_infos[KX_RenderDebugInfo::INFO_LIGHT_COUNT]++;
		}
	}
}

unsigned int KX_RenderDebugInfo::GetInfoValue(unsigned int category) const
{
	return m_infos[category];
}

const char *const KX_RenderDebugInfo::GetInfoName(unsigned int category) const
{
	return m_infoLabels[category];
}

void KX_RenderDebugInfo::ResetAllInfos()
{
	for (unsigned int i = 0; i < INFO_NUM_CATEGORIES; ++i) {
		m_infos[i] = 0;
	}
}
