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

/** \file KX_RenderDebugInfo.h
 *  \ingroup ketsji
 */

#ifndef __RENDER_INFO_H__
#define __RENDER_INFO_H__

class KX_Scene;

class KX_RenderDebugInfo
{
public:
	enum {
		INFO_POLYGON_COUNT = 0,
		INFO_VERTEX_COUNT,
		INFO_MESH_COUNT,
		INFO_LIGHT_COUNT,
		INFO_NUM_CATEGORIES,
	} InfoCategories;

	KX_RenderDebugInfo();
	virtual ~KX_RenderDebugInfo();

	/** Extract all render infos from the scene. These info are added to the previous.
	 * \param scene The scene used for render infos.
	 */
	void Update(KX_Scene *scene);
	/** Return the cooresponding value for the given render info category.
	 * \param category The given render info category.
	 * \return The category's value.
	 */
	unsigned int GetInfoValue(unsigned int category) const;
	/** Return the cooresponding name for the given render info category.
	 * \param category The given render info category.
	 * \return The category's name.
	 */
	const char *const GetInfoName(unsigned int category) const;
	/// Set all category's value to zero.
	void ResetAllInfos();

private:
	/// Array of all category's value.
	unsigned int m_infos[INFO_NUM_CATEGORIES];
	/// Array of all category's name, shared by all instances.
	static const char m_infoLabels[INFO_NUM_CATEGORIES][20];
};

#endif // __RENDER_INFO_H__
