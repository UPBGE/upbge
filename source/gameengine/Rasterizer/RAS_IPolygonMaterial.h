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
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file RAS_IPolygonMaterial.h
 *  \ingroup bgerast
 */

#ifndef __RAS_IPOLYGONMATERIAL_H__
#define __RAS_IPOLYGONMATERIAL_H__

#include "STR_HashedString.h"

#ifdef WITH_CXX_GUARDEDALLOC
#include "MEM_guardedalloc.h"
#endif

class RAS_IRasterizer;
class RAS_MeshSlot;
struct MTexPoly;
struct Material;
struct Image;
struct Scene;
class SCA_IScene;
struct GameSettings;

enum MaterialProps
{
	RAS_MULTITEX = (1 << 0),
	RAS_MULTILIGHT = (1 << 1),
	RAS_BLENDERMAT = (1 << 2),
	RAS_BLENDERGLSL = (1 << 3),
	RAS_CASTSHADOW = (1 << 4),
	RAS_ONLYSHADOW = (1 << 5),
	RAS_OBJECTCOLOR = (1 << 6),
};

/**
 * Polygon Material on which the material buckets are sorted
 */
class RAS_IPolyMaterial
{
protected:
	STR_HashedString m_texturename;
	STR_HashedString m_materialname; // also needed for touchsensor
	int m_tile;
	int m_tilexrep;
	int m_tileyrep;
	int m_drawingmode;
	int m_alphablend;
	bool m_alpha;
	bool m_zsort;
	bool m_light;
	int m_materialindex;

	unsigned int m_polymatid;
	static unsigned int m_newpolymatid;

	unsigned int m_flag;
public:

	// care! these are taken from blender polygonflags, see file DNA_mesh_types.h for #define TF_BILLBOARD etc.
	enum MaterialFlags
	{
		BILLBOARD_SCREENALIGNED = 512, // GEMAT_HALO
		BILLBOARD_AXISALIGNED = 1024, // GEMAT_BILLBOARD
		SHADOW = 2048 // GEMAT_SHADOW
	};

	RAS_IPolyMaterial();
	RAS_IPolyMaterial(const STR_String& texname,
	                  const STR_String& matname,
	                  int materialindex,
	                  int tile,
	                  int tilexrep,
	                  int tileyrep,
	                  int transp,
	                  bool alpha,
	                  bool zsort);
	void Initialize(const STR_String& texname,
	                const STR_String& matname,
	                int materialindex,
	                int tile,
	                int tilexrep,
	                int tileyrep,
	                int transp,
	                bool alpha,
	                bool zsort,
	                bool light,
	                bool image,
	                GameSettings *game);

	virtual ~RAS_IPolyMaterial()
	{
	}

	virtual void Activate(RAS_IRasterizer *rasty) = 0;
	virtual void Desactivate(RAS_IRasterizer *rasty) = 0;
	virtual void ActivateMeshSlot(RAS_MeshSlot *ms, RAS_IRasterizer *rasty) = 0;

	bool IsAlpha() const;
	bool IsZSort() const;
	unsigned int hash() const;
	int GetDrawingMode() const;
	const STR_String& GetMaterialName() const;
	dword GetMaterialNameHash() const;
	const STR_String& GetTextureName() const;
	unsigned int GetFlag() const;
	int GetMaterialIndex() const;

	virtual Material *GetBlenderMaterial() const = 0;
	virtual Image *GetBlenderImage() const = 0;
	virtual MTexPoly *GetMTexPoly() const = 0;
	virtual unsigned int *GetMCol() const = 0;
	virtual Scene *GetBlenderScene() const = 0;
	virtual void ReleaseMaterial() = 0;
	virtual void GetMaterialRGBAColor(unsigned char *rgba) const;
	virtual bool UsesLighting(RAS_IRasterizer *rasty) const;
	virtual bool UsesObjectColor() const;
	virtual bool CastsShadows() const;
	virtual bool OnlyShadow() const;

	/// Overridden by KX_BlenderMaterial
	virtual void Replace_IScene(SCA_IScene *val) = 0;

	/**
	 * \return the equivalent drawing mode for the material settings (equivalent to old TexFace tface->mode).
	 */
	int ConvertFaceMode(struct GameSettings *game, bool image) const;

	/*
	 * PreCalculate texture gen
	 */
	virtual void OnConstruction() = 0;

#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:RAS_IPolyMaterial")
#endif
};

#endif  /* __RAS_IPOLYGONMATERIAL_H__ */
