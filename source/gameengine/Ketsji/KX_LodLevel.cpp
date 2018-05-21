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
 * Contributor(s): Ulysse Martin, Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/KX_LodLevel.cpp
 *  \ingroup ketsji
 */

#include "KX_LodLevel.h"
#include "KX_Mesh.h"

KX_LodLevel::KX_LodLevel(float distance, float hysteresis, unsigned short level, KX_Mesh *mesh, unsigned short flag)
	:m_distance(distance),
	m_hysteresis(hysteresis),
	m_level(level),
	m_flags(flag),
	m_mesh(mesh)
{
}

KX_LodLevel::~KX_LodLevel()
{
}

std::string KX_LodLevel::GetName() const
{
	return m_mesh->GetName();
}

float KX_LodLevel::GetDistance() const
{
	return m_distance;
}

float KX_LodLevel::GetHysteresis() const
{
	return m_hysteresis;
}

unsigned short KX_LodLevel::GetLevel() const
{
	return m_level;
}

unsigned short KX_LodLevel::GetFlag() const
{
	return m_flags;
}

KX_Mesh *KX_LodLevel::GetMesh() const
{
	return m_mesh;
}

#ifdef WITH_PYTHON

PyTypeObject KX_LodLevel::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_LodLevel",
	sizeof(EXP_PyObjectPlus_Proxy),
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
	&EXP_Value::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_LodLevel::Methods[] = {
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef KX_LodLevel::Attributes[] = {
	EXP_PYATTRIBUTE_RO_FUNCTION("mesh", KX_LodLevel, pyattr_get_mesh),
	EXP_PYATTRIBUTE_SHORT_RO("level", KX_LodLevel, m_level),
	EXP_PYATTRIBUTE_FLOAT_RO("distance", KX_LodLevel, m_distance),
	EXP_PYATTRIBUTE_FLOAT_RO("hysteresis", KX_LodLevel, m_hysteresis),
	EXP_PYATTRIBUTE_RO_FUNCTION("useHysteresis", KX_LodLevel, pyattr_get_use_hysteresis),
	EXP_PYATTRIBUTE_RO_FUNCTION("useMesh", KX_LodLevel, pyattr_get_use_mesh),
	EXP_PYATTRIBUTE_RO_FUNCTION("useMaterial", KX_LodLevel, pyattr_get_use_material),
	EXP_PYATTRIBUTE_NULL  // Sentinel
};

PyObject *KX_LodLevel::pyattr_get_mesh(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_LodLevel *self = static_cast<KX_LodLevel *>(self_v);
	return self->GetMesh()->GetProxy();
}

PyObject *KX_LodLevel::pyattr_get_use_hysteresis(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_LodLevel *self = static_cast<KX_LodLevel *>(self_v);
	return PyBool_FromLong(self->GetFlag() & KX_LodLevel::USE_HYSTERESIS);
}

PyObject *KX_LodLevel::pyattr_get_use_mesh(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_LodLevel *self = static_cast<KX_LodLevel *>(self_v);
	return PyBool_FromLong(self->GetFlag() & KX_LodLevel::USE_MESH);
}

PyObject *KX_LodLevel::pyattr_get_use_material(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_LodLevel *self = static_cast<KX_LodLevel *>(self_v);
	return PyBool_FromLong(self->GetFlag() & KX_LodLevel::USE_MATERIAL);
}

#endif //WITH_PYTHON
