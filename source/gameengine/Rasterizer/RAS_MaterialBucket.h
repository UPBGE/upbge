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

/** \file RAS_MaterialBucket.h
 *  \ingroup bgerast
 */

#ifndef __RAS_MATERIALBUCKET_H__
#define __RAS_MATERIALBUCKET_H__

#include "RAS_TexVert.h"
#include "CTR_Map.h"

#include "MT_Transform.h"
#include "MT_Matrix4x4.h"

#include <vector>
#include <list>

class RAS_MaterialBucket;
class RAS_DisplayArrayBucket;
struct DerivedMesh;
class CTR_HashedPtr;
class RAS_Deformer;
class RAS_IPolyMaterial;
class RAS_IRasterizer;
class RAS_MeshObject;

using namespace std;

// Display List Slot
class KX_ListSlot
{
protected:
	int m_refcount;
public:
	KX_ListSlot()
	{
		m_refcount = 1;
	}
	virtual ~KX_ListSlot()
	{
	}
	virtual int Release()
	{
		if (--m_refcount > 0)
			return m_refcount;
		delete this;
		return 0;
	}
	virtual KX_ListSlot *AddRef()
	{
		m_refcount++;
		return this;
	}
	virtual void SetModified(bool mod) = 0;
};

// An array with data used for OpenGL drawing
class RAS_DisplayArray
{
public:
	vector<RAS_TexVert> m_vertex;
	vector<unsigned int> m_index;

	enum {BUCKET_MAX_INDEX = 65535};
	enum {BUCKET_MAX_VERTEX = 65535};
};

// Entry of a RAS_MeshObject into RAS_MaterialBucket
typedef std::vector<RAS_DisplayArray *>  RAS_DisplayArrayList;

class RAS_MeshSlot
{
	friend class RAS_ListRasterizer;
private:
	RAS_DisplayArray *m_displayArray;

public:
	// for rendering
	RAS_MaterialBucket *m_bucket;
	RAS_DisplayArrayBucket *m_displayArrayBucket;
	RAS_MeshObject *m_mesh;
	void *m_clientObj;
	RAS_Deformer *m_pDeformer;
	DerivedMesh *m_pDerivedMesh;
	float *m_OpenGLMatrix;
	// visibility
	bool m_bVisible;
	bool m_bCulled;
	// object color
	bool m_bObjectColor;
	MT_Vector4 m_RGBAcolor;
	// display lists
	KX_ListSlot *m_DisplayList;
	bool m_bDisplayList;
	// joined mesh slots
	RAS_MeshSlot *m_joinSlot;
	MT_Matrix4x4 m_joinInvTransform;
	list<RAS_MeshSlot *> m_joinedSlots;

	RAS_MeshSlot();
	RAS_MeshSlot(const RAS_MeshSlot& slot);
	virtual ~RAS_MeshSlot();

	void init(RAS_MaterialBucket *bucket);

	RAS_DisplayArray *GetDisplayArray();
	void SetDeformer(RAS_Deformer *deformer);

	int AddVertex(const RAS_TexVert& tv);
	void AddPolygonVertex(int offset);

	// optimization
	bool Split(bool force = false);
	bool Join(RAS_MeshSlot *target, MT_Scalar distance);
	bool Equals(RAS_MeshSlot *target);
#ifdef USE_SPLIT
	bool IsCulled();
#else
	bool IsCulled()
	{
		return m_bCulled;
	}
#endif
	void SetCulled(bool culled)
	{
		m_bCulled = culled;
	}

#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:RAS_MeshSlot")
#endif
};

typedef std::vector<RAS_MeshSlot *> RAS_MeshSlotList;

// Used by RAS_MeshObject, to point to it's slots in a bucket
class RAS_MeshMaterial
{
public:
	RAS_MeshSlot *m_baseslot;
	RAS_MaterialBucket *m_bucket;
	/// The material index position in the mesh.
	unsigned int m_index;

	/// the KX_GameObject is used as a key here
	CTR_Map<CTR_HashedPtr, RAS_MeshSlot *> m_slots;

#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:RAS_MeshMaterial")
#endif
};

/** This class is an interface between RAS_MeshSlots and the RAS_DisplayArray.
 * It manages the allocation and deletion of the RAS_DisplayArray.
 * It can have a NULL display array. e.g modifier meshes.
 */
class RAS_DisplayArrayBucket
{
private:
	/// The number of mesh slot using it.
	unsigned int m_refcount;
	/// The parent bucket.
	RAS_MaterialBucket *m_bucket;
	/// The display array = list of vertexes and indexes.
	RAS_DisplayArray *m_displayArray;
	/// The list fo all visible mesh slots to render this frame.
	RAS_MeshSlotList m_activeMeshSlots;

public:
	RAS_DisplayArrayBucket(RAS_MaterialBucket *bucket, RAS_DisplayArray *array);
	~RAS_DisplayArrayBucket();

	/// \section Reference Count Management.
	RAS_DisplayArrayBucket *AddRef();
	RAS_DisplayArrayBucket *Release();
	unsigned int GetRefCount() const;

	/// \section Replication
	RAS_DisplayArrayBucket *GetReplica();
	void ProcessReplica();

	RAS_DisplayArray *GetDisplayArray() const;

	/// \section Active Mesh Slots Management.
	void ActivateMesh(RAS_MeshSlot *slot);
	RAS_MeshSlotList& GetActiveMeshSlots();
	unsigned int GetNumActiveMeshSlots() const;
	/// Remove all mesh slots from the list.
	void RemoveActiveMeshSlots();
};

typedef std::vector<RAS_DisplayArrayBucket *> RAS_DisplayArrayBucketList;

/* Contains a list of display arrays with the same material,
 * and a mesh slot for each mesh that uses display arrays in
 * this bucket */

class RAS_MaterialBucket
{
public:
	RAS_MaterialBucket(RAS_IPolyMaterial *mat);
	virtual ~RAS_MaterialBucket();

	// Material Properties
	RAS_IPolyMaterial *GetPolyMaterial() const;
	bool IsAlpha() const;
	bool IsZSort() const;

	// Rendering
	bool ActivateMaterial(const MT_Transform& cameratrans, RAS_IRasterizer *rasty);
	void RenderMeshSlot(const MT_Transform& cameratrans, RAS_IRasterizer *rasty, RAS_MeshSlot *ms);

	// Mesh Slot Access
	RAS_MeshSlotList::iterator msBegin();
	RAS_MeshSlotList::iterator msEnd();

	RAS_MeshSlot *AddMesh();
	RAS_MeshSlot *CopyMesh(RAS_MeshSlot *ms);
	void RemoveMesh(RAS_MeshSlot *ms);
	void Optimize(MT_Scalar distance);

	/// Find a display array bucket for the given display array, if not retrurn a new one.
	RAS_DisplayArrayBucket *FindDisplayArrayBucket(RAS_DisplayArray *array);
	void AddDisplayArrayBucket(RAS_DisplayArrayBucket *bucket);
	void RemoveDisplayArrayBucket(RAS_DisplayArrayBucket *bucket);

	RAS_DisplayArrayBucketList& GetDisplayArrayBucketList();

private:
	RAS_MeshSlotList m_meshSlots; // all the mesh slots
	RAS_IPolyMaterial *m_material;
	RAS_DisplayArrayBucketList m_displayArrayBucketList;

#ifdef WITH_CXX_GUARDEDALLOC
	MEM_CXX_CLASS_ALLOC_FUNCS("GE:RAS_MaterialBucket")
#endif
};

#endif  // __RAS_MATERIAL_BUCKET
