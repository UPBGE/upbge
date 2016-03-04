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
 *
 * Le principe de cette classe est de supprimer les erreurs d'un mauvais lod en début de jeu et de pouvoir 
 * pendant la jeu changer de mesh de LOD, par exemple LOD d'une maison vers LOD d'une maison detruite
 */

#include <vector>

class RAS_MeshObject;
class KX_Scene;
class KX_BlenderSceneConverter;
struct Object;

struct KX_LodLevel
{
	float distance;
	float hysteresis;
	unsigned short level;
	unsigned short flags;
	RAS_MeshObject* meshobj;

	enum {
		USE_HYST = (1 << 0),
	};
};

class KX_LodLevels
{
private:
	std::vector<KX_LodLevel> m_lodLevelList;

	float GetHysteresis(KX_Scene *scene, unsigned short level);

public:
	KX_LodLevels(Object *ob, KX_Scene *scene, KX_BlenderSceneConverter *converter, bool libloading);
	virtual ~KX_LodLevels();

	const KX_LodLevel& GetDistance2ToLodLevel(KX_Scene *scene, unsigned short previouslod, float distance2);

	inline bool Empty() const
	{
		return m_lodLevelList.empty();
	}
};
