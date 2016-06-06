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

KX_LodManager::KX_LodManager(Object *ob, KX_Scene* scene, KX_BlenderSceneConverter* converter, bool libloading)
	:m_refcount(1),
	m_distanceFactor(1.0f)
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
			unsigned short flag = 0;
			if (lod->flags & OB_LOD_USE_HYST) {
				flag |= KX_LodLevel::USE_HYSTERESIS;
			}

			if (lod->flags & OB_LOD_USE_MESH) {
				lodmesh = (Mesh*)lod->source->data;
				flag |= KX_LodLevel::USE_MESH;
			}

			if (lod->flags & OB_LOD_USE_MAT) {
				lodmatob = lod->source;
				flag |= KX_LodLevel::USE_MATERIAL;
			}
			KX_LodLevel *lodLevel = new KX_LodLevel(lod->distance, lod->obhysteresis, level++,
				BL_ConvertMesh(lodmesh, lodmatob, scene, converter, libloading), flag);

			m_levels.push_back(lodLevel);
		}
	}
}

KX_LodManager::~KX_LodManager()
{
	for (int i = 0; i < m_levels.size(); i++) {
		if (m_levels[i]) {
			delete m_levels[i];
		}
	}
}

float KX_LodManager::GetHysteresis(KX_Scene *scene, unsigned short level)
{
	if (!scene->IsActivedLodHysteresis()) {
		return 0.0f;
	}

	KX_LodLevel *lod = m_levels[level];
	KX_LodLevel *lodnext = m_levels[level + 1];

	float hysteresis = 0.0f;
	// if exists, LoD level hysteresis will override scene hysteresis
	if (lodnext->GetFlag() & KX_LodLevel::USE_HYSTERESIS) {
		hysteresis = lodnext->GetHysteresis();
	}
	else {
		hysteresis = scene->GetLodHysteresisValue() / 100.0f;
	}
	return MT_abs(lodnext->GetDistance() - lod->GetDistance()) * hysteresis;
}

KX_LodLevel *KX_LodManager::GetLevel(KX_Scene *scene, unsigned short previouslod, float distance2)
{
	unsigned short level = 0;
	unsigned short count = m_levels.size();
	distance2 *= (m_distanceFactor * m_distanceFactor);

	while (level < count) {
		if (level == (count - 1)) {
			break;
		}
		else if (level == previouslod || level == (previouslod + 1)) {
			const float hystvariance = GetHysteresis(scene, level);
			const float newdistance = m_levels[level + 1]->GetDistance() + hystvariance;
			if (newdistance * newdistance > distance2) {
				break;
			}
		}
		else if (level == (previouslod - 1)) {
			const float hystvariance = GetHysteresis(scene, level);
			const float newdistance = m_levels[level + 1]->GetDistance() - hystvariance;
			if (newdistance * newdistance > distance2) {
				break;
			}
		}
		++level;
	}
	return m_levels[level];
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
	{NULL, NULL} // Sentinel
};

PyAttributeDef KX_LodManager::Attributes[] = {
	KX_PYATTRIBUTE_RO_FUNCTION("Level", KX_LodManager, pyattr_get_levels),
	KX_PYATTRIBUTE_FLOAT_RW("distanceFactor", 0.0f, FLT_MAX, KX_LodManager, m_distanceFactor),
	{NULL} //Sentinel
};

PyObject *KX_LodManager::pyattr_get_levels(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LodManager* self = static_cast<KX_LodManager*>(self_v);
	PyObject *levelList = PyList_New(self->m_levels.size());
	if (self->m_levels.size() != 0) {
		for (int i = 0; i < self->m_levels.size(); i++) {
			PyList_SET_ITEM(levelList, i, self->m_levels[i]->GetProxy());
		}
	}

	return levelList;
}

bool ConvertPythonToLodManager(PyObject *value, KX_LodManager **object, bool py_none_ok, const char *error_prefix)
{
	if (value == NULL) {
		PyErr_Format(PyExc_TypeError, "%s, python pointer NULL, should never happen", error_prefix);
		*object = NULL;
		return false;
	}

	if (value == Py_None) {
		*object = NULL;

		if (py_none_ok) {
			return true;
		}
		else {
			PyErr_Format(PyExc_TypeError, "%s, expected KX_LodManager, None is invalid", error_prefix);
			return false;
		}
	}

	if (PyObject_TypeCheck(value, &KX_LodManager::Type)) {
		*object = static_cast<KX_LodManager *>BGE_PROXY_REF(value);

		/* sets the error */
		if (*object == NULL) {
			PyErr_Format(PyExc_SystemError, "%s, " BGE_PROXY_ERROR_MSG, error_prefix);
			return false;
		}

		return true;
	}

	*object = NULL;

	if (py_none_ok) {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_LodManager or None", error_prefix);
	}
	else {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_LodManager", error_prefix);
	}

	return false;
}

#endif //WITH_PYTHON
