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
#include <vector>

class KX_Scene;
class KX_BlenderSceneConverter;
class KX_LodLevel;
struct Object;

class KX_LodManager: public PyObjectPlus
{
	Py_Header
public:

private:
	std::vector<KX_LodLevel *> m_lodLevelList;

	/** Get the hysteresis from the level or the scene.
	 * \param scene Scene used to get default hysteresis.
	 * \param level Level index used to get hysteresis.
	 */
	float GetHysteresis(KX_Scene *scene, unsigned short level);

	int m_refcount;

	float m_lodLevelsScale;

public:
	KX_LodManager(Object *ob, KX_Scene *scene, KX_BlenderSceneConverter *converter, bool libloading);
	virtual ~KX_LodManager();

#ifdef WITH_PYTHON

	static PyObject *pyattr_get_lodlevels(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	//static int pyattr_set_(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_lod_levels_scale(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_lod_levels_scale(void *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value);

#endif //WITH_PYTHON

	/** Get lod level cooresponding to distance and previous level.
	 * \param scene Scene used to get default hysteresis.
	 * \param previouslod Previous lod computed by this function before.
	 * \param distance2 Squared distance object to the camera.
	 */
	KX_LodLevel *GetLevel(KX_Scene *scene, unsigned short previouslod, float distance2);

	/// If it returns true, then the lod is useless then.
	inline bool Empty() const
	{
		return m_lodLevelList.empty();
	}

	KX_LodManager *AddRef()
	{
		++m_refcount;
		return this;
	}
	KX_LodManager *Release()
	{
		if (--m_refcount == 0) {
			delete this;
			return NULL;
		}
		return this;
	}
	void SetScale(float scale);
};
