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

MTex* getMTexFromMaterial(Material *mat, int index)
{
	if (mat && (index >= 0) && (index < MAX_MTEX)) {
		return mat->mtex[index];
	}
	else {
		return NULL;
	}
}

BL_Material::BL_Material()
{
	Initialize();
}

void BL_Material::Initialize()
{
	rgb[0] = 0xFFFFFFFFL;
	rgb[1] = 0xFFFFFFFFL;
	rgb[2] = 0xFFFFFFFFL;
	rgb[3] = 0xFFFFFFFFL;
	ras_mode = 0;
	tile = 0;
	matname = "NoMaterial";
	matcolor[0] = 0.5f;
	matcolor[1] = 0.5f;
	matcolor[2] = 0.5f;
	matcolor[3] = 0.5f;
	speccolor[0] = 1.f;
	speccolor[1] = 1.f;
	speccolor[2] = 1.f;
	alphablend = 0;
	hard = 50.f;
	spec_f = 0.5f;
	alpha = 1.f;
	specalpha = 1.0f;
	emit = 0.f;
	material = 0;
	memset(&mtexpoly, 0, sizeof(mtexpoly));
	amb=0.5f;
	num_enabled = 0;

	int i;

	for (i = 0; i < MAXTEX; i++) // :(
	{
		texname[i] = "NULL";
		tilexrep[i] = 1;
		tileyrep[i] = 1;
		color_blend[i] = 1.f;
		img[i] = 0;
		cubemap[i] = 0;
	}
}
