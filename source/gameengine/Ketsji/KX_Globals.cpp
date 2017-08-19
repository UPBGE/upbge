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

/** \file gameengine/Ketsji/KX_Globals.cpp
 *  \ingroup ketsji
 */

#include "KX_Globals.h"
#include "KX_KetsjiEngine.h"
#include "RAS_Rasterizer.h"

extern "C" {
#  include "BLI_blenlib.h"
}

static KX_KetsjiEngine *g_engine = nullptr;
static KX_Scene *g_scene = nullptr;
static std::string g_mainPath = "";
static std::string g_origPath = "";

void KX_SetActiveEngine(KX_KetsjiEngine *engine)
{
	g_engine = engine;
}

void KX_SetActiveScene(KX_Scene *scene)
{
	g_scene = scene;
}

void KX_SetMainPath(const std::string& path)
{
	char cpath[FILE_MAX];
	BLI_strncpy(cpath, path.c_str(), sizeof(cpath));
	BLI_cleanup_file(nullptr, cpath);
	g_mainPath = std::string(cpath);
}

void KX_SetOrigPath(const std::string& path)
{
	char cpath[FILE_MAX];
	BLI_strncpy(cpath, path.c_str(), sizeof(cpath));
	BLI_cleanup_file(nullptr, cpath);
	g_origPath = std::string(cpath);
}

KX_KetsjiEngine *KX_GetActiveEngine()
{
	return g_engine;
}

KX_Scene *KX_GetActiveScene()
{
	return g_scene;
}

const std::string& KX_GetMainPath()
{
	return g_mainPath;
}

const std::string& KX_GetOrigPath()
{
	return g_origPath;
}

void KX_RasterizerDrawDebugLine(const mt::vec3& from,const mt::vec3& to,const mt::vec4& color)
{
	g_engine->GetRasterizer()->GetDebugDraw(g_scene).DrawLine(from, to, color);
}

void KX_RasterizerDrawDebugCircle(const mt::vec3& center, const float radius, const mt::vec4& color,
                                  const mt::vec3& normal, int nsector)
{
	g_engine->GetRasterizer()->GetDebugDraw(g_scene).DrawCircle(center, radius, color, normal, nsector);
}
