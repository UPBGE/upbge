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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */


#include "KX_BoundingBox.h"
#include "KX_GameObject.h"
#include "KX_PyMath.h"

#include <sstream>

#ifdef WITH_PYTHON

KX_BoundingBox::KX_BoundingBox(KX_GameObject *owner)
	:m_owner(owner),
	m_proxy(owner->GetProxy())
{
}

KX_BoundingBox::~KX_BoundingBox()
{
}

std::string KX_BoundingBox::GetName() const
{
	return "KX_BoundingBox";
}

std::string KX_BoundingBox::GetText() const
{
	if (!IsValidOwner()) {
		return "KX_BoundingBox of invalid object";
	}

	std::stringstream stream;
	stream << "KX_BoundingBox of object " << m_owner->GetName() << ", min: " << GetMin() << ", max: " << GetMax();
	return stream.str();
}

bool KX_BoundingBox::IsValidOwner() const
{
	if (!EXP_PROXY_REF(m_proxy)) {
		PyErr_SetString(PyExc_SystemError, "KX_BoundingBox, " EXP_PROXY_ERROR_MSG);
		return false;
	}
	return true;
}

const mt::vec3& KX_BoundingBox::GetMax() const
{
	// Update AABB to make sure we have the last one.
	m_owner->UpdateBounds(false);
	const SG_BBox& box = m_owner->GetCullingNode().GetAabb();
	return box.GetMax();
}

const mt::vec3& KX_BoundingBox::GetMin() const
{
	// Update AABB to make sure we have the last one.
	m_owner->UpdateBounds(false);
	const SG_BBox& box = m_owner->GetCullingNode().GetAabb();
	return box.GetMin();
}

const mt::vec3 KX_BoundingBox::GetCenter() const
{
	// Update AABB to make sure we have the last one.
	m_owner->UpdateBounds(false);
	const SG_BBox& box = m_owner->GetCullingNode().GetAabb();
	return box.GetCenter();
}

float KX_BoundingBox::GetRadius() const
{
	// Update AABB to make sure we have the last one.
	m_owner->UpdateBounds(false);
	const SG_BBox& box = m_owner->GetCullingNode().GetAabb();
	return box.GetRadius();
}

bool KX_BoundingBox::SetMax(const mt::vec3 &max)
{
	const mt::vec3& min = GetMin();

	if (min.x > max.x || min.y > max.y || min.z > max.z) {
		return false;
	}

	m_owner->SetBoundsAabb(min, max);
	return true;
}

bool KX_BoundingBox::SetMin(const mt::vec3 &min)
{
	const mt::vec3& max = GetMax();

	if (min.x > max.x || min.y > max.y || min.z > max.z) {
		return false;
	}

	m_owner->SetBoundsAabb(min, max);
	return true;
}

PyTypeObject KX_BoundingBox::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_BoundingBox",
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
	&EXP_PyObjectPlus::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_BoundingBox::Methods[] = {
	{nullptr, nullptr} // Sentinel
};

EXP_Attribute KX_BoundingBox::Attributes[] = {
	EXP_ATTRIBUTE_RW_FUNCTION("min", pyattr_get_min, pyattr_set_min),
	EXP_ATTRIBUTE_RW_FUNCTION("max", pyattr_get_max, pyattr_set_max),
	EXP_ATTRIBUTE_RO_FUNCTION("center", pyattr_get_center),
	EXP_ATTRIBUTE_RO_FUNCTION("radius", pyattr_get_radius),
	EXP_ATTRIBUTE_RW_FUNCTION("autoUpdate", pyattr_get_auto_update, pyattr_set_auto_update),
	EXP_ATTRIBUTE_NULL // Sentinel
};

bool KX_BoundingBox::pyattr_check(const EXP_Attribute *attrdef)
{
	if (!IsValidOwner()) {
		attrdef->PrintError(": No game object for this bounding box.");
		return false;
	}

	return true;
}

mt::vec3 KX_BoundingBox::pyattr_get_min()
{
	return GetMin();
}

bool KX_BoundingBox::pyattr_set_min(const mt::vec3& value, const EXP_Attribute *attrdef)
{
	if (!SetMin(value)) {
		attrdef->PrintError(": min bigger than max");
		return false;
	}

	return true;
}

mt::vec3 KX_BoundingBox::pyattr_get_max()
{
	return GetMax();
}

bool KX_BoundingBox::pyattr_set_max(const mt::vec3& value, const EXP_Attribute *attrdef)
{
	if (!SetMax(value)) {
		attrdef->PrintError(": max smaller than min");
		return false;
	}

	return true;
}

mt::vec3 KX_BoundingBox::pyattr_get_center()
{
	return GetCenter();
}

float KX_BoundingBox::pyattr_get_radius()
{
	return GetRadius();
}

bool KX_BoundingBox::pyattr_get_auto_update()
{
	return m_owner->GetAutoUpdateBounds();
}

void KX_BoundingBox::pyattr_set_auto_update(bool value)
{
	m_owner->SetAutoUpdateBounds(value);
}

#endif  // WITH_PYTHON
