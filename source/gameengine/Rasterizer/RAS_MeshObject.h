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

/** \file RAS_MeshObject.h
 *  \ingroup bgerast
 */

#ifndef __RAS_MESHOBJECT_H__
#define __RAS_MESHOBJECT_H__

#ifdef _MSC_VER
/* disable the STL warnings ("debug information length > 255") */
#  pragma warning (disable:4786)
#endif

#include <vector>
#include <list>

#include "RAS_MaterialBucket.h"
#include "RAS_MeshMaterial.h"
#include "RAS_Texture.h"
#include "MT_Transform.h"
#include "MT_Vector2.h"
#include <string>

class RAS_Polygon;
class RAS_ITexVert;
struct Mesh;
struct MLoopUV;
struct MLoopCol;

/* RAS_MeshObject is a mesh used for rendering. It stores polygons,
 * but the actual vertices and index arrays are stored in material
 * buckets, referenced by the list of RAS_MeshMaterials. */

class RAS_MeshObject
{
public:
	/** Additionals data stored in mesh layers. These datas can be the colors layer or the
	 * UV map layers. They are used to find attribute's layers index by looking for similar
	 * attribute's names in shader and names of the mesh layers here.
	 */
	struct Layer {
		MLoopUV *uv;
		MLoopCol *color;
		/// The index of the color or uv layer in the vertices.
		unsigned short index;
		/// The name of the color or uv layer used to find corresponding material attributes.
		std::string name;
	};

	typedef std::vector<Layer> LayerList;

	struct LayersInfo {
		LayerList layers;
		// The active color layer index as default.
		unsigned short activeColor;
		// The active uv layer index as default.
		unsigned short activeUv;
	};

private:
	std::string m_name;

	LayersInfo m_layersInfo;

	std::vector<RAS_Polygon *> m_polygons;

	/* polygon sorting */
	struct polygonSlot;
	struct backtofront;
	struct fronttoback;

protected:
	RAS_MeshMaterialList m_materials;
	Mesh *m_mesh;

public:
	// for now, meshes need to be in a certain layer (to avoid sorting on lights in realtime)
	RAS_MeshObject(Mesh *mesh, const LayersInfo& layersInfo);
	virtual ~RAS_MeshObject();

	// materials
	int NumMaterials();
	const std::string GetMaterialName(unsigned int matid);
	const std::string GetTextureName(unsigned int matid);

	RAS_MeshMaterial *GetMeshMaterial(unsigned int matid) const;
	RAS_MeshMaterial *GetMeshMaterialBlenderIndex(unsigned int index);

	// name
	std::string& GetName();

	// original blender mesh
	Mesh *GetMesh()
	{
		return m_mesh;
	}

	// mesh construction
	RAS_MeshMaterial *AddMaterial(RAS_MaterialBucket *bucket, unsigned int index, const RAS_TexVertFormat& format);
	void AddLine(RAS_MeshMaterial *meshmat, unsigned int v1, unsigned int v2);
	virtual RAS_Polygon *AddPolygon(RAS_MeshMaterial *meshmat, int numverts, unsigned int indices[4],
									bool visible, bool collider, bool twoside);
	virtual unsigned int AddVertex(
				RAS_MeshMaterial *meshmat,
				const MT_Vector3& xyz,
				const MT_Vector2 * const uvs,
				const MT_Vector4& tangent,
				const unsigned int *rgba,
				const MT_Vector3& normal,
				const bool flat,
				const unsigned int origindex);

	// vertex and polygon acces
	RAS_IDisplayArray *GetDisplayArray(unsigned int matid) const;
	RAS_ITexVert *GetVertex(unsigned int matid, unsigned int index);
	const float *GetVertexLocation(unsigned int orig_index);

	int NumPolygons();
	RAS_Polygon *GetPolygon(int num) const;

	void EndConversion();

	/// Return the list of blender's layers.
	const LayersInfo& GetLayersInfo() const;

	// polygon sorting by Z for alpha
	void SortPolygons(RAS_IDisplayArray *array, const MT_Transform &transform, unsigned int *indexmap);

	bool HasColliderPolygon();

	// for construction to find shared vertices
	struct SharedVertex
	{
		RAS_IDisplayArray *m_darray;
		int m_offset;
	};

	std::vector<std::vector<SharedVertex> > m_sharedvertex_map;
};

#endif  // __RAS_MESHOBJECT_H__
