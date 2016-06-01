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

#include "KX_LodManager.h"
#include "KX_LodLevel.h"
#include "KX_Scene.h"
#include "BL_BlenderDataConversion.h"
#include "DNA_object_types.h"
#include "BLI_listbase.h"
#include "BLI_math.h"

KX_LodManager::KX_LodManager(Object *ob, KX_Scene* scene, KX_BlenderSceneConverter* converter, bool libloading)
	:m_refcount(1),
	m_distanceScale(1.0f)
{
	if (BLI_listbase_count_ex(&ob->lodlevels, 2) > 1) {
		LodLevel *lod = (LodLevel*)ob->lodlevels.first;
		Mesh* lodmesh = (Mesh*)ob->data;
		Object* lodmatob = ob;
		unsigned short level = 0;

		for (; lod; lod = lod->next) {
			if (!lod->source || lod->source->type != OB_MESH) {
				continue;
			}
			unsigned short flag = ((lod->flags & OB_LOD_USE_HYST) != 0);

			if (lod->flags & OB_LOD_USE_MESH) {
				lodmesh = (Mesh*)lod->source->data;
			}

			if (lod->flags & OB_LOD_USE_MAT) {
				lodmatob = lod->source;
			}
			KX_LodLevel *lodLevel = new KX_LodLevel(lod->distance, lod->obhysteresis, level++,
				BL_ConvertMesh(lodmesh, lodmatob, scene, converter, libloading), flag);

			m_lodLevelList.push_back(lodLevel);
		}
	}
}

KX_LodManager::~KX_LodManager()
{
	for (int i = 0; i < m_lodLevelList.size(); i++) {
		if (m_lodLevelList[i]) {
			free(m_lodLevelList[i]);
			m_lodLevelList[i] = NULL;
		}
	}
}

float KX_LodManager::GetHysteresis(KX_Scene *scene, unsigned short level)
{
	if (!scene->IsActivedLodHysteresis()) {
		return 0.0f;
	}

	KX_LodLevel *lod = m_lodLevelList[level];
	KX_LodLevel *lodnext = m_lodLevelList[level + 1];

	float hysteresis = 0.0f;
	// if exists, LoD level hysteresis will override scene hysteresis
	if (lodnext->GetFlag() & KX_LodLevel::USE_HYST) {
		hysteresis = lodnext->GetHysteresis();
	}
	else {
		hysteresis = scene->GetLodHysteresisValue() / 100.0f;
	}
	return MT_abs(lodnext->GetDistance() - lod->GetDistance()) * hysteresis;
}

KX_LodLevel *KX_LodManager::GetLevel(KX_Scene *scene, unsigned short previouslod, float distance)
{
	unsigned short level = 0;
	unsigned short count = m_lodLevelList.size();
	distance *= m_distanceScale;

	while (level < count) {
		if (level == (count - 1)) {
			break;
		}
		else if (level == previouslod || level == (previouslod + 1)) {
			const float hystvariance = GetHysteresis(scene, level);
			const float newdistance = m_lodLevelList[level + 1]->GetDistance() + hystvariance;
			if (newdistance > distance) {
				break;
			}
		}
		else if (level == (previouslod - 1)) {
			const float hystvariance = GetHysteresis(scene, level);
			const float newdistance = m_lodLevelList[level + 1]->GetDistance() - hystvariance;
			if (newdistance > distance) {
				break;
			}
		}
		++level;
	}
	return m_lodLevelList[level];
}

#ifdef WITH_PYTHON

PyTypeObject KX_LodManager::Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"KX_LodManager",
	sizeof(PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&CValue::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_LodManager::Methods[] = {
	//KX_PYMETHODTABLE(KX_LodManager, get...),
	{ NULL, NULL } //Sentinel
};

PyAttributeDef KX_LodManager::Attributes[] = {
	KX_PYATTRIBUTE_RO_FUNCTION("lodLevel", KX_LodManager, pyattr_get_lodlevels),
	KX_PYATTRIBUTE_FLOAT_RW("distanceScale", 0.0f, FLT_MAX, KX_LodManager, m_distanceScale),
	{ NULL }    //Sentinel
};

PyObject *KX_LodManager::pyattr_get_lodlevels(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LodManager* self = static_cast<KX_LodManager*>(self_v);
	PyObject *levelList = PyList_New(self->m_lodLevelList.size());
	if (self->m_lodLevelList.size() != 0) {
		for (int i = 0; i < self->m_lodLevelList.size(); i++) {
			PyList_SET_ITEM(levelList, i, self->m_lodLevelList[i]->GetProxy());
		}
	}

	return levelList;
}

#endif //WITH_PYTHON
