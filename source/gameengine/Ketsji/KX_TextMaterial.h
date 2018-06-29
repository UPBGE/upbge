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

/** \file KX_TextMaterial.h
 *  \ingroup ketsji
 *  \brief Fake material used for all text objects.
 */

#ifndef __KX_TEXTMATERIAL_H__
#define __KX_TEXTMATERIAL_H__

#include "RAS_IMaterial.h"

class KX_TextMaterial : public RAS_IMaterial
{
public:
	KX_TextMaterial();
	virtual ~KX_TextMaterial();

	virtual void Prepare(RAS_Rasterizer *rasty, unsigned short viewportIndex);
	virtual void Activate(RAS_Rasterizer *rasty);
	virtual void Desactivate(RAS_Rasterizer *rasty);
	virtual void ActivateInstancing(RAS_Rasterizer *rasty, RAS_InstancingBuffer *buffer);
	virtual void DesactivateInstancing();
	virtual void ActivateMeshUser(RAS_MeshUser *meshUser, RAS_Rasterizer *rasty, const mt::mat3x4& camtrans);

	virtual const std::string GetTextureName() const;
	virtual Material *GetBlenderMaterial() const;
	virtual Scene *GetBlenderScene() const;
	virtual SCA_IScene *GetScene() const;
	virtual bool UseInstancing() const;
	virtual void ReloadMaterial();

	virtual void UpdateIPO(const mt::vec4 &rgba, const mt::vec3 &specrgb, float hard, float spec, float ref,
						   float emit, float ambient, float alpha, float specalpha);

	virtual const RAS_AttributeArray::AttribList GetAttribs(const RAS_Mesh::LayersInfo& layersInfo) const;
	virtual RAS_InstancingBuffer::Attrib GetInstancingAttribs() const;

	static KX_TextMaterial *GetSingleton();
};

#endif  // __KX_TEXTMATERIAL_H__
