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

#include "EXP_Value.h"
#include "RAS_MeshObject.h"

class KX_LodLevel : public PyObjectPlus
{
	Py_Header

private:

	float m_distance;
	float m_initialDistance;
	float m_hysteresis;
	unsigned short m_level;
	unsigned short m_flags;
	RAS_MeshObject *m_meshobj;

public:
	KX_LodLevel();
	KX_LodLevel(float distance, float hysteresis, unsigned short level, RAS_MeshObject *meshobj, unsigned short flag);
	virtual ~KX_LodLevel();

	float GetDistance() {
		return m_distance;
	}
	void SetDistance(float dist) {
		m_distance = dist;
	}
	float GetInitialDistance() {
		return m_initialDistance;
	}
	float GetHysteresis() {
		return m_hysteresis;
	}
	unsigned short GetLevel() {
		return m_level;
	}
	unsigned short GetFlag() {
		return m_flags;
	}
	RAS_MeshObject *GetMesh() {
		return m_meshobj;
	}
	enum {
		// Use custom hysteresis for this level.
		USE_HYST = (1 << 0),
	};

#ifdef WITH_PYTHON

	static PyObject *pyattr_get_mesh_name(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_use_hysteresis(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);

	//KX_PYMETHOD_DOC(KX_LodLevel, ...);

#endif //WITH_PYTHON

};
