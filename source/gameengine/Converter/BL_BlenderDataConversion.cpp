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
 * Convert blender data to ketsji
 */

/** \file gameengine/Converter/BL_BlenderDataConversion.cpp
 *  \ingroup bgeconv
 */

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

/* Since threaded object update we've disabled in-place
 * curve evaluation (in cases when applying curve modifier
 * with target curve non-evaluated yet).
 *
 * This requires game engine to take care of DAG and object
 * evaluation (currently it's designed to export only objects
 * it able to render).
 *
 * This workaround will make sure that curve_cache for curves
 * is up-to-date.
 */
#define THREADED_DAG_WORKAROUND

#include <math.h>
#include <vector>
#include <algorithm>


#include "mathfu.h"

#include "PHY_IPhysicsEnvironment.h"
#include "DummyPhysicsEnvironment.h"

#ifdef WITH_BULLET
#  include "CcdPhysicsEnvironment.h"
#  include "CcdGraphicController.h"
#endif

#include "RAS_Rasterizer.h"
#include "RAS_ILightObject.h"

#include "RAS_ICanvas.h"
#include "RAS_BucketManager.h"
#include "RAS_BoundingBoxManager.h"
#include "RAS_IMaterial.h"

#include "SG_Node.h"
#include "SG_BBox.h"

#include "SCA_LogicManager.h"
#include "SCA_TimeEventManager.h"

#include "KX_SoftBodyDeformer.h"
#include "KX_ClientObjectInfo.h"
#include "KX_Scene.h"
#include "KX_GameObject.h"
#include "KX_LightObject.h"
#include "KX_Camera.h"
#include "KX_EmptyObject.h"
#include "KX_FontObject.h"
#include "KX_LodManager.h"
#include "KX_PythonComponent.h"
#include "KX_WorldInfo.h"
#include "KX_Mesh.h"
#include "KX_BlenderMaterial.h"
#include "KX_TextureRendererManager.h"
#include "KX_Globals.h"
#include "KX_PyConstraintBinding.h"
#include "KX_KetsjiEngine.h"
#include "KX_NodeRelationships.h"
#include "KX_BoneParentNodeRelationship.h"
#include "KX_MotionState.h"
#include "KX_NavMeshObject.h"
#include "KX_ObstacleSimulation.h"

#include "BL_BlenderDataConversion.h"
#include "BL_ModifierDeformer.h"
#include "BL_ShapeDeformer.h"
#include "BL_SkinDeformer.h"
#include "BL_MeshDeformer.h"
#include "BL_Texture.h"
#include "BL_SceneConverter.h"
#include "BL_ConvertActuators.h"
#include "BL_ConvertControllers.h"
#include "BL_ConvertSensors.h"
#include "BL_ConvertProperties.h"
#include "BL_ConvertObjectInfo.h"
#include "BL_ArmatureObject.h"
#include "BL_ActionData.h"

#include "LA_SystemCommandLine.h"

#include "CM_Message.h"

#include "GPU_texture.h"

// This little block needed for linking to Blender...
#ifdef WIN32
#include "BLI_winstuff.h"
#endif

#include "BLI_utildefines.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_threads.h"

#include "DNA_object_types.h"
#include "DNA_material_types.h"
#include "DNA_texture_types.h"
#include "DNA_image_types.h"
#include "DNA_lamp_types.h"
#include "DNA_group_types.h"
#include "DNA_scene_types.h"
#include "DNA_camera_types.h"
#include "DNA_property_types.h"
#include "DNA_text_types.h"
#include "DNA_sensor_types.h"
#include "DNA_controller_types.h"
#include "DNA_actuator_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_view3d_types.h"
#include "DNA_world_types.h"
#include "DNA_sound_types.h"
#include "DNA_key_types.h"
#include "DNA_armature_types.h"
#include "DNA_action_types.h"
#include "DNA_object_force_types.h"
#include "DNA_constraint_types.h"
#include "DNA_python_component_types.h"

#include "MEM_guardedalloc.h"

#include "BKE_main.h"
#include "BKE_global.h"
#include "BKE_object.h"
#include "BKE_python_component.h"
#include "BKE_key.h"
#include "BKE_mesh.h"

extern "C" {
#  include "BKE_scene.h"
#  include "BKE_customdata.h"
#  include "BKE_cdderivedmesh.h"
#  include "BKE_DerivedMesh.h"
#  include "BKE_material.h" // Needed for give_current_material.
#  include "BKE_image.h"
#  include "IMB_imbuf_types.h"
#  include "BKE_displist.h"

extern Material defmaterial;
}

#include "wm_event_types.h"

// For construction to find shared vertices.
struct BL_SharedVertex {
	RAS_DisplayArray *array;
	unsigned int offset;
};

using BL_SharedVertexList = std::vector<BL_SharedVertex>;
using BL_SharedVertexMap = std::vector<BL_SharedVertexList>;

class BL_SharedVertexPredicate
{
private:
	RAS_DisplayArray *m_array;
	mt::vec3_packed m_normal;
	mt::vec4_packed m_tangent;
	mt::vec2_packed m_uvs[RAS_Texture::MaxUnits];
	unsigned int m_colors[RAS_Texture::MaxUnits];

public:
	BL_SharedVertexPredicate(RAS_DisplayArray *array, const mt::vec3_packed& normal, const mt::vec4_packed& tangent, mt::vec2_packed uvs[], unsigned int colors[])
		:m_array(array),
		m_normal(normal),
		m_tangent(tangent)
	{
		const RAS_DisplayArray::Format& format = m_array->GetFormat();

		for (unsigned short i = 0, size = format.uvSize; i < size; ++i) {
			m_uvs[i] = uvs[i];
		}

		for (unsigned short i = 0, size = format.colorSize; i < size; ++i) {
			m_colors[i] = colors[i];
		}
	}

	bool operator()(const BL_SharedVertex& sharedVert) const
	{
		RAS_DisplayArray *otherArray = sharedVert.array;
		if (m_array != otherArray) {
			return false;
		}

		const unsigned int offset = sharedVert.offset;

		static const float eps = FLT_EPSILON;
		if (!compare_v3v3(m_array->GetNormal(offset).data, m_normal.data, eps) ||
			!compare_v3v3(m_array->GetTangent(offset).data, m_tangent.data, eps))
		{
			return false;
		}

		const RAS_DisplayArray::Format& format = m_array->GetFormat();
		for (unsigned short i = 0, size = format.uvSize; i < size; ++i) {
			if (!compare_v2v2(m_array->GetUv(offset, i).data, m_uvs[i].data, eps)) {
				return false;
			}
		}

		for (unsigned short i = 0, size = format.colorSize; i < size; ++i) {
			if (m_array->GetRawColor(offset, i) != m_colors[i]) {
				return false;
			}
		}

		return true;
	}
};

/* The reverse table. In order to not confuse ourselves, we
 * immediately convert all events that come in to KX codes. */
static std::map<int, SCA_IInputDevice::SCA_EnumInputs> gReverseKeyTranslateTable = {
	{LEFTMOUSE, SCA_IInputDevice::LEFTMOUSE},
	{MIDDLEMOUSE, SCA_IInputDevice::MIDDLEMOUSE},
	{RIGHTMOUSE, SCA_IInputDevice::RIGHTMOUSE},
	{WHEELUPMOUSE, SCA_IInputDevice::WHEELUPMOUSE},
	{WHEELDOWNMOUSE, SCA_IInputDevice::WHEELDOWNMOUSE},
	{MOUSEMOVE, SCA_IInputDevice::MOUSEX},
	{ACTIONMOUSE, SCA_IInputDevice::MOUSEY},
	// Standard keyboard.
	{AKEY, SCA_IInputDevice::AKEY},
	{BKEY, SCA_IInputDevice::BKEY},
	{CKEY, SCA_IInputDevice::CKEY},
	{DKEY, SCA_IInputDevice::DKEY},
	{EKEY, SCA_IInputDevice::EKEY},
	{FKEY, SCA_IInputDevice::FKEY},
	{GKEY, SCA_IInputDevice::GKEY},
	{HKEY, SCA_IInputDevice::HKEY_},
	{IKEY, SCA_IInputDevice::IKEY},
	{JKEY, SCA_IInputDevice::JKEY},
	{KKEY, SCA_IInputDevice::KKEY},
	{LKEY, SCA_IInputDevice::LKEY},
	{MKEY, SCA_IInputDevice::MKEY},
	{NKEY, SCA_IInputDevice::NKEY},
	{OKEY, SCA_IInputDevice::OKEY},
	{PKEY, SCA_IInputDevice::PKEY},
	{QKEY, SCA_IInputDevice::QKEY},
	{RKEY, SCA_IInputDevice::RKEY},
	{SKEY, SCA_IInputDevice::SKEY},
	{TKEY, SCA_IInputDevice::TKEY},
	{UKEY, SCA_IInputDevice::UKEY},
	{VKEY, SCA_IInputDevice::VKEY},
	{WKEY, SCA_IInputDevice::WKEY},
	{XKEY, SCA_IInputDevice::XKEY},
	{YKEY, SCA_IInputDevice::YKEY},
	{ZKEY, SCA_IInputDevice::ZKEY},

	{ZEROKEY, SCA_IInputDevice::ZEROKEY},
	{ONEKEY, SCA_IInputDevice::ONEKEY},
	{TWOKEY, SCA_IInputDevice::TWOKEY},
	{THREEKEY, SCA_IInputDevice::THREEKEY},
	{FOURKEY, SCA_IInputDevice::FOURKEY},
	{FIVEKEY, SCA_IInputDevice::FIVEKEY},
	{SIXKEY, SCA_IInputDevice::SIXKEY},
	{SEVENKEY, SCA_IInputDevice::SEVENKEY},
	{EIGHTKEY, SCA_IInputDevice::EIGHTKEY},
	{NINEKEY, SCA_IInputDevice::NINEKEY},

	{CAPSLOCKKEY, SCA_IInputDevice::CAPSLOCKKEY},

	{LEFTCTRLKEY, SCA_IInputDevice::LEFTCTRLKEY},
	{LEFTALTKEY, SCA_IInputDevice::LEFTALTKEY},
	{RIGHTALTKEY, SCA_IInputDevice::RIGHTALTKEY},
	{RIGHTCTRLKEY, SCA_IInputDevice::RIGHTCTRLKEY},
	{RIGHTSHIFTKEY, SCA_IInputDevice::RIGHTSHIFTKEY},
	{LEFTSHIFTKEY, SCA_IInputDevice::LEFTSHIFTKEY},

	{ESCKEY, SCA_IInputDevice::ESCKEY},
	{TABKEY, SCA_IInputDevice::TABKEY},
	{RETKEY, SCA_IInputDevice::RETKEY},
	{SPACEKEY, SCA_IInputDevice::SPACEKEY},
	{LINEFEEDKEY, SCA_IInputDevice::LINEFEEDKEY},
	{BACKSPACEKEY, SCA_IInputDevice::BACKSPACEKEY},
	{DELKEY, SCA_IInputDevice::DELKEY},
	{SEMICOLONKEY, SCA_IInputDevice::SEMICOLONKEY},
	{PERIODKEY, SCA_IInputDevice::PERIODKEY},
	{COMMAKEY, SCA_IInputDevice::COMMAKEY},
	{QUOTEKEY, SCA_IInputDevice::QUOTEKEY},
	{ACCENTGRAVEKEY, SCA_IInputDevice::ACCENTGRAVEKEY},
	{MINUSKEY, SCA_IInputDevice::MINUSKEY},
	{SLASHKEY, SCA_IInputDevice::SLASHKEY},
	{BACKSLASHKEY, SCA_IInputDevice::BACKSLASHKEY},
	{EQUALKEY, SCA_IInputDevice::EQUALKEY},
	{LEFTBRACKETKEY, SCA_IInputDevice::LEFTBRACKETKEY},
	{RIGHTBRACKETKEY, SCA_IInputDevice::RIGHTBRACKETKEY},

	{LEFTARROWKEY, SCA_IInputDevice::LEFTARROWKEY},
	{DOWNARROWKEY, SCA_IInputDevice::DOWNARROWKEY},
	{RIGHTARROWKEY, SCA_IInputDevice::RIGHTARROWKEY},
	{UPARROWKEY, SCA_IInputDevice::UPARROWKEY},

	{PAD2, SCA_IInputDevice::PAD2},
	{PAD4, SCA_IInputDevice::PAD4},
	{PAD6, SCA_IInputDevice::PAD6},
	{PAD8, SCA_IInputDevice::PAD8},

	{PAD1, SCA_IInputDevice::PAD1},
	{PAD3, SCA_IInputDevice::PAD3},
	{PAD5, SCA_IInputDevice::PAD5},
	{PAD7, SCA_IInputDevice::PAD7},
	{PAD9, SCA_IInputDevice::PAD9},

	{PADPERIOD, SCA_IInputDevice::PADPERIOD},
	{PADSLASHKEY, SCA_IInputDevice::PADSLASHKEY},
	{PADASTERKEY, SCA_IInputDevice::PADASTERKEY},

	{PAD0, SCA_IInputDevice::PAD0},
	{PADMINUS, SCA_IInputDevice::PADMINUS},
	{PADENTER, SCA_IInputDevice::PADENTER},
	{PADPLUSKEY, SCA_IInputDevice::PADPLUSKEY},

	{F1KEY, SCA_IInputDevice::F1KEY},
	{F2KEY, SCA_IInputDevice::F2KEY},
	{F3KEY, SCA_IInputDevice::F3KEY},
	{F4KEY, SCA_IInputDevice::F4KEY},
	{F5KEY, SCA_IInputDevice::F5KEY},
	{F6KEY, SCA_IInputDevice::F6KEY},
	{F7KEY, SCA_IInputDevice::F7KEY},
	{F8KEY, SCA_IInputDevice::F8KEY},
	{F9KEY, SCA_IInputDevice::F9KEY},
	{F10KEY, SCA_IInputDevice::F10KEY},
	{F11KEY, SCA_IInputDevice::F11KEY},
	{F12KEY, SCA_IInputDevice::F12KEY},
	{F13KEY, SCA_IInputDevice::F13KEY},
	{F14KEY, SCA_IInputDevice::F14KEY},
	{F15KEY, SCA_IInputDevice::F15KEY},
	{F16KEY, SCA_IInputDevice::F16KEY},
	{F17KEY, SCA_IInputDevice::F17KEY},
	{F18KEY, SCA_IInputDevice::F18KEY},
	{F19KEY, SCA_IInputDevice::F19KEY},

	{OSKEY, SCA_IInputDevice::OSKEY},

	{PAUSEKEY, SCA_IInputDevice::PAUSEKEY},
	{INSERTKEY, SCA_IInputDevice::INSERTKEY},
	{HOMEKEY, SCA_IInputDevice::HOMEKEY},
	{PAGEUPKEY, SCA_IInputDevice::PAGEUPKEY},
	{PAGEDOWNKEY, SCA_IInputDevice::PAGEDOWNKEY},
	{ENDKEY, SCA_IInputDevice::ENDKEY}
};

SCA_IInputDevice::SCA_EnumInputs BL_ConvertKeyCode(int key_code)
{
	return gReverseKeyTranslateTable[key_code];
}

static void BL_GetUvRgba(const RAS_Mesh::LayersInfo& layersInfo, std::vector<MLoopUV *>& uvLayers,
                         std::vector<MLoopCol *>& colorLayers, unsigned int loop, mt::vec2_packed uvs[RAS_Texture::MaxUnits],
                         unsigned int rgba[RAS_Texture::MaxUnits])
{
	// No need to initialize layers to zero as all the converted layer are all the layers needed.

	for (const RAS_Mesh::Layer& layer : layersInfo.colorLayers) {
		const unsigned short index = layer.index;
		const MLoopCol& col = colorLayers[index][loop];

		union Convert
		{
			// Color isn't swapped in MLoopCol.
			MLoopCol col;
			unsigned int val;
		};
		Convert con;
		con.col = col;

		rgba[index] = con.val;
	}

	for (const RAS_Mesh::Layer& layer : layersInfo.uvLayers) {
		const unsigned short index = layer.index;
		const MLoopUV& uv = uvLayers[index][loop];
		uvs[index] = mt::vec2_packed(uv.uv);
	}

	/* All vertices have at least one uv and color layer accessible to the user
	 * even if it they are not used in any shaders. Initialize this layer to zero
	 * when no uv or color layer exist.
	 */
	if (layersInfo.uvLayers.empty()) {
		uvs[0] = mt::zero2;
	}
	if (layersInfo.colorLayers.empty()) {
		rgba[0] = 0xFFFFFFFF;
	}
}

static RAS_MaterialBucket *BL_ConvertMaterial(Material *ma, KX_Scene *scene, BL_SceneConverter& converter)
{
	KX_BlenderMaterial *mat = converter.FindMaterial(ma);

	if (!mat) {
		std::string name = ma->id.name;
		// Always ensure that the name of a material start with "MA" prefix due to video texture name check.
		if (name.empty()) {
			name = "MA";
		}

		mat = new KX_BlenderMaterial(ma, name, scene);

		// this is needed to free up memory afterwards.
		converter.RegisterMaterial(mat, ma);
	}

	// see if a bucket was reused or a new one was created
	// this way only one KX_BlenderMaterial object has to exist per bucket
	bool bucketCreated;
	RAS_MaterialBucket *bucket = scene->GetBucketManager()->FindBucket(mat, bucketCreated);

	return bucket;
}

/* blenderobj can be nullptr, make sure its checked for */
KX_Mesh *BL_ConvertMesh(Mesh *me, Object *blenderobj, KX_Scene *scene, BL_SceneConverter& converter)
{
	KX_Mesh *meshobj;

	// Without checking names, we get some reuse we don't want that can cause
	// problems with material LoDs.
	if (blenderobj && ((meshobj = converter.FindGameMesh(me)) != nullptr)) {
		const std::string bge_name = meshobj->GetName();
		const std::string blender_name = ((ID *)blenderobj->data)->name + 2;
		if (bge_name == blender_name) {
			return meshobj;
		}
	}

	// Get DerivedMesh data.
	DerivedMesh *dm = CDDM_from_mesh(me);

	/* Extract available layers.
	 * Get the active color and uv layer. */
	const short activeUv = CustomData_get_active_layer(&dm->loopData, CD_MLOOPUV);
	const short activeColor = CustomData_get_active_layer(&dm->loopData, CD_MLOOPCOL);
	const unsigned short uvCount = CustomData_number_of_layers(&dm->loopData, CD_MLOOPUV);
	const unsigned short colorCount = CustomData_number_of_layers(&dm->loopData, CD_MLOOPCOL);

	RAS_Mesh::LayersInfo layersInfo;
	layersInfo.activeUv = (activeUv == -1) ? 0 : activeUv;
	layersInfo.activeColor = (activeColor == -1) ? 0 : activeColor;

	// Extract UV loops.
	for (unsigned short i = 0; i < uvCount; ++i) {
		const std::string name = CustomData_get_layer_name(&dm->loopData, CD_MLOOPUV, i);
		layersInfo.uvLayers.push_back({i, name});
	}
	// Extract color loops.
	for (unsigned short i = 0; i < colorCount; ++i) {
		const std::string name = CustomData_get_layer_name(&dm->loopData, CD_MLOOPCOL, i);
		layersInfo.colorLayers.push_back({i, name});
	}

	// Initialize vertex format with used uv and color layers.
	RAS_DisplayArray::Format vertformat;
	vertformat.uvSize = max_ii(1, uvCount);
	vertformat.colorSize = max_ii(1, colorCount);

	meshobj = new KX_Mesh(scene, me, layersInfo);

	const unsigned short totmat = max_ii(me->totcol, 1);
	std::vector<BL_MeshMaterial> mats(totmat);
	// Convert all the materials contained in the mesh.
	for (unsigned short i = 0; i < totmat; ++i) {
		Material *ma = nullptr;
		if (blenderobj) {
			ma = give_current_material(blenderobj, i + 1);
		}
		else {
			ma = me->mat ? me->mat[i] : nullptr;
		}
		// Check for blender material
		if (!ma) {
			ma = &defmaterial;
		}

		RAS_MaterialBucket *bucket = BL_ConvertMaterial(ma, scene, converter);
		RAS_MeshMaterial *meshmat = meshobj->AddMaterial(bucket, i, vertformat);
		RAS_IMaterial *mat = meshmat->GetBucket()->GetMaterial();

		mats[i] = {meshmat->GetDisplayArray(), bucket, mat->IsVisible(), mat->IsTwoSided(), mat->IsCollider(), mat->IsWire()};
	}

	BL_ConvertDerivedMeshToArray(dm, me, mats, layersInfo);

	meshobj->EndConversion(scene->GetBoundingBoxManager());

	dm->release(dm);

	// Needed for python scripting.
	scene->GetLogicManager()->RegisterMeshName(meshobj->GetName(), meshobj);
	converter.RegisterGameMesh(meshobj, me);

	return meshobj;
}

void BL_ConvertDerivedMeshToArray(DerivedMesh *dm, Mesh *me, const std::vector<BL_MeshMaterial>& mats,
                                  const RAS_Mesh::LayersInfo& layersInfo)
{
	const MVert *mverts = dm->getVertArray(dm);
	const int totverts = dm->getNumVerts(dm);
	const MPoly *mpolys = (MPoly *)dm->getPolyArray(dm);
	const MLoopTri *mlooptris = (MLoopTri *)dm->getLoopTriArray(dm);
	const MLoop *mloops = (MLoop *)dm->getLoopArray(dm);
	const MEdge *medges = (MEdge *)dm->getEdgeArray(dm);
	const unsigned int numpolys = dm->getNumPolys(dm);

	if (CustomData_get_layer_index(&dm->loopData, CD_NORMAL) == -1) {
		dm->calcLoopNormals(dm, (me->flag & ME_AUTOSMOOTH), me->smoothresh);
	}
	const float(*normals)[3] = (float(*)[3])dm->getLoopDataArray(dm, CD_NORMAL);

	float(*tangent)[4] = nullptr;
	if (!layersInfo.uvLayers.empty()) {
		if (CustomData_get_layer_index(&dm->loopData, CD_TANGENT) == -1) {
			DM_calc_loop_tangents(dm, true, nullptr, 0);
		}
		tangent = (float(*)[4])dm->getLoopDataArray(dm, CD_TANGENT);
	}

	// List of MLoopUV per uv layer index.
	std::vector<MLoopUV *> uvLayers(layersInfo.uvLayers.size());
	// List of MLoopCol per color layer index.
	std::vector<MLoopCol *> colorLayers(layersInfo.colorLayers.size());

	for (const RAS_Mesh::Layer& layer : layersInfo.uvLayers) {
		const unsigned short index = layer.index;
		uvLayers[index] = (MLoopUV *)CustomData_get_layer_n(&dm->loopData, CD_MLOOPUV, index);
	}
	for (const RAS_Mesh::Layer& layer : layersInfo.colorLayers) {
		const unsigned short index = layer.index;
		colorLayers[index] = (MLoopCol *)CustomData_get_layer_n(&dm->loopData, CD_MLOOPCOL, index);
	}

	BL_SharedVertexMap sharedMap(totverts);

	// Tracked vertices during a mpoly conversion, should never be used by the next mpoly.
	std::vector<unsigned int> vertices(totverts, -1);

	for (unsigned int i = 0; i < numpolys; ++i) {
		const MPoly& mpoly = mpolys[i];

		const BL_MeshMaterial& mat = mats[mpoly.mat_nr];
		RAS_DisplayArray *array = mat.array;

		// Mark face as flat, so vertices are split.
		const bool flat = (mpoly.flag & ME_SMOOTH) == 0;

		const unsigned int lpstart = mpoly.loopstart;
		const unsigned int totlp = mpoly.totloop;
		for (unsigned int j = lpstart; j < lpstart + totlp; ++j) {
			const MLoop& mloop = mloops[j];
			const unsigned int vertid = mloop.v;
			const MVert& mvert = mverts[vertid];

			static const float dummyTangent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
			const mt::vec4_packed tan(tangent ? tangent[j] : dummyTangent);
			const mt::vec3_packed nor(normals[j]);
			const mt::vec3_packed pos(mvert.co);
			mt::vec2_packed uvs[RAS_Texture::MaxUnits];
			unsigned int rgba[RAS_Texture::MaxUnits];

			BL_GetUvRgba(layersInfo, uvLayers, colorLayers, j, uvs, rgba);

			BL_SharedVertexList& sharedList = sharedMap[vertid];
			BL_SharedVertexList::iterator it = std::find_if(sharedList.begin(), sharedList.end(),
					BL_SharedVertexPredicate(array, nor, tan, uvs, rgba));

			unsigned int offset;
			if (it != sharedList.end()) {
				offset = it->offset;
			}
			else {
				offset = array->AddVertex(pos, nor, tan, uvs, rgba, vertid, flat);
				sharedList.push_back({array, offset});
			}

			// Add tracked vertices by the mpoly.
			vertices[vertid] = offset;
		}

		const unsigned int ltstart = poly_to_tri_count(i, mpoly.loopstart);
		const unsigned int lttot = ME_POLY_TRI_TOT(&mpoly);

		if (mat.visible) {
			if (mat.wire) {
				// Convert to edges if material is rendering wire.
				for (unsigned int j = lpstart; j < (lpstart + totlp); ++j) {
					const MLoop& mloop = mloops[j];
					const MEdge& edge = medges[mloop.e];
					array->AddPrimitiveIndex(vertices[edge.v1]);
					array->AddPrimitiveIndex(vertices[edge.v2]);
				}
			}
			else {
				for (unsigned int j = ltstart; j < (ltstart + lttot); ++j) {
					const MLoopTri& mlooptri = mlooptris[j];
					for (unsigned short k = 0; k < 3; ++k) {
						array->AddPrimitiveIndex(vertices[mloops[mlooptri.tri[k]].v]);
					}
				}
			}
		}

		for (unsigned int j = ltstart; j < (ltstart + lttot); ++j) {
			const MLoopTri& mlooptri = mlooptris[j];
			for (unsigned short k = 0; k < 3; ++k) {
				// Add triangle index into display array.
				array->AddTriangleIndex(vertices[mloops[mlooptri.tri[k]].v]);
			}
		}
	}
}

RAS_Deformer *BL_ConvertDeformer(KX_GameObject *object, KX_Mesh *meshobj)
{
	Mesh *mesh = meshobj->GetMesh();

	if (!mesh) {
		return nullptr;
	}

	KX_Scene *scene = object->GetScene();
	Scene *blenderScene = scene->GetBlenderScene();
	// We must create a new deformer but which one?
	KX_GameObject *parentobj = object->GetParent();
	/* Object that owns the mesh. If this is not the current blender object, look at one of the object registered
	 * along the blender mesh. */
	Object *meshblendobj;
	Object *blenderobj = object->GetBlenderObject();
	if (blenderobj->data != mesh) {
		meshblendobj = static_cast<Object *>(scene->GetLogicManager()->FindBlendObjByGameMeshName(meshobj->GetName()));
	}
	else {
		meshblendobj = blenderobj;
	}

	const bool isParentArmature = parentobj && parentobj->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE;
	const bool bHasModifier = BL_ModifierDeformer::HasCompatibleDeformer(blenderobj);
	const bool bHasShapeKey = mesh->key && mesh->key->type == KEY_RELATIVE;
	const bool bHasDvert = mesh->dvert && blenderobj->defbase.first;
	const bool bHasArmature = BL_ModifierDeformer::HasArmatureDeformer(blenderobj) &&
	                          isParentArmature && meshblendobj && bHasDvert;
#ifdef WITH_BULLET
	const bool bHasSoftBody = (!parentobj && (blenderobj->gameflag & OB_SOFT_BODY));
#endif

	if (!meshblendobj) {
		if (bHasModifier || bHasShapeKey || bHasDvert || bHasArmature) {
			CM_FunctionWarning("new mesh is not used in an object from the current scene, you will get incorrect behavior.");
			return nullptr;
		}
	}

	RAS_Deformer *deformer = nullptr;
	if (bHasModifier) {
		if (isParentArmature) {
			BL_ModifierDeformer *modifierDeformer = new BL_ModifierDeformer(object, blenderScene, meshblendobj, blenderobj,
			                                                                meshobj, static_cast<BL_ArmatureObject *>(parentobj));
			modifierDeformer->LoadShapeDrivers(parentobj);
			deformer = modifierDeformer;
		}
		else {
			deformer = new BL_ModifierDeformer(object, blenderScene, meshblendobj, blenderobj, meshobj, nullptr);
		}
	}
	else if (bHasShapeKey) {
		if (isParentArmature) {
			BL_ShapeDeformer *shapeDeformer = new BL_ShapeDeformer(object, meshblendobj, blenderobj, meshobj,
			                                                       static_cast<BL_ArmatureObject *>(parentobj));
			shapeDeformer->LoadShapeDrivers(parentobj);
			deformer = shapeDeformer;
		}
		else {
			deformer = new BL_ShapeDeformer(object, meshblendobj, blenderobj, meshobj, nullptr);
		}
	}
	else if (bHasArmature) {
		deformer = new BL_SkinDeformer(object, meshblendobj, blenderobj, meshobj,
		                                 static_cast<BL_ArmatureObject *>(parentobj));
	}
	else if (bHasDvert) {
		deformer = new BL_MeshDeformer(object, meshblendobj, meshobj);
	}
#ifdef WITH_BULLET
	else if (bHasSoftBody) {
		deformer = new KX_SoftBodyDeformer(meshobj, object);
	}
#endif

	if (deformer) {
		deformer->InitializeDisplayArrays();
	}

	return deformer;
}

BL_ActionData *BL_ConvertAction(bAction *action, KX_Scene *scene, BL_SceneConverter& converter)
{
	BL_ActionData *data = new BL_ActionData(action);
	converter.RegisterActionData(data);
	scene->GetLogicManager()->RegisterActionName(action->id.name + 2, data);

	return data;
}

void BL_ConvertActions(KX_Scene *scene, Main *maggie, BL_SceneConverter& converter)
{
	// Convert all actions and register.
	for (bAction *act = (bAction *)maggie->action.first; act; act = (bAction *)act->id.next) {
		BL_ConvertAction(act, scene, converter);
	}
}

static void BL_CreateGraphicObjectNew(KX_GameObject *gameobj, KX_Scene *kxscene, bool isActive, PHY_IPhysicsEnvironment *phyEnv)
{
#ifdef WITH_BULLET
	CcdPhysicsEnvironment *env = static_cast<CcdPhysicsEnvironment *>(phyEnv);
	PHY_IMotionState *motionstate = new KX_MotionState(gameobj->GetNode());
	CcdGraphicController *ctrl = new CcdGraphicController(env, motionstate);
	gameobj->SetGraphicController(ctrl);
	ctrl->SetNewClientInfo(&gameobj->GetClientInfo());
	if (isActive) {
		// add first, this will create the proxy handle, only if the object is visible or occluder
		if (gameobj->GetVisible() || gameobj->GetOccluder()) {
			env->AddCcdGraphicController(ctrl);
		}
	}
#endif
}

static void BL_CreatePhysicsObjectNew(KX_GameObject *gameobj, Object *blenderobject, KX_Mesh *meshobj,
                                      KX_Scene *kxscene, int activeLayerBitInfo, BL_SceneConverter& converter, bool processCompoundChildren)

{
	// Object has physics representation?
	if (!(blenderobject->gameflag & OB_COLLISION)) {
		return;
	}

	Object *parent = blenderobject->parent;

	bool isCompoundChild = false;
	bool hasCompoundChildren = false;

	// Pretend for compound parent or child if the object has compound option and use a physics type with solid shape.
	if ((blenderobject->gameflag & (OB_CHILD)) && (blenderobject->gameflag & (OB_DYNAMIC | OB_COLLISION | OB_RIGID_BODY)) &&
	    !(blenderobject->gameflag & OB_SOFT_BODY)) {
		hasCompoundChildren = true;
		while (parent) {
			if ((parent->gameflag & OB_CHILD) && (parent->gameflag & (OB_COLLISION | OB_DYNAMIC | OB_RIGID_BODY)) &&
			    !(parent->gameflag & OB_SOFT_BODY)) {
				// Found a parent in the tree with compound shape.
				isCompoundChild = true;
				/* The object is not a parent compound shape if it has a parent
				 * object with compound shape. */
				hasCompoundChildren = false;
				break;
			}
			parent = parent->parent;
		}
	}

	if (processCompoundChildren != isCompoundChild) {
		return;
	}

	PHY_IMotionState *motionstate = new KX_MotionState(gameobj->GetNode());

	PHY_IPhysicsEnvironment *phyenv = kxscene->GetPhysicsEnvironment();
	phyenv->ConvertObject(converter, gameobj, meshobj, kxscene, motionstate, activeLayerBitInfo,
	                      isCompoundChild, hasCompoundChildren);

	bool isActor = (blenderobject->gameflag & OB_ACTOR) != 0;
	bool isSensor = (blenderobject->gameflag & OB_SENSOR) != 0;
	gameobj->GetClientInfo().m_type =
		(isSensor) ? ((isActor) ? KX_ClientObjectInfo::OBACTORSENSOR : KX_ClientObjectInfo::OBSENSOR) :
		(isActor) ? KX_ClientObjectInfo::ACTOR : KX_ClientObjectInfo::STATIC;
}

static KX_LodManager *BL_LodManagerFromBlenderObject(Object *ob, KX_Scene *scene, BL_SceneConverter& converter)
{
	if (BLI_listbase_count(&ob->lodlevels) <= 1) {
		return nullptr;
	}

	KX_LodManager *lodManager = new KX_LodManager(ob, scene, converter);
	// The lod manager is useless ?
	if (lodManager->GetLevelCount() <= 1) {
		lodManager->Release();
		return nullptr;
	}

	return lodManager;
}

/** Convert the object activity culling settings from blender to a KX_GameObject::ActivityCullingInfo.
 * \param ob The object to convert the activity culling settings from.
 */
static KX_GameObject::ActivityCullingInfo activityCullingInfoFromBlenderObject(Object *ob)
{
	KX_GameObject::ActivityCullingInfo cullingInfo;
	const ObjectActivityCulling& blenderInfo = ob->activityCulling;
	// Convert the flags.
	if (blenderInfo.flags & OB_ACTIVITY_PHYSICS) {
		// Enable physics culling.
		cullingInfo.m_flags = (KX_GameObject::ActivityCullingInfo::Flag)(
			cullingInfo.m_flags | KX_GameObject::ActivityCullingInfo::ACTIVITY_PHYSICS);
	}
	if (blenderInfo.flags & OB_ACTIVITY_LOGIC) {
		// Enable logic culling.
		cullingInfo.m_flags = (KX_GameObject::ActivityCullingInfo::Flag)(
			cullingInfo.m_flags | KX_GameObject::ActivityCullingInfo::ACTIVITY_LOGIC);
	}

	// Set culling radius.
	cullingInfo.m_physicsRadius = blenderInfo.physicsRadius * blenderInfo.physicsRadius;
	cullingInfo.m_logicRadius = blenderInfo.logicRadius * blenderInfo.logicRadius;

	return cullingInfo;
}

static KX_LightObject *BL_GameLightFromBlenderLamp(Lamp *la, unsigned int layerflag, KX_Scene *kxscene, RAS_Rasterizer *rasterizer)
{
	RAS_ILightObject *lightobj = rasterizer->CreateLight();

	lightobj->m_att1 = la->att1;
	lightobj->m_att2 = (la->mode & LA_QUAD) ? la->att2 : 0.0f;
	lightobj->m_coeff_const = la->coeff_const;
	lightobj->m_coeff_lin = la->coeff_lin;
	lightobj->m_coeff_quad = la->coeff_quad;
	lightobj->m_color = mt::vec3(la->r, la->g, la->b);
	lightobj->m_distance = la->dist;
	lightobj->m_energy = la->energy;
	lightobj->m_shadowclipstart = la->clipsta;
	lightobj->m_shadowclipend = la->clipend;
	lightobj->m_shadowbias = la->bias;
	lightobj->m_shadowbleedbias = la->bleedbias;
	lightobj->m_shadowmaptype = la->shadowmap_type;
	lightobj->m_shadowfrustumsize = la->shadow_frustum_size;
	lightobj->m_shadowcolor = mt::vec3(la->shdwr, la->shdwg, la->shdwb);
	lightobj->m_layer = layerflag;
	lightobj->m_spotblend = la->spotblend;
	lightobj->m_spotsize = la->spotsize;
	lightobj->m_staticShadow = la->mode & LA_STATIC_SHADOW;
	// Set to true to make at least one shadow render in static mode.
	lightobj->m_requestShadowUpdate = true;

	lightobj->m_nodiffuse = (la->mode & LA_NO_DIFF) != 0;
	lightobj->m_nospecular = (la->mode & LA_NO_SPEC) != 0;

	switch (la->type) {
		case LA_SUN:
		{
			lightobj->m_type = RAS_ILightObject::LIGHT_SUN;
			break;
		}
		case LA_SPOT:
		{
			lightobj->m_type = RAS_ILightObject::LIGHT_SPOT;
			break;
		}
		case LA_HEMI:
		{
			lightobj->m_type = RAS_ILightObject::LIGHT_HEMI;
			break;
		}
		default:
		{
			lightobj->m_type = RAS_ILightObject::LIGHT_NORMAL;
		}
	}

	KX_LightObject *gamelight = new KX_LightObject(kxscene, KX_Scene::m_callbacks, rasterizer, lightobj);

	gamelight->SetShowShadowFrustum((la->mode & LA_SHOW_SHADOW_BOX) && (la->mode & LA_SHAD_RAY));

	return gamelight;
}

static KX_Camera *BL_GameCameraFromBlenderCamera(Object *ob, KX_Scene *kxscene, RAS_ICanvas *canvas, float camZoom)
{
	Camera *ca = static_cast<Camera *>(ob->data);
	RAS_CameraData camdata(ca->lens, ca->ortho_scale, ca->sensor_x, ca->sensor_y, ca->sensor_fit, ca->shiftx, ca->shifty, ca->clipsta, ca->clipend, ca->type == CAM_PERSP, ca->YF_dofdist, camZoom);
	KX_Camera *gamecamera;

	gamecamera = new KX_Camera(kxscene, KX_Scene::m_callbacks, camdata);
	gamecamera->SetName(ca->id.name + 2);

	if (ca->gameflag & GAME_CAM_VIEWPORT) {
		const GameCameraViewportSettings& settings = ca->gameviewport;
		if (settings.leftratio > settings.rightratio || settings.bottomratio > settings.topratio) {
			CM_Warning("\"" << gamecamera->GetName() << "\" uses invalid custom viewport ratios, disabling custom viewport.");
		}
		else {
			gamecamera->EnableViewport(true);
			const int maxx = canvas->GetMaxX();
			const int maxy = canvas->GetMaxY();
			gamecamera->SetViewport(maxx * settings.leftratio, maxy * settings.bottomratio,
			                        maxx * settings.rightratio, maxy * settings.topratio);
		}
	}

	gamecamera->SetShowCameraFrustum(ca->gameflag & GAME_CAM_SHOW_FRUSTUM);
	gamecamera->SetLodDistanceFactor(ca->lodfactor);

	gamecamera->SetActivityCulling(ca->gameflag & GAME_CAM_OBJECT_ACTIVITY_CULLING);

	if (ca->gameflag & GAME_CAM_OVERRIDE_CULLING) {
		if (kxscene->GetOverrideCullingCamera()) {
			CM_Warning("\"" << gamecamera->GetName() << "\" sets for culling override whereas \""
			                << kxscene->GetOverrideCullingCamera()->GetName() << "\" is already used for culling override.");
		}
		else {
			kxscene->SetOverrideCullingCamera(gamecamera);
		}
	}

	return gamecamera;
}

static KX_GameObject *BL_GameObjectFromBlenderObject(Object *ob, KX_Scene *kxscene, RAS_Rasterizer *rendertools,
                                                     RAS_ICanvas *canvas, BL_SceneConverter &converter, float camZoom)
{
	KX_GameObject *gameobj = nullptr;
	Scene *blenderscene = kxscene->GetBlenderScene();

	switch (ob->type) {
		case OB_LAMP:
		{
			KX_LightObject *gamelight = BL_GameLightFromBlenderLamp(static_cast<Lamp *>(ob->data), ob->lay, kxscene, rendertools);
			gameobj = gamelight;
			gamelight->AddRef();
			kxscene->GetLightList()->Add(gamelight);

			break;
		}

		case OB_CAMERA:
		{
			KX_Camera *gamecamera = BL_GameCameraFromBlenderCamera(ob, kxscene, canvas, camZoom);
			gameobj = gamecamera;

			kxscene->GetCameraList()->Add(CM_AddRef(gamecamera));

			break;
		}

		case OB_MESH:
		{
			Mesh *mesh = static_cast<Mesh *>(ob->data);
			KX_Mesh *meshobj = BL_ConvertMesh(mesh, ob, kxscene, converter);

			if (ob->gameflag & OB_NAVMESH) {
				gameobj = new KX_NavMeshObject(kxscene, KX_Scene::m_callbacks);
				gameobj->AddMesh(meshobj);
				break;
			}


			KX_GameObject *deformableGameObj = new KX_GameObject(kxscene, KX_Scene::m_callbacks);
			gameobj = deformableGameObj;

			// set transformation
			gameobj->AddMesh(meshobj);

			// gather levels of detail
			KX_LodManager *lodManager = BL_LodManagerFromBlenderObject(ob, kxscene, converter);
			gameobj->SetLodManager(lodManager);
			if (lodManager) {
				lodManager->Release();
			}

			gameobj->SetOccluder((ob->gameflag & OB_OCCLUDER) != 0, false);
			gameobj->SetActivityCullingInfo(activityCullingInfoFromBlenderObject(ob));
			break;
		}

		case OB_ARMATURE:
		{
			gameobj = new BL_ArmatureObject(kxscene, KX_Scene::m_callbacks, ob, kxscene->GetBlenderScene());

			break;
		}

		case OB_EMPTY:
		{
			gameobj = new KX_EmptyObject(kxscene, KX_Scene::m_callbacks);

			break;
		}

		case OB_FONT:
		{
			// Font objects have no bounding box.
			KX_FontObject *fontobj = new KX_FontObject(kxscene, KX_Scene::m_callbacks, rendertools,
			                                           kxscene->GetBoundingBoxManager(), ob);
			gameobj = fontobj;

			kxscene->GetFontList()->Add(CM_AddRef(fontobj));
			break;
		}

#ifdef THREADED_DAG_WORKAROUND
		case OB_CURVE:
		{
			if (ob->curve_cache == nullptr) {
				BKE_displist_make_curveTypes(blenderscene, ob, false);
			}
			/* We can convert curves as empty for experimental purposes in 2.7
			 * and to prepare transition to 2.8.
			 * Note: if we use eevee render in 2.8, to finalize stuff about curves,
			 * see : https://github.com/youle31/EEVEEinUPBGE/commit/ff11e0fdea4dfc121a7eaa7b7d48183eaf5fd9f6
			 * for comments about culling.
			 */
			gameobj = new KX_EmptyObject(kxscene, KX_Scene::m_callbacks);
			break;
		}
#endif
	}
	if (gameobj) {
		gameobj->SetLayer(ob->lay);
		BL_ConvertObjectInfo *info = converter.GetObjectInfo(ob);
		gameobj->SetConvertObjectInfo(info);
		gameobj->SetObjectColor(mt::vec4(ob->col));
		// Set the visibility state based on the objects render option in the outliner.
		if (ob->restrictflag & OB_RESTRICT_RENDER) {
			gameobj->SetVisible(false, false);
		}
	}

	return gameobj;
}

struct BL_ParentChildLink {
	struct Object *m_blenderchild;
	SG_Node *m_gamechildnode;
};


static bPoseChannel *BL_GetActivePoseChannel(Object *ob)
{
	bArmature *arm = (bArmature *)ob->data;
	bPoseChannel *pchan;

	/* find active */
	for (pchan = (bPoseChannel *)ob->pose->chanbase.first; pchan; pchan = pchan->next) {
		if (pchan->bone && (pchan->bone == arm->act_bone) && (pchan->bone->layer & arm->layer)) {
			return pchan;
		}
	}

	return nullptr;
}

static ListBase *BL_GetActiveConstraint(Object *ob)
{
	if (!ob) {
		return nullptr;
	}

	// XXX - shouldnt we care about the pose data and not the mode???
	if (ob->mode & OB_MODE_POSE) {
		bPoseChannel *pchan;

		pchan = BL_GetActivePoseChannel(ob);
		if (pchan) {
			return &pchan->constraints;
		}
	}
	else {
		return &ob->constraints;
	}

	return nullptr;
}

// Copy base layer to object layer like in BKE_scene_set_background
static void BL_SetBlenderSceneBackground(Scene *blenderscene)
{
	Scene *it;
	Base *base;

	for (SETLOOPER(blenderscene, it, base)) {
		base->object->lay = base->lay;
		base->object->flag = base->flag;
	}
}

static void BL_ConvertComponentsObject(KX_GameObject *gameobj, Object *blenderobj)
{
#ifdef WITH_PYTHON
	PythonComponent *pc = (PythonComponent *)blenderobj->components.first;
	PyObject *arg_dict = nullptr, *args = nullptr, *mod = nullptr, *cls = nullptr, *pycomp = nullptr, *ret = nullptr;

	if (!pc) {
		return;
	}

	EXP_ListValue<KX_PythonComponent> *components = new EXP_ListValue<KX_PythonComponent>();

	while (pc) {
		// Make sure to clean out anything from previous loops
		Py_XDECREF(args);
		Py_XDECREF(arg_dict);
		Py_XDECREF(mod);
		Py_XDECREF(cls);
		Py_XDECREF(ret);
		Py_XDECREF(pycomp);
		args = arg_dict = mod = cls = pycomp = ret = nullptr;

		// Grab the module
		mod = PyImport_ImportModule(pc->module);

		if (mod == nullptr) {
			if (PyErr_Occurred()) {
				PyErr_Print();
			}
			CM_Error("coulding import the module '" << pc->module << "'");
			pc = pc->next;
			continue;
		}

		// Grab the class object
		cls = PyObject_GetAttrString(mod, pc->name);
		if (cls == nullptr) {
			if (PyErr_Occurred()) {
				PyErr_Print();
			}
			CM_Error("python module found, but failed to find the component '" << pc->name << "'");
			pc = pc->next;
			continue;
		}

		// Lastly make sure we have a class and it's an appropriate sub type
		if (!PyType_Check(cls) || !PyObject_IsSubclass(cls, (PyObject *)&KX_PythonComponent::Type)) {
			CM_Error(pc->module << "." << pc->name << " is not a KX_PythonComponent subclass");
			pc = pc->next;
			continue;
		}

		// Every thing checks out, now generate the args dictionary and init the component
		args = PyTuple_Pack(1, gameobj->GetProxy());

		pycomp = PyObject_Call(cls, args, nullptr);

		if (PyErr_Occurred()) {
			// The component is invalid, drop it
			PyErr_Print();
		}
		else {
			KX_PythonComponent *comp = static_cast<KX_PythonComponent *>(EXP_PROXY_REF(pycomp));
			comp->SetBlenderPythonComponent(pc);
			comp->SetGameObject(gameobj);
			components->Add(comp);
		}

		pc = pc->next;
	}

	Py_XDECREF(args);
	Py_XDECREF(mod);
	Py_XDECREF(cls);
	Py_XDECREF(pycomp);

	gameobj->SetComponents(components);
#endif  // WITH_PYTHON
}

/* helper for BL_ConvertBlenderObjects, avoids code duplication
 * note: all var names match args are passed from the caller */
static void bl_ConvertBlenderObject_Single(BL_SceneConverter& converter,
                                           Object *blenderobject,
                                           std::vector<BL_ParentChildLink> &vec_parent_child,
                                           EXP_ListValue<KX_GameObject> *logicbrick_conversionlist,
                                           EXP_ListValue<KX_GameObject> *objectlist, EXP_ListValue<KX_GameObject> *inactivelist,
                                           KX_Scene *kxscene, KX_GameObject *gameobj,
                                           SCA_LogicManager *logicmgr, SCA_TimeEventManager *timemgr,
                                           bool isInActiveLayer)
{
	const mt::vec3 pos(
		blenderobject->loc[0] + blenderobject->dloc[0],
		blenderobject->loc[1] + blenderobject->dloc[1],
		blenderobject->loc[2] + blenderobject->dloc[2]);

	float rotmat[3][3];
	BKE_object_rot_to_mat3(blenderobject, rotmat, false);
	const mt::mat3 rotation(rotmat);

	const mt::vec3 scale(
		blenderobject->size[0] * blenderobject->dscale[0],
		blenderobject->size[1] * blenderobject->dscale[1],
		blenderobject->size[2] * blenderobject->dscale[2]);

	gameobj->NodeSetLocalPosition(pos);
	gameobj->NodeSetLocalOrientation(rotation);
	gameobj->NodeSetLocalScale(scale);
	gameobj->NodeUpdate();

	BL_ConvertProperties(blenderobject, gameobj, timemgr, kxscene, isInActiveLayer);

	gameobj->SetName(blenderobject->id.name + 2);

	// Update children/parent hierarchy.
	if (blenderobject->parent != 0) {
		// Blender has an additional 'parentinverse' offset in each object.
		SG_Callbacks callback(nullptr, nullptr, nullptr, KX_Scene::KX_ScenegraphUpdateFunc, KX_Scene::KX_ScenegraphRescheduleFunc);
		SG_Node *parentinversenode = new SG_Node(nullptr, kxscene, callback);

		// Define a normal parent relationship for this node.
		KX_NormalParentRelation *parent_relation = new KX_NormalParentRelation();
		parentinversenode->SetParentRelation(parent_relation);

		BL_ParentChildLink pclink;
		pclink.m_blenderchild = blenderobject;
		pclink.m_gamechildnode = parentinversenode;
		vec_parent_child.push_back(pclink);

		// Extract location, orientation and scale out of the inverse parent matrix.
		float invp_loc[3], invp_rot[3][3], invp_size[3];
		mat4_to_loc_rot_size(invp_loc, invp_rot, invp_size, blenderobject->parentinv);

		parentinversenode->SetLocalPosition(mt::vec3(invp_loc));
		parentinversenode->SetLocalOrientation(mt::mat3(invp_rot));
		parentinversenode->SetLocalScale(mt::vec3(invp_size));
		parentinversenode->AddChild(gameobj->GetNode());
	}

	// Needed for python scripting.
	logicmgr->RegisterGameObjectName(gameobj->GetName(), gameobj);

	// Needed for group duplication.
	logicmgr->RegisterGameObj(blenderobject, gameobj);
	for (RAS_Mesh *meshobj : gameobj->GetMeshList()) {
		logicmgr->RegisterGameMeshName(meshobj->GetName(), blenderobject);
	}

	converter.RegisterGameObject(gameobj, blenderobject);

	logicbrick_conversionlist->Add(CM_AddRef(gameobj));

	// Only draw/use objects in active 'blender' layers.
	if (isInActiveLayer) {
		objectlist->Add(CM_AddRef(gameobj));

		gameobj->NodeUpdate();
	}
	else {
		// We must store this object otherwise it will be deleted at the end of this function if it is not a root object.
		inactivelist->Add(CM_AddRef(gameobj));
	}
}


/// Convert blender objects into ketsji gameobjects.
void BL_ConvertBlenderObjects(struct Main *maggie,
                              KX_Scene *kxscene,
                              KX_KetsjiEngine *ketsjiEngine,
                              RAS_Rasterizer *rendertools,
                              RAS_ICanvas *canvas,
                              BL_SceneConverter& converter,
                              bool alwaysUseExpandFraming,
							  float camZoom,
                              bool libloading)
{

#define BL_CONVERTBLENDEROBJECT_SINGLE                                 \
	bl_ConvertBlenderObject_Single(converter,                          \
	                               blenderobject,                      \
	                               vec_parent_child,                   \
	                               logicbrick_conversionlist,          \
	                               objectlist, inactivelist, \
	                               kxscene, gameobj,                   \
	                               logicmgr, timemgr,                  \
	                               isInActiveLayer                     \
	                               )



	Scene *blenderscene = kxscene->GetBlenderScene();

	// List of groups to be converted
	std::set<Group *> grouplist;
	// All objects converted.
	std::set<Object *> allblobj;
	// Objects from groups (never in active layer).
	std::set<Object *> groupobj;

	/* We have to ensure that group definitions are only converted once
	 * push all converted group members to this set.
	 * This will happen when a group instance is made from a linked group instance
	 * and both are on the active layer. */
	EXP_ListValue<KX_GameObject> *convertedlist = new EXP_ListValue<KX_GameObject>();

	// Find out which physics engine
	PHY_IPhysicsEnvironment *phyEnv = nullptr;
	e_PhysicsEngine physicsEngine = UseBullet;

	switch (blenderscene->gm.physicsEngine) {
#ifdef WITH_BULLET
		case WOPHY_BULLET:
		{
			SYS_SystemHandle syshandle = SYS_GetSystem();
			int visualizePhysics = SYS_GetCommandLineInt(syshandle, "show_physics", 0);

			phyEnv = CcdPhysicsEnvironment::Create(blenderscene, visualizePhysics);
			physicsEngine = UseBullet;
			break;
		}
#endif
		case WOPHY_NONE:
		default:
		{
			// We should probably use some sort of factory here
			phyEnv = new DummyPhysicsEnvironment();
			physicsEngine = UseNone;
			break;
		}
	}
	kxscene->SetPhysicsEnvironment(phyEnv);


	// Get the frame settings of the canvas.
	// Get the aspect ratio of the canvas as designed by the user.
	RAS_FrameSettings::RAS_FrameType frame_type;
	int aspect_width;
	int aspect_height;

	if (alwaysUseExpandFraming) {
		frame_type = RAS_FrameSettings::e_frame_extend;
		aspect_width = canvas->GetWidth();
		aspect_height = canvas->GetHeight();
	}
	else {
		if (blenderscene->gm.framing.type == SCE_GAMEFRAMING_BARS) {
			frame_type = RAS_FrameSettings::e_frame_bars;
		}
		else if (blenderscene->gm.framing.type == SCE_GAMEFRAMING_EXTEND) {
			frame_type = RAS_FrameSettings::e_frame_extend;
		}
		else {
			frame_type = RAS_FrameSettings::e_frame_scale;
		}

		aspect_width  = (int)(blenderscene->r.xsch * blenderscene->r.xasp);
		aspect_height = (int)(blenderscene->r.ysch * blenderscene->r.yasp);
	}

	RAS_FrameSettings frame_settings(
		frame_type,
		blenderscene->gm.framing.col[0],
		blenderscene->gm.framing.col[1],
		blenderscene->gm.framing.col[2],
		aspect_width,
		aspect_height);
	kxscene->SetFramingType(frame_settings);

	kxscene->SetGravity(mt::vec3(0.0f, 0.0f, -blenderscene->gm.gravity));

	// Set activity culling parameters.
	kxscene->SetActivityCulling((blenderscene->gm.mode & WO_ACTIVITY_CULLING) != 0);
	kxscene->SetDbvtCulling((blenderscene->gm.mode & WO_DBVT_CULLING) != 0);

	// No occlusion culling by default.
	kxscene->SetDbvtOcclusionRes(0);

	if (blenderscene->gm.lodflag & SCE_LOD_USE_HYST) {
		kxscene->SetLodHysteresis(true);
		kxscene->SetLodHysteresisValue(blenderscene->gm.scehysteresis);
	}

	// Convert world.
	KX_WorldInfo *worldinfo = new KX_WorldInfo(blenderscene, blenderscene->world);
	worldinfo->UpdateWorldSettings(rendertools);
	worldinfo->UpdateBackGround(rendertools);
	kxscene->SetWorldInfo(worldinfo);

	const bool showObstacleSimulation = (blenderscene->gm.flag & GAME_SHOW_OBSTACLE_SIMULATION) != 0;
	KX_ObstacleSimulation *obstacleSimulation = nullptr;
	switch (blenderscene->gm.obstacleSimulation) {
		case OBSTSIMULATION_TOI_rays:
		{
			obstacleSimulation = new KX_ObstacleSimulationTOI_rays(blenderscene->gm.levelHeight, showObstacleSimulation);
			break;
		}
		case OBSTSIMULATION_TOI_cells:
		{
			obstacleSimulation = new KX_ObstacleSimulationTOI_cells(blenderscene->gm.levelHeight, showObstacleSimulation);
			break;
		}
	}
	kxscene->SetObstacleSimulation(obstacleSimulation);

	int activeLayerBitInfo = blenderscene->lay;

	std::vector<BL_ParentChildLink> vec_parent_child;

	EXP_ListValue<KX_GameObject> *objectlist = kxscene->GetObjectList();
	EXP_ListValue<KX_GameObject> *inactivelist = kxscene->GetInactiveList();
	EXP_ListValue<KX_GameObject> *parentlist = kxscene->GetRootParentList();

	SCA_LogicManager *logicmgr = kxscene->GetLogicManager();
	SCA_TimeEventManager *timemgr = kxscene->GetTimeEventManager();

	EXP_ListValue<KX_GameObject> *logicbrick_conversionlist = new EXP_ListValue<KX_GameObject>();

	BL_SetBlenderSceneBackground(blenderscene);

	/* Let's support scene set.
	 * Beware of name conflict in linked data, it will not crash but will create confusion
	 * in Python scripting and in certain actuators (replace mesh). Linked scene *should* have
	 * no conflicting name for Object, Object data and Action.
	 */
	Scene *sce_iter;
	Base *base;
	for (SETLOOPER(blenderscene, sce_iter, base)) {
		Object *blenderobject = base->object;
		allblobj.insert(blenderobject);

		KX_GameObject *gameobj = BL_GameObjectFromBlenderObject(base->object, kxscene, rendertools, canvas, converter, camZoom);

		bool isInActiveLayer = (blenderobject->lay & activeLayerBitInfo) != 0;
		if (gameobj) {
			// Macro calls object conversion funcs.
			BL_CONVERTBLENDEROBJECT_SINGLE;

			if (gameobj->IsDupliGroup()) {
				grouplist.insert(blenderobject->dup_group);
			}

			/* Note about memory leak issues:
			 * When a EXP_Value derived class is created, m_refcount is initialized to 1
			 * so the class must be released after being used to make sure that it won't
			 * hang in memory. If the object needs to be stored for a long time,
			 * use AddRef() so that this Release() does not free the object.
			 * Make sure that for any AddRef() there is a Release()!!!!
			 * Do the same for any object derived from EXP_Value, EXP_Expression and NG_NetworkMessage
			 */
			gameobj->Release();
		}
	}

	if (!grouplist.empty()) {
		/* Now convert the group referenced by dupli group object
		 * keep track of all groups already converted. */
		std::set<Group *> allgrouplist = grouplist;
		std::set<Group *> tempglist;
		while (!grouplist.empty()) {
			tempglist.clear();
			tempglist.swap(grouplist);
			for (Group *group : tempglist) {
				for (GroupObject *go = (GroupObject *)group->gobject.first; go; go = (GroupObject *)go->next) {
					Object *blenderobject = go->ob;
					if (!converter.FindGameObject(blenderobject)) {
						allblobj.insert(blenderobject);
						groupobj.insert(blenderobject);
						KX_GameObject *gameobj = BL_GameObjectFromBlenderObject(blenderobject, kxscene, rendertools, canvas, converter, camZoom);

						bool isInActiveLayer = false;
						if (gameobj) {
							/* Insert object to the constraint game object list
							 * so we can check later if there is a instance in the scene or
							 * an instance and its actual group definition. */
							convertedlist->Add(CM_AddRef(gameobj));

							// Macro calls object conversion funcs.
							BL_CONVERTBLENDEROBJECT_SINGLE;

							if (gameobj->IsDupliGroup()) {
								if (allgrouplist.insert(blenderobject->dup_group).second) {
									grouplist.insert(blenderobject->dup_group);
								}
							}

							gameobj->Release();
						}
					}
				}
			}
		}
	}

	// Non-camera objects not supported as camera currently.
	if (blenderscene->camera && blenderscene->camera->type == OB_CAMERA) {
		KX_Camera *gamecamera = static_cast<KX_Camera *>(converter.FindGameObject(blenderscene->camera));
		if (gamecamera) {
			kxscene->SetActiveCamera(gamecamera);
		}
	}

	// Create hierarchy information.
	for (const BL_ParentChildLink& link : vec_parent_child) {

		Object *blenderchild = link.m_blenderchild;
		Object *blenderparent = blenderchild->parent;
		KX_GameObject *parentobj = converter.FindGameObject(blenderparent);
		KX_GameObject *childobj = converter.FindGameObject(blenderchild);

		BLI_assert(childobj);

		if (!parentobj || objectlist->SearchValue(childobj) != objectlist->SearchValue(parentobj)) {
			/* Special case: the parent and child object are not in the same layer.
			 * This weird situation is used in Apricot for test purposes.
			 * Resolve it by not converting the child
			 */
			childobj->GetNode()->DisconnectFromParent();
			delete link.m_gamechildnode;
			/* Now destroy the child object but also all its descendent that may already be linked
			 * Remove the child reference in the local list!
			 * Note: there may be descendents already if the children of the child were processed
			 * by this loop before the child. In that case, we must remove the children also
			 */
			std::vector<KX_GameObject *> childrenlist = childobj->GetChildrenRecursive();
			// The returned list by GetChildrenRecursive is not owned by anyone and must not own items, so no AddRef().
			childrenlist.push_back(childobj);
			for (KX_GameObject *obj : childrenlist) {
				if (logicbrick_conversionlist->RemoveValue(obj)) {
					obj->Release();
				}
				if (convertedlist->RemoveValue(obj)) {
					obj->Release();
				}
			}

			converter.UnregisterGameObject(childobj);
			kxscene->RemoveObject(childobj);

			continue;
		}

		switch (blenderchild->partype) {
			case PARVERT1:
			{
				// Create a new vertex parent relationship for this node.
				KX_VertexParentRelation *vertex_parent_relation = new KX_VertexParentRelation();
				link.m_gamechildnode->SetParentRelation(vertex_parent_relation);
				break;
			}
			case PARSLOW:
			{
				// Create a new slow parent relationship for this node.
				KX_SlowParentRelation *slow_parent_relation = new KX_SlowParentRelation(blenderchild->sf);
				link.m_gamechildnode->SetParentRelation(slow_parent_relation);
				break;
			}
			case PARBONE:
			{
				// Parent this to a bone.
				Bone *parent_bone = BKE_armature_find_bone_name(BKE_armature_from_object(blenderchild->parent),
				                                                blenderchild->parsubstr);

				if (parent_bone) {
					KX_BoneParentRelation *bone_parent_relation = new KX_BoneParentRelation(parent_bone);
					link.m_gamechildnode->SetParentRelation(bone_parent_relation);
				}

				break;
			}
			default:
			{
				// Unhandled.
				break;
			}
		}

		parentobj->GetNode()->AddChild(link.m_gamechildnode);
	}
	vec_parent_child.clear();

	const std::vector<KX_GameObject *>& sumolist = converter.GetObjects();

	// Find 'root' parents (object that has not parents in SceneGraph).
	for (KX_GameObject *gameobj : sumolist) {
		if (!gameobj->GetNode()->GetParent()) {
			parentlist->Add(CM_AddRef(gameobj));
			gameobj->NodeUpdate();
		}
	}

	for (KX_GameObject *gameobj : objectlist) {
		// Init mesh users, mesh slots and deformers.
		gameobj->AddMeshUser();

		// Add active armature for update.
		if (gameobj->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE) {
			kxscene->AddAnimatedObject(gameobj);
		}
	}

	// Create graphic controller for culling.
	if (kxscene->GetDbvtCulling()) {
		bool occlusion = false;
		for (KX_GameObject *gameobj : sumolist) {
			// The object can't be culled ?
			if (gameobj->GetMeshList().empty() && gameobj->GetGameObjectType() != SCA_IObject::OBJ_TEXT) {
				continue;
			}

			if (physicsEngine == UseBullet) {
				const bool isactive = objectlist->SearchValue(gameobj);
				BL_CreateGraphicObjectNew(gameobj, kxscene, isactive, phyEnv);
			}
			if (gameobj->GetOccluder()) {
				occlusion = true;
			}
		}
		if (occlusion) {
			kxscene->SetDbvtOcclusionRes(blenderscene->gm.occlusionRes);
		}
	}

	if (blenderscene->world) {
		kxscene->GetPhysicsEnvironment()->SetNumTimeSubSteps(blenderscene->gm.physubstep);
	}

	for (KX_GameObject *gameobj : sumolist) {
		/* Now that the scenegraph is complete, let's instantiate the deformers.
		* We need that to create reusable derived mesh and physic shapes.
		*/
		if (gameobj->GetDeformer()) {
			gameobj->GetDeformer()->UpdateBuckets();
		}

		// Set up armature constraints and shapekey drivers.
		if (gameobj->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE) {
			BL_ArmatureObject *armobj = static_cast<BL_ArmatureObject *>(gameobj);
			armobj->LoadConstraints(converter);

			const std::vector<KX_GameObject *> children = armobj->GetChildren();
			for (KX_GameObject *child : children) {
				BL_ShapeDeformer *deformer = dynamic_cast<BL_ShapeDeformer *>(child->GetDeformer());
				if (deformer) {
					deformer->LoadShapeDrivers(armobj);
				}
			}
		}
	}

	// Create physics information.
	for (unsigned short i = 0; i < 2; ++i) {
		const bool processCompoundChildren = (i == 1);
		for (KX_GameObject *gameobj : sumolist) {
			Object *blenderobject = gameobj->GetBlenderObject();

			const std::vector<KX_Mesh *>& meshes = gameobj->GetMeshList();
			KX_Mesh *meshobj = (meshes.empty()) ? nullptr : meshes.front();

			int layerMask = (groupobj.find(blenderobject) == groupobj.end()) ? activeLayerBitInfo : 0;
			BL_CreatePhysicsObjectNew(gameobj, blenderobject, meshobj, kxscene, layerMask, converter, processCompoundChildren);
		}
	}

	// Create and set bounding volume.
	for (KX_GameObject *gameobj : sumolist) {
		Object *blenderobject = gameobj->GetBlenderObject();
		Mesh *predifinedBoundMesh = blenderobject->gamePredefinedBound;

		if (predifinedBoundMesh) {
			KX_Mesh *meshobj = converter.FindGameMesh(predifinedBoundMesh);
			// In case of mesh taken in a other scene.
			if (!meshobj) {
				continue;
			}

			gameobj->SetAutoUpdateBounds(false);

			// AABB Box : min/max.
			mt::vec3 aabbMin;
			mt::vec3 aabbMax;
			// Get the mesh bounding box for none deformer.
			RAS_BoundingBox *boundingBox = meshobj->GetBoundingBox();
			// Get the AABB.
			boundingBox->GetAabb(aabbMin, aabbMax);
			gameobj->SetBoundsAabb(aabbMin, aabbMax);
		}
		else {
			// The object allow AABB auto update only if there's no predefined bound.
			gameobj->SetAutoUpdateBounds(true);

			gameobj->UpdateBounds(true);
		}
	}

	// Create physics joints.
	for (KX_GameObject *gameobj : sumolist) {
		PHY_IPhysicsEnvironment *physEnv = kxscene->GetPhysicsEnvironment();
		Object *blenderobject = gameobj->GetBlenderObject();
		ListBase *conlist = BL_GetActiveConstraint(blenderobject);

		if (!conlist) {
			continue;
		}

		BL_ConvertObjectInfo *info = gameobj->GetConvertObjectInfo();

		for (bConstraint *curcon = (bConstraint *)conlist->first; curcon; curcon = (bConstraint *)curcon->next) {
			if (curcon->type != CONSTRAINT_TYPE_RIGIDBODYJOINT) {
				continue;
			}

			bRigidBodyJointConstraint *dat = (bRigidBodyJointConstraint *)curcon->data;

			// Skip if no target or a child object is selected or constraints are deactivated.
			if (!dat->tar || dat->child || (curcon->flag & CONSTRAINT_OFF)) {
				continue;
			}

			// Store constraints of grouped and instanced objects for all layers.
			info->m_constraints.push_back(dat);

			/** if it's during libload we only add constraints in the object but
			 * doesn't create it. Constraint will be replicated later in scene->MergeScene
			 */
			if (libloading) {
				continue;
			}

			/* Skipped already converted constraints.
			 * This will happen when a group instance is made from a linked group instance
			 * and both are on the active layer. */
			if (convertedlist->FindValue(gameobj->GetName())) {
				continue;
			}

			for (KX_GameObject *gotar : sumolist) {
				if (gotar->GetName() == (dat->tar->id.name + 2) &&
					(gotar->GetLayer() & activeLayerBitInfo) && gotar->GetPhysicsController() &&
					(gameobj->GetLayer() & activeLayerBitInfo) && gameobj->GetPhysicsController())
				{
					physEnv->SetupObjectConstraints(gameobj, gotar, dat);
					break;
				}
			}
		}
	}

	// Create object representations for obstacle simulation.
	KX_ObstacleSimulation *obssimulation = kxscene->GetObstacleSimulation();
	if (obssimulation) {
		for (KX_GameObject *gameobj : objectlist) {
			Object *blenderobject = gameobj->GetBlenderObject();
			if (blenderobject->gameflag & OB_HASOBSTACLE) {
				obssimulation->AddObstacleForObj(gameobj);
			}
		}
	}

	// Process navigation mesh objects.
	for (KX_GameObject *gameobj : objectlist) {
		Object *blenderobject = gameobj->GetBlenderObject();
		if (blenderobject->type == OB_MESH && (blenderobject->gameflag & OB_NAVMESH)) {
			KX_NavMeshObject *navmesh = static_cast<KX_NavMeshObject *>(gameobj);
			navmesh->SetVisible(false, true);
			navmesh->BuildNavMesh();
		}
	}
	for (KX_GameObject *gameobj : inactivelist) {
		Object *blenderobject = gameobj->GetBlenderObject();
		if (blenderobject->type == OB_MESH && (blenderobject->gameflag & OB_NAVMESH)) {
			KX_NavMeshObject *navmesh = static_cast<KX_NavMeshObject *>(gameobj);
			navmesh->SetVisible(false, true);
		}
	}

	// Convert logic bricks, sensors, controllers and actuators.
	for (KX_GameObject *gameobj : logicbrick_conversionlist) {
		Object *blenderobj = gameobj->GetBlenderObject();
		int layerMask = (groupobj.find(blenderobj) == groupobj.end()) ? activeLayerBitInfo : 0;
		bool isInActiveLayer = (blenderobj->lay & layerMask) != 0;
		BL_ConvertActuators(maggie->name, blenderobj, gameobj, logicmgr, kxscene, ketsjiEngine, layerMask, isInActiveLayer, converter);
	}

	for (KX_GameObject *gameobj : logicbrick_conversionlist) {
		Object *blenderobj = gameobj->GetBlenderObject();
		int layerMask = (groupobj.find(blenderobj) == groupobj.end()) ? activeLayerBitInfo : 0;
		bool isInActiveLayer = (blenderobj->lay & layerMask) != 0;
		BL_ConvertControllers(blenderobj, gameobj, logicmgr, layerMask, isInActiveLayer, converter, libloading);
	}

	for (KX_GameObject *gameobj : logicbrick_conversionlist) {
		Object *blenderobj = gameobj->GetBlenderObject();
		int layerMask = (groupobj.find(blenderobj) == groupobj.end()) ? activeLayerBitInfo : 0;
		bool isInActiveLayer = (blenderobj->lay & layerMask) != 0;
		BL_ConvertSensors(blenderobj, gameobj, logicmgr, kxscene, ketsjiEngine, layerMask, isInActiveLayer, canvas, converter);
		// Set the init state to all objects.
		gameobj->SetInitState((blenderobj->init_state) ? blenderobj->init_state : blenderobj->state);
	}

	// Apply the initial state to controllers, only on the active objects as this registers the sensors.
	for (KX_GameObject *gameobj : objectlist) {
		gameobj->ResetState();
	}

	// Cleanup converted set of group objects.
	convertedlist->Release();
	logicbrick_conversionlist->Release();

	/* Instantiate dupli group, we will loop trough the object
	 * that are in active layers. Note that duplicating group
	 * has the effect of adding objects at the end of objectlist.
	 * Only loop through the first part of the list.
	 */
	int objcount = objectlist->GetCount();
	for (unsigned int i = 0; i < objcount; ++i) {
		KX_GameObject *gameobj = objectlist->GetValue(i);
		if (gameobj->IsDupliGroup()) {
			kxscene->DupliGroupRecurse(gameobj, 0);
		}
	}
}

void BL_PostConvertBlenderObjects(KX_Scene *kxscene, const BL_SceneConverter& sceneconverter)
{
	const std::vector<KX_GameObject *>& sumolist = sceneconverter.GetObjects();
	EXP_ListValue<KX_GameObject> *objectlist = kxscene->GetObjectList();

#ifdef WITH_PYTHON

	// Convert the python components of each object if the component execution is available.
	if (G.f & G_SCRIPT_AUTOEXEC) {
		for (KX_GameObject *gameobj : sumolist) {
			Object *blenderobj = gameobj->GetBlenderObject();
			BL_ConvertComponentsObject(gameobj, blenderobj);
		}

		for (KX_GameObject *gameobj : objectlist) {
			if (gameobj->GetComponents()) {
				// Register object for component update.
				kxscene->GetPythonComponentManager().RegisterObject(gameobj);
			}
		}
	}
	else {
		CM_Warning("Python components auto-execution disabled");
	}

#endif  // WITH_PYTHON

	// Init textures for all materials.
	for (KX_BlenderMaterial *mat : sceneconverter.GetMaterials()) {
		mat->InitTextures();
	}

	// Look at every material texture and ask to create realtime map.
	for (KX_GameObject *gameobj : sumolist) {
		for (KX_Mesh *mesh : gameobj->GetMeshList()) {
			for (RAS_MeshMaterial *meshmat : mesh->GetMeshMaterialList()) {
				RAS_IMaterial *mat = meshmat->GetBucket()->GetMaterial();

				for (unsigned short k = 0; k < RAS_Texture::MaxUnits; ++k) {
					RAS_Texture *tex = mat->GetTexture(k);
					if (!tex || !tex->Ok()) {
						continue;
					}

					EnvMap *env = tex->GetTex()->env;
					if (!env || env->stype != ENV_REALT) {
						continue;
					}

					KX_GameObject *viewpoint = gameobj;
					if (env->object) {
						KX_GameObject *obj = sceneconverter.FindGameObject(env->object);
						if (obj) {
							viewpoint = obj;
						}
					}

					KX_TextureRendererManager::RendererType type = tex->IsCubeMap() ? KX_TextureRendererManager::CUBE : KX_TextureRendererManager::PLANAR;
					kxscene->GetTextureRendererManager()->AddRenderer(type, tex, viewpoint);
				}
			}
		}
	}
}

