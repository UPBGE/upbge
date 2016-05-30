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

#include "KX_LodLevel.h"

KX_LodLevel::KX_LodLevel()
{
}

KX_LodLevel::KX_LodLevel(float distance, float hysteresis, unsigned short level, RAS_MeshObject *meshobj, unsigned short flag)
	:m_distance(distance),
	m_hysteresis(hysteresis),
	m_level(level),
	m_meshobj(meshobj),
	m_flags(flag)
{
	m_initialDistance = distance;
}

KX_LodLevel::~KX_LodLevel()
{
}

#ifdef WITH_PYTHON

PyTypeObject KX_LodLevel::Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"KX_LodLevel",
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

PyMethodDef KX_LodLevel::Methods[] = {
	//KX_PYMETHODTABLE(KX_LodLevel, getLevelMeshName),
	{ NULL, NULL } //Sentinel
};

PyAttributeDef KX_LodLevel::Attributes[] = {
	KX_PYATTRIBUTE_RO_FUNCTION("meshName", KX_LodLevel, pyattr_get_mesh_name),
	KX_PYATTRIBUTE_RO_FUNCTION("useHysteresis", KX_LodLevel, pyattr_get_use_hysteresis),
	KX_PYATTRIBUTE_FLOAT_RW("distance", 0.0f, 99999999.0f, KX_LodLevel, m_distance),
	KX_PYATTRIBUTE_FLOAT_RW("hysteresis", 0.0f, 100.0f, KX_LodLevel, m_hysteresis),
	{ NULL }    //Sentinel
};

PyObject *KX_LodLevel::pyattr_get_mesh_name(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LodLevel *self = static_cast<KX_LodLevel *>(self_v);
	return PyUnicode_FromString(self->m_meshobj->GetName());
}

PyObject *KX_LodLevel::pyattr_get_use_hysteresis(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_LodLevel *self = static_cast<KX_LodLevel *>(self_v);
	int useHysteresis = self->GetFlag();
	return PyBool_FromLong(useHysteresis);
}

#endif //WITH_PYTHON
