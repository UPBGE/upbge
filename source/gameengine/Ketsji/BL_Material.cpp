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
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/BL_Material.cpp
 *  \ingroup ketsji
 */

#include "BL_Material.h"
#include "DNA_material_types.h"
#include "DNA_texture_types.h"
#include "DNA_image_types.h"
#include "DNA_mesh_types.h"
#include "IMB_imbuf_types.h"
#include "IMB_imbuf.h"

BL_Material::BL_Material()
{
	Initialize();
}

void BL_Material::Initialize()
{
	ras_mode = 0;
	matname = "NoMaterial";
	alphablend = 0;
	material = 0;
	memset(&mtexpoly, 0, sizeof(mtexpoly));
}
