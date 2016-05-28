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

#include "KX_Lod.h"
#include "KX_Scene.h"
#include "BL_BlenderDataConversion.h"
#include "DNA_object_types.h"
#include "BLI_listbase.h"

KX_LodList::KX_LodList(Object *ob, KX_Scene* scene, KX_BlenderSceneConverter* converter, bool libloading)
	:m_refcount(1),
	m_lodListName(STR_String("lodlist"))
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

			Level lodLevel;

			if (lod->flags & OB_LOD_USE_MESH) {
				lodmesh = (Mesh*)lod->source->data;
			}

			if (lod->flags & OB_LOD_USE_MAT) {
				lodmatob = lod->source;
			}

			if (lod->flags & OB_LOD_USE_HYST) {
				lodLevel.flags |= Level::USE_HYST;
			}

			lodLevel.level = level++;
			lodLevel.meshobj = BL_ConvertMesh(lodmesh, lodmatob, scene, converter, libloading);
			lodLevel.distance = lod->distance;
			lodLevel.hysteresis = lod->obhysteresis;
			m_lodLevelList.push_back(lodLevel);
		}
	}
}

KX_LodList::~KX_LodList()
{
}

float KX_LodList::GetHysteresis(KX_Scene *scene, unsigned short level)
{
	if (!scene->IsActivedLodHysteresis()) {
		return 0.0f;
	}

	const Level& lod = m_lodLevelList[level];
	const Level& lodnext = m_lodLevelList[level + 1];

	float hysteresis = 0.0f;
	// if exists, LoD level hysteresis will override scene hysteresis
	if (lodnext.flags & Level::USE_HYST) {
		hysteresis = lodnext.hysteresis;
	}
	else {
		hysteresis = scene->GetLodHysteresisValue() / 100.0f;
	}
	return MT_abs(lodnext.distance - lod.distance) * hysteresis;
}

// stuff for cvalue related things
CValue *KX_LodList::Calc(VALUE_OPERATOR op, CValue *val)
{
	return NULL;
}

CValue *KX_LodList::CalcFinal(VALUE_DATA_TYPE dtype, VALUE_OPERATOR op, CValue *val)
{
	return NULL;
}

const STR_String &KX_LodList::GetText()
{
	return GetName();
}

double KX_LodList::GetNumber()
{
	return -1.0;
}

STR_String &KX_LodList::GetName()
{
	return m_lodListName;
}

void KX_LodList::SetName(const char *name)
{
}

CValue *KX_LodList::GetReplica()
{
	return NULL;
}

const KX_LodList::Level& KX_LodList::GetLevel(KX_Scene *scene, unsigned short previouslod, float distance2)
{
	unsigned short level = 0;
	unsigned short count = m_lodLevelList.size();
	while (level < count) {
		if (level == (count - 1)) {
			break;
		}
		else if (level == previouslod || level == (previouslod + 1)) {
			const float hystvariance = GetHysteresis(scene, level);
			const float newdistance = m_lodLevelList[level + 1].distance + hystvariance;
			if (newdistance * newdistance > distance2) {
				break;
			}
		}
		else if (level == (previouslod - 1)) {
			const float hystvariance = GetHysteresis(scene, level);
			const float newdistance = m_lodLevelList[level + 1].distance - hystvariance;
			if (newdistance * newdistance > distance2) {
				break;
			}
		}
		++level;
	}
	return m_lodLevelList[level];
}

#ifdef WITH_PYTHON

PyTypeObject KX_LodList::Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"KX_LodList",
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

PyMethodDef KX_LodList::Methods[] = {
	KX_PYMETHODTABLE(KX_LodList, getLevelMeshName),
	{ NULL, NULL } //Sentinel
};

PyAttributeDef KX_LodList::Attributes[] = {
	//KX_PYATTRIBUTE_RW_FUNCTION("", KX_LodList, pyattr_get_, pyattr_set_),
	{ NULL }    //Sentinel
};

KX_PYMETHODDEF_DOC(KX_LodList, getLevelMeshName, "getLevelMeshName(levelIndex)")
{
	int index;
	if (!PyArg_ParseTuple(args, "i:index", &index)) {
		PyErr_SetString(PyExc_ValueError, "KX_LodList.getLevelMeshName(levelIndex): KX_LodList, expected an int.");
		return NULL;
	}
	if (index < 0 || index > m_lodLevelList.size() - 1 || m_lodLevelList.size() == 0) {
		PyErr_SetString(PyExc_ValueError, "KX_LodList.getLevelMeshName(levelIndex): KX_LodList, expected an int in range len(lod levels list).");
		return NULL;
	}
	Level level = m_lodLevelList[index];
	RAS_MeshObject *rasmesh = level.meshobj;
	STR_String name = rasmesh->GetName();
	return PyUnicode_FromString(name);
}

#endif //WITH_PYTHON
