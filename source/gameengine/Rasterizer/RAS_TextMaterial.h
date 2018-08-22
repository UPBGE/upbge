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

/** \file RAS_TextMaterial.h
 *  \ingroup ketsji
 *  \brief Fake material used for all text objects.
 */

#ifndef __RAS_TEXTMATERIAL_H__
#define __RAS_TEXTMATERIAL_H__

#include "RAS_IMaterial.h"

class RAS_TextMaterial : public RAS_IMaterial
{
public:
	RAS_TextMaterial();
	virtual ~RAS_TextMaterial();

	virtual void Prepare();

	virtual RAS_IMaterialShader *GetShader(RAS_Rasterizer::DrawType drawingMode) const;
	virtual const std::string GetTextureName() const;
	virtual SCA_IScene *GetScene() const;
	virtual bool UseInstancing() const;
	virtual void ReloadMaterial();

	virtual void UpdateIPO(const mt::vec4 &rgba, const mt::vec3 &specrgb, float hard, float spec, float ref,
						   float emit, float ambient, float alpha, float specalpha);

	static RAS_TextMaterial *GetSingleton();
};

#endif  // __RAS_TEXTMATERIAL_H__
