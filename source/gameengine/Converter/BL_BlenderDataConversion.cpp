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

#include "BL_BlenderDataConversion.h"

#include "MT_Transform.h"
#include "MT_MinMax.h"

#include "GPU_texture.h"

#include "PHY_Pro.h"
#include "PHY_IPhysicsEnvironment.h"

#include "RAS_MeshObject.h"
#include "RAS_Rasterizer.h"
#include "RAS_ILightObject.h"

#include "BL_ConvertActuators.h"
#include "BL_ConvertControllers.h"
#include "BL_ConvertSensors.h"
#include "SCA_LogicManager.h"
#include "SCA_TimeEventManager.h"

#include "KX_ClientObjectInfo.h"
#include "KX_Scene.h"
#include "KX_GameObject.h"
#include "KX_Light.h"
#include "KX_Camera.h"
#include "KX_EmptyObject.h"
#include "KX_FontObject.h"
#include "KX_LodManager.h"
#include "KX_PythonComponent.h"

#include "RAS_ICanvas.h"
#include "RAS_Vertex.h"
#include "RAS_BucketManager.h"
#include "RAS_BoundingBoxManager.h"
#include "RAS_IPolygonMaterial.h"
#include "KX_BlenderMaterial.h"
#include "KX_TextureRendererManager.h"
#include "BL_Texture.h"

#include "BKE_main.h"
#include "BKE_global.h"
#include "BKE_object.h"
#include "BKE_python_component.h"
#include "BL_ModifierDeformer.h"
#include "BL_ShapeDeformer.h"
#include "BL_SkinDeformer.h"
#include "BL_MeshDeformer.h"
#include "KX_SoftBodyDeformer.h"
#include "BLI_utildefines.h"
#include "BLI_listbase.h"

#include "KX_WorldInfo.h"

#include "KX_KetsjiEngine.h"
#include "BL_BlenderSceneConverter.h"

#include "KX_Globals.h"
#include "KX_PyConstraintBinding.h"

/* This little block needed for linking to Blender... */
#ifdef WIN32
#include "BLI_winstuff.h"
#endif

/* This list includes only data type definitions */
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
#include "DNA_object_force.h"
#include "DNA_constraint_types.h"
#include "DNA_python_component_types.h"

#include "MEM_guardedalloc.h"

#include "BKE_key.h"
#include "BKE_mesh.h"

#include "BLI_math.h"

extern "C" {
#include "BKE_scene.h"
#include "BKE_customdata.h"
#include "BKE_cdderivedmesh.h"
#include "BKE_DerivedMesh.h"
#include "BKE_material.h" /* give_current_material */
#include "BKE_image.h"
#include "IMB_imbuf_types.h"
#include "BKE_displist.h"

extern Material defmaterial;	/* material.c */
}

#include "wm_event_types.h"

/* end of blender include block */

#include "BL_ConvertProperties.h"

#include "SG_Node.h"
#include "SG_BBox.h"
#include "KX_SG_NodeRelationships.h"
#include "KX_SG_BoneParentNodeRelationship.h"

#ifdef WITH_BULLET
#include "CcdPhysicsEnvironment.h"
#include "CcdGraphicController.h"
#endif

#include "KX_MotionState.h"

#include "BL_ArmatureObject.h"
#include "BL_DeformableGameObject.h"

#include "KX_NavMeshObject.h"
#include "KX_ObstacleSimulation.h"

#include "CM_Message.h"

#include "BLI_threads.h"

// For construction to find shared vertices.
struct BL_SharedVertex {
	RAS_IDisplayArray *array;
	unsigned int offset;
};

using BL_SharedVertexList = std::vector<BL_SharedVertex>;
using BL_SharedVertexMap = std::vector<BL_SharedVertexList>;

class BL_SharedVertexPredicate
{
private:
	RAS_IVertex *m_vertex;
	RAS_IDisplayArray *m_array;

public:
	BL_SharedVertexPredicate(RAS_IVertex *vertex, RAS_IDisplayArray *array)
		:m_vertex(vertex),
		m_array(array)
	{
	}

	bool operator()(const BL_SharedVertex& sharedVert) const
	{
		RAS_IDisplayArray *otherArray = sharedVert.array;
		return (m_array == otherArray) && (otherArray->GetVertexNoCache(sharedVert.offset)->closeTo(m_vertex));
	}
};

static std::map<int, SCA_IInputDevice::SCA_EnumInputs> create_translate_table()
{
	std::map<int, SCA_IInputDevice::SCA_EnumInputs> m;
		
	/* The reverse table. In order to not confuse ourselves, we      */
	/* immediately convert all events that come in to KX codes.      */
	m[LEFTMOUSE			] =	SCA_IInputDevice::LEFTMOUSE;
	m[MIDDLEMOUSE		] =	SCA_IInputDevice::MIDDLEMOUSE;
	m[RIGHTMOUSE		] =	SCA_IInputDevice::RIGHTMOUSE;
	m[WHEELUPMOUSE		] =	SCA_IInputDevice::WHEELUPMOUSE;
	m[WHEELDOWNMOUSE	] =	SCA_IInputDevice::WHEELDOWNMOUSE;
	m[MOUSEX			] = SCA_IInputDevice::MOUSEX;
	m[MOUSEY			] =	SCA_IInputDevice::MOUSEY;

	// standard keyboard                                                                                       
		
	m[AKEY				] = SCA_IInputDevice::AKEY;                  
	m[BKEY				] = SCA_IInputDevice::BKEY;                  
	m[CKEY				] = SCA_IInputDevice::CKEY;                  
	m[DKEY				] = SCA_IInputDevice::DKEY;                  
	m[EKEY				] = SCA_IInputDevice::EKEY;                  
	m[FKEY				] = SCA_IInputDevice::FKEY;                  
	m[GKEY				] = SCA_IInputDevice::GKEY;                  
	m[HKEY				] = SCA_IInputDevice::HKEY_;                  
	m[IKEY				] = SCA_IInputDevice::IKEY;                  
	m[JKEY				] = SCA_IInputDevice::JKEY;                  
	m[KKEY				] = SCA_IInputDevice::KKEY;                  
	m[LKEY				] = SCA_IInputDevice::LKEY;                  
	m[MKEY				] = SCA_IInputDevice::MKEY;                  
	m[NKEY				] = SCA_IInputDevice::NKEY;                  
	m[OKEY				] = SCA_IInputDevice::OKEY;                  
	m[PKEY				] = SCA_IInputDevice::PKEY;                  
	m[QKEY				] = SCA_IInputDevice::QKEY;                  
	m[RKEY				] = SCA_IInputDevice::RKEY;                  
	m[SKEY				] = SCA_IInputDevice::SKEY;                  
	m[TKEY				] = SCA_IInputDevice::TKEY;                  
	m[UKEY				] = SCA_IInputDevice::UKEY;                  
	m[VKEY				] = SCA_IInputDevice::VKEY;                  
	m[WKEY				] = SCA_IInputDevice::WKEY;                  
	m[XKEY				] = SCA_IInputDevice::XKEY;                  
	m[YKEY				] = SCA_IInputDevice::YKEY;                  
	m[ZKEY				] = SCA_IInputDevice::ZKEY;                  
		
	m[ZEROKEY			] = SCA_IInputDevice::ZEROKEY;                  
	m[ONEKEY			] = SCA_IInputDevice::ONEKEY;                  
	m[TWOKEY			] = SCA_IInputDevice::TWOKEY;                  
	m[THREEKEY			] = SCA_IInputDevice::THREEKEY;                  
	m[FOURKEY			] = SCA_IInputDevice::FOURKEY;                  
	m[FIVEKEY			] = SCA_IInputDevice::FIVEKEY;                  
	m[SIXKEY			] = SCA_IInputDevice::SIXKEY;                  
	m[SEVENKEY			] = SCA_IInputDevice::SEVENKEY;                  
	m[EIGHTKEY			] = SCA_IInputDevice::EIGHTKEY;                  
	m[NINEKEY			] = SCA_IInputDevice::NINEKEY;                  
		
	m[CAPSLOCKKEY		] = SCA_IInputDevice::CAPSLOCKKEY;                  
		
	m[LEFTCTRLKEY		] = SCA_IInputDevice::LEFTCTRLKEY;                  
	m[LEFTALTKEY		] = SCA_IInputDevice::LEFTALTKEY;                  
	m[RIGHTALTKEY		] = SCA_IInputDevice::RIGHTALTKEY;                  
	m[RIGHTCTRLKEY		] = SCA_IInputDevice::RIGHTCTRLKEY;                  
	m[RIGHTSHIFTKEY		] = SCA_IInputDevice::RIGHTSHIFTKEY;                  
	m[LEFTSHIFTKEY		] = SCA_IInputDevice::LEFTSHIFTKEY;                  
		
	m[ESCKEY			] = SCA_IInputDevice::ESCKEY;                  
	m[TABKEY			] = SCA_IInputDevice::TABKEY;                  
	m[RETKEY			] = SCA_IInputDevice::RETKEY;                  
	m[SPACEKEY			] = SCA_IInputDevice::SPACEKEY;                  
	m[LINEFEEDKEY		] = SCA_IInputDevice::LINEFEEDKEY;                  
	m[BACKSPACEKEY		] = SCA_IInputDevice::BACKSPACEKEY;                  
	m[DELKEY			] = SCA_IInputDevice::DELKEY;                  
	m[SEMICOLONKEY		] = SCA_IInputDevice::SEMICOLONKEY;                  
	m[PERIODKEY			] = SCA_IInputDevice::PERIODKEY;                  
	m[COMMAKEY			] = SCA_IInputDevice::COMMAKEY;                  
	m[QUOTEKEY			] = SCA_IInputDevice::QUOTEKEY;                  
	m[ACCENTGRAVEKEY	] = SCA_IInputDevice::ACCENTGRAVEKEY;                  
	m[MINUSKEY			] = SCA_IInputDevice::MINUSKEY;                  
	m[SLASHKEY			] = SCA_IInputDevice::SLASHKEY;
	m[BACKSLASHKEY		] = SCA_IInputDevice::BACKSLASHKEY;                  
	m[EQUALKEY			] = SCA_IInputDevice::EQUALKEY;                  
	m[LEFTBRACKETKEY	] = SCA_IInputDevice::LEFTBRACKETKEY;                  
	m[RIGHTBRACKETKEY	] = SCA_IInputDevice::RIGHTBRACKETKEY;                  
		
	m[LEFTARROWKEY		] = SCA_IInputDevice::LEFTARROWKEY;                  
	m[DOWNARROWKEY		] = SCA_IInputDevice::DOWNARROWKEY;                  
	m[RIGHTARROWKEY		] = SCA_IInputDevice::RIGHTARROWKEY;                  
	m[UPARROWKEY		] = SCA_IInputDevice::UPARROWKEY;                  
		
	m[PAD2				] = SCA_IInputDevice::PAD2;                  
	m[PAD4				] = SCA_IInputDevice::PAD4;                  
	m[PAD6				] = SCA_IInputDevice::PAD6;                  
	m[PAD8				] = SCA_IInputDevice::PAD8;                  
		
	m[PAD1				] = SCA_IInputDevice::PAD1;                  
	m[PAD3				] = SCA_IInputDevice::PAD3;                  
	m[PAD5				] = SCA_IInputDevice::PAD5;                  
	m[PAD7				] = SCA_IInputDevice::PAD7;                  
	m[PAD9				] = SCA_IInputDevice::PAD9;                  
		
	m[PADPERIOD			] = SCA_IInputDevice::PADPERIOD;                  
	m[PADSLASHKEY		] = SCA_IInputDevice::PADSLASHKEY;                  
	m[PADASTERKEY		] = SCA_IInputDevice::PADASTERKEY;                  
		
	m[PAD0				] = SCA_IInputDevice::PAD0;                  
	m[PADMINUS			] = SCA_IInputDevice::PADMINUS;                  
	m[PADENTER			] = SCA_IInputDevice::PADENTER;                  
	m[PADPLUSKEY		] = SCA_IInputDevice::PADPLUSKEY;                  
		
		
	m[F1KEY				] = SCA_IInputDevice::F1KEY;                  
	m[F2KEY				] = SCA_IInputDevice::F2KEY;                  
	m[F3KEY				] = SCA_IInputDevice::F3KEY;                  
	m[F4KEY				] = SCA_IInputDevice::F4KEY;                  
	m[F5KEY				] = SCA_IInputDevice::F5KEY;                  
	m[F6KEY				] = SCA_IInputDevice::F6KEY;                  
	m[F7KEY				] = SCA_IInputDevice::F7KEY;                  
	m[F8KEY				] = SCA_IInputDevice::F8KEY;                  
	m[F9KEY				] = SCA_IInputDevice::F9KEY;                  
	m[F10KEY			] = SCA_IInputDevice::F10KEY;                  
	m[F11KEY			] = SCA_IInputDevice::F11KEY;                  
	m[F12KEY			] = SCA_IInputDevice::F12KEY;
	m[F13KEY			] = SCA_IInputDevice::F13KEY;
	m[F14KEY			] = SCA_IInputDevice::F14KEY;
	m[F15KEY			] = SCA_IInputDevice::F15KEY;
	m[F16KEY			] = SCA_IInputDevice::F16KEY;
	m[F17KEY			] = SCA_IInputDevice::F17KEY;
	m[F18KEY			] = SCA_IInputDevice::F18KEY;
	m[F19KEY			] = SCA_IInputDevice::F19KEY;

	m[OSKEY				] = SCA_IInputDevice::OSKEY;

	m[PAUSEKEY			] = SCA_IInputDevice::PAUSEKEY;                  
	m[INSERTKEY			] = SCA_IInputDevice::INSERTKEY;                  
	m[HOMEKEY			] = SCA_IInputDevice::HOMEKEY;                  
	m[PAGEUPKEY			] = SCA_IInputDevice::PAGEUPKEY;                  
	m[PAGEDOWNKEY		] = SCA_IInputDevice::PAGEDOWNKEY;                  
	m[ENDKEY			] = SCA_IInputDevice::ENDKEY;

	return m;
}

static std::map<int, SCA_IInputDevice::SCA_EnumInputs> gReverseKeyTranslateTable = create_translate_table();

SCA_IInputDevice::SCA_EnumInputs ConvertKeyCode(int key_code)
{
	return gReverseKeyTranslateTable[key_code];
}

static void GetUvRgba(const RAS_MeshObject::LayerList& layers, std::vector<MLoopUV *>& uvLayers, std::vector<MLoopCol *>& colorLayers,
		unsigned short uvCount, unsigned short colorCount,
		unsigned int loop, MT_Vector2 uvs[RAS_Texture::MaxUnits], unsigned int rgba[RAS_IVertex::MAX_UNIT])
{
	// No need to initialize layers to zero as all the converted layer are all the layers needed.

	for (const RAS_MeshObject::Layer& layer : layers) {
		const unsigned short index = layer.index;
		switch (layer.type) {
			case RAS_MeshObject::Layer::COLOR:
			{
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
				break;
			}
			case RAS_MeshObject::Layer::UV:
			{
				const MLoopUV& uv = uvLayers[index][loop];
				uvs[index].setValue(uv.uv);
				break;
			}
		}
	}

	/* All vertices have at least one uv and color layer accessible to the user
	 * even if it they are not used in any shaders. Initialize this layer to zero
	 * when no uv or color layer exist.
	 */
	if (uvCount == 0) {
		uvs[0] = MT_Vector2(0.0f, 0.0f);
	}
	if (colorCount == 0) {
		rgba[0] = 0xFFFFFFFF;
	}
}

static KX_BlenderMaterial *ConvertMaterial(Material *mat, int lightlayer)
{
	std::string name = mat->id.name;
	// Always ensure that the name of a material start with "MA" prefix due to video texture name check.
	if (name.empty()) {
		name = "MA";
	}

	KX_BlenderMaterial *kx_blmat = new KX_BlenderMaterial(mat, name, lightlayer);

	return kx_blmat;
}

static RAS_MaterialBucket *material_from_mesh(Material *ma, int lightlayer, KX_Scene *scene, BL_BlenderSceneConverter& converter)
{
	KX_BlenderMaterial* mat = converter.FindMaterial(ma);

	if (!mat) {
		mat = ConvertMaterial(ma, lightlayer);
		// this is needed to free up memory afterwards.
		converter.RegisterMaterial(mat, ma);
	}

	// see if a bucket was reused or a new one was created
	// this way only one KX_BlenderMaterial object has to exist per bucket
	bool bucketCreated;
	RAS_MaterialBucket* bucket = scene->FindBucket(mat, bucketCreated);

	return bucket;
}

/* blenderobj can be nullptr, make sure its checked for */
RAS_MeshObject* BL_ConvertMesh(Mesh *me, Object *blenderobj, KX_Scene *scene, BL_BlenderSceneConverter& converter)
{
	RAS_MeshObject *meshobj;
	const int lightlayer = blenderobj ? blenderobj->lay : (1 << 20) -1; // all layers if no object.

	// Without checking names, we get some reuse we don't want that can cause
	// problems with material LoDs.
	if (blenderobj && ((meshobj = converter.FindGameMesh(me/*, ob->lay*/)) != nullptr)) {
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

	RAS_MeshObject::LayersInfo layersInfo;
	layersInfo.activeUv = (activeUv == -1) ? 0 : activeUv;
	layersInfo.activeColor = (activeColor == -1) ? 0 : activeColor;
	layersInfo.uvCount = uvCount;
	layersInfo.colorCount = colorCount;

	// Extract UV loops.
	for (unsigned short i = 0; i < uvCount; ++i) {
		const std::string name = CustomData_get_layer_name(&dm->loopData, CD_MLOOPUV, i);
		layersInfo.layers.push_back({RAS_MeshObject::Layer::UV, i, name});
	}
	// Extract color loops.
	for (unsigned short i = 0; i < colorCount; ++i) {
		const std::string name = CustomData_get_layer_name(&dm->loopData, CD_MLOOPCOL, i);
		layersInfo.layers.push_back({RAS_MeshObject::Layer::COLOR, i, name});
	}

	// Initialize vertex format with used uv and color layers.
	RAS_VertexFormat vertformat;
	vertformat.uvSize = max_ii(1, uvCount);
	vertformat.colorSize = max_ii(1, colorCount);

	meshobj = new RAS_MeshObject(me, layersInfo);

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

		RAS_MaterialBucket *bucket = material_from_mesh(ma, lightlayer, scene, converter);
		RAS_MeshMaterial *meshmat = meshobj->AddMaterial(bucket, i, vertformat);
		RAS_IPolyMaterial *mat = meshmat->GetBucket()->GetPolyMaterial();

		mats[i] = {meshmat->GetDisplayArray(), bucket, mat->IsVisible(), mat->IsTwoSided(), mat->IsCollider(), mat->IsWire()};
	}

	BL_ConvertDerivedMeshToArray(dm, me, mats, layersInfo);

	meshobj->EndConversion(scene->GetBoundingBoxManager());

	dm->release(dm);

	converter.RegisterGameMesh(meshobj, me);
	return meshobj;
}

void BL_ConvertDerivedMeshToArray(DerivedMesh *dm, Mesh *me, const std::vector<BL_MeshMaterial>& mats,
		const RAS_MeshObject::LayersInfo& layersInfo)
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
	const float (*normals)[3] = (float (*)[3])dm->getLoopDataArray(dm, CD_NORMAL);

	float (*tangent)[4] = nullptr;
	if (layersInfo.uvCount > 0) {
		if (CustomData_get_layer_index(&dm->loopData, CD_TANGENT) == -1) {
			DM_calc_loop_tangents(dm, true, nullptr, 0);
		}
		tangent = (float(*)[4])dm->getLoopDataArray(dm, CD_TANGENT);
	}

	// List of MLoopUV per uv layer index.
	std::vector<MLoopUV *> uvLayers(layersInfo.uvCount);
	// List of MLoopCol per color layer index.
	std::vector<MLoopCol *> colorLayers(layersInfo.colorCount);

	for (const RAS_MeshObject::Layer& layer : layersInfo.layers) {
		const unsigned short index = layer.index;
		switch (layer.type) {
			case RAS_MeshObject::Layer::UV:
			{
				uvLayers[index] = (MLoopUV *)CustomData_get_layer_n(&dm->loopData, CD_MLOOPUV, index);
				break;
			}
			case RAS_MeshObject::Layer::COLOR:
			{
				colorLayers[index] = (MLoopCol *)CustomData_get_layer_n(&dm->loopData, CD_MLOOPCOL, index);
				break;
			}
		}
	}


	BL_SharedVertexMap sharedMap(totverts);

	// Tracked vertices during a mpoly conversion, should never be used by the next mpoly.
	std::vector<unsigned int> vertices(totverts, -1);

	for (unsigned int i = 0; i < numpolys; ++i) {
		const MPoly& mpoly = mpolys[i];

		const BL_MeshMaterial& mat = mats[mpoly.mat_nr];
		RAS_IDisplayArray *array = mat.array;

		// Mark face as flat, so vertices are split.
		const bool flat = (mpoly.flag & ME_SMOOTH) == 0;

		const unsigned int lpstart = mpoly.loopstart;
		const unsigned int totlp = mpoly.totloop;
		for (unsigned int j = lpstart; j < lpstart + totlp; ++j) {
			const MLoop& mloop = mloops[j];
			const unsigned int vertid = mloop.v;
			const MVert& mvert = mverts[vertid];

			const MT_Vector3 pt(mvert.co);
			const MT_Vector3 no(normals[j]);
			const MT_Vector4 tan = tangent ? MT_Vector4(tangent[j]) : MT_Vector4(0.0f, 0.0f, 0.0f, 0.0f);
			MT_Vector2 uvs[RAS_Texture::MaxUnits];
			unsigned int rgba[RAS_Texture::MaxUnits];

			GetUvRgba(layersInfo.layers, uvLayers, colorLayers, layersInfo.uvCount, layersInfo.colorCount, j, uvs, rgba);

			RAS_IVertex *vertex = array->CreateVertex(pt, uvs, tan, rgba, no);

			BL_SharedVertexList& sharedList = sharedMap[vertid];
			BL_SharedVertexList::iterator it = std::find_if(sharedList.begin(), sharedList.end(),
					BL_SharedVertexPredicate(vertex, array));

			unsigned int offset;
			if (it != sharedList.end()) {
				offset = it->offset;
			}
			else {
				offset = array->AddVertex(vertex);
				const RAS_VertexInfo info(vertid, flat);
				array->AddVertexInfo(info);
				sharedList.push_back({array, offset});
			}

			// Add tracked vertices by the mpoly.
			vertices[vertid] = offset;
			delete vertex;
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

static PHY_ShapeProps *CreateShapePropsFromBlenderObject(struct Object* blenderobject)
{
	PHY_ShapeProps *shapeProps = new PHY_ShapeProps;
	
	BLI_assert(shapeProps);
		
	shapeProps->m_mass = blenderobject->mass;
	
//  This needs to be fixed in blender. For now, we use:
	
// in Blender, inertia stands for the size value which is equivalent to
// the sphere radius
	shapeProps->m_inertia = blenderobject->formfactor;
	
	BLI_assert(0.0f <= blenderobject->damping && blenderobject->damping <= 1.0f);
	BLI_assert(0.0f <= blenderobject->rdamping && blenderobject->rdamping <= 1.0f);
	
	shapeProps->m_lin_drag = 1.0f - blenderobject->damping;
	shapeProps->m_ang_drag = 1.0f - blenderobject->rdamping;
	
	shapeProps->m_friction_scaling = MT_Vector3(blenderobject->anisotropicFriction);
	shapeProps->m_do_anisotropic = ((blenderobject->gameflag & OB_ANISOTROPIC_FRICTION) != 0);
	
	shapeProps->m_do_fh     = (blenderobject->gameflag & OB_DO_FH) != 0; 
	shapeProps->m_do_rot_fh = (blenderobject->gameflag & OB_ROT_FH) != 0;
	
//	velocity clamping XXX
	shapeProps->m_clamp_vel_min = blenderobject->min_vel;
	shapeProps->m_clamp_vel_max = blenderobject->max_vel;
	shapeProps->m_clamp_angvel_min = blenderobject->min_angvel;
	shapeProps->m_clamp_angvel_max = blenderobject->max_angvel;

//  Character physics properties
	shapeProps->m_step_height = blenderobject->step_height;
	shapeProps->m_jump_speed = blenderobject->jump_speed;
	shapeProps->m_fall_speed = blenderobject->fall_speed;
	shapeProps->m_max_jumps = blenderobject->max_jumps;

	shapeProps->m_restitution = blenderobject->reflect;
	shapeProps->m_friction = blenderobject->friction;
	shapeProps->m_rollingFriction = blenderobject->rolling_friction;
	shapeProps->m_fh_spring = blenderobject->fh;
	shapeProps->m_fh_damping = blenderobject->xyfrict;
	shapeProps->m_fh_distance = blenderobject->fhdist;
	shapeProps->m_fh_normal = (blenderobject->dynamode & OB_FH_NOR) != 0;

	return shapeProps;
}

//////////////////////////////////////////////////////


static void BL_CreateGraphicObjectNew(KX_GameObject* gameobj,
                                      KX_Scene* kxscene,
                                      bool isActive,
                                      e_PhysicsEngine physics_engine)
{
	switch (physics_engine)
	{
#ifdef WITH_BULLET
	case UseBullet:
		{
			CcdPhysicsEnvironment* env = (CcdPhysicsEnvironment*)kxscene->GetPhysicsEnvironment();
			BLI_assert(env);
			PHY_IMotionState* motionstate = new KX_MotionState(gameobj->GetSGNode());
			CcdGraphicController* ctrl = new CcdGraphicController(env, motionstate);
			gameobj->SetGraphicController(ctrl);
			ctrl->SetNewClientInfo(gameobj->getClientInfo());
			if (isActive) {
				// add first, this will create the proxy handle, only if the object is visible
				if (gameobj->GetVisible())
					env->AddCcdGraphicController(ctrl);
			}
		}
		break;
#endif
	default:
		break;
	}
}

static void BL_CreatePhysicsObjectNew(KX_GameObject* gameobj,
                                      struct Object* blenderobject,
                                      RAS_MeshObject* meshobj,
                                      KX_Scene* kxscene,
                                      int activeLayerBitInfo,
                                      BL_BlenderSceneConverter& converter,
                                      bool processCompoundChildren
                                      )

{
	//SYS_SystemHandle syshandle = SYS_GetSystem(); /*unused*/
	//int userigidbody = SYS_GetCommandLineInt(syshandle,"norigidbody",0);
	//bool bRigidBody = (userigidbody == 0);

	// object has physics representation?
	if (!(blenderobject->gameflag & OB_COLLISION)) {
		// Respond to all collisions so that Near sensors work on No Collision
		// objects.
		gameobj->SetUserCollisionGroup(0xffff);
		gameobj->SetUserCollisionMask(0xffff);
		return;
	}

	gameobj->SetUserCollisionGroup(blenderobject->col_group);
	gameobj->SetUserCollisionMask(blenderobject->col_mask);

	Object *parent = blenderobject->parent;

	bool isCompoundChild = false;
	bool hasCompoundChildren = false;

	// Pretend for compound parent or child if the object has compound option and use a physics type with solid shape.
	if ((blenderobject->gameflag & (OB_CHILD)) && (blenderobject->gameflag & (OB_DYNAMIC | OB_COLLISION | OB_RIGID_BODY)) &&
		!(blenderobject->gameflag & OB_SOFT_BODY))
	{
		hasCompoundChildren = true;
		while (parent) {
			if ((parent->gameflag & OB_CHILD) && (parent->gameflag & (OB_COLLISION | OB_DYNAMIC | OB_RIGID_BODY)) &&
				!(parent->gameflag & OB_SOFT_BODY))
			{
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

	PHY_ShapeProps* shapeprops =
			CreateShapePropsFromBlenderObject(blenderobject);

	class PHY_IMotionState* motionstate = new KX_MotionState(gameobj->GetSGNode());

	kxscene->GetPhysicsEnvironment()->ConvertObject(converter, gameobj, meshobj, kxscene, shapeprops, motionstate, activeLayerBitInfo, isCompoundChild, hasCompoundChildren);

	bool isActor = (blenderobject->gameflag & OB_ACTOR)!=0;
	bool isSensor = (blenderobject->gameflag & OB_SENSOR) != 0;
	gameobj->getClientInfo()->m_type =
		(isSensor) ? ((isActor) ? KX_ClientObjectInfo::OBACTORSENSOR : KX_ClientObjectInfo::OBSENSOR) :
		(isActor) ? KX_ClientObjectInfo::ACTOR : KX_ClientObjectInfo::STATIC;

	delete shapeprops;
}

static KX_LodManager *lodmanager_from_blenderobject(Object *ob, KX_Scene *scene, BL_BlenderSceneConverter& converter)
{
	if (BLI_listbase_count_ex(&ob->lodlevels, 2) <= 1) {
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

static KX_LightObject *gamelight_from_blamp(Object *ob, Lamp *la, unsigned int layerflag, KX_Scene *kxscene, RAS_Rasterizer *rasterizer, BL_BlenderSceneConverter& converter)
{
	RAS_ILightObject *lightobj = rasterizer->CreateLight();
	KX_LightObject *gamelight;
	
	lightobj->m_att1 = la->att1;
	lightobj->m_att2 = (la->mode & LA_QUAD) ? la->att2 : 0.0f;
	lightobj->m_coeff_const = la->coeff_const;
	lightobj->m_coeff_lin = la->coeff_lin;
	lightobj->m_coeff_quad = la->coeff_quad;
	lightobj->m_color[0] = la->r;
	lightobj->m_color[1] = la->g;
	lightobj->m_color[2] = la->b;
	lightobj->m_distance = la->dist;
	lightobj->m_energy = la->energy;
	lightobj->m_shadowclipstart = la->clipsta;
	lightobj->m_shadowclipend = la->clipend;
	lightobj->m_shadowbias = la->bias;
	lightobj->m_shadowbleedbias = la->bleedbias;
	lightobj->m_shadowmaptype = la->shadowmap_type;
	lightobj->m_shadowfrustumsize = la->shadow_frustum_size;
	lightobj->m_shadowcolor[0] = la->shdwr;
	lightobj->m_shadowcolor[1] = la->shdwg;
	lightobj->m_shadowcolor[2] = la->shdwb;
	lightobj->m_layer = layerflag;
	lightobj->m_spotblend = la->spotblend;
	lightobj->m_spotsize = la->spotsize;
	lightobj->m_staticShadow = la->mode & LA_STATIC_SHADOW;
	// Set to true to make at least one shadow render in static mode.
	lightobj->m_requestShadowUpdate = true;

	lightobj->m_nodiffuse = (la->mode & LA_NO_DIFF) != 0;
	lightobj->m_nospecular = (la->mode & LA_NO_SPEC) != 0;

	if (la->type == LA_SUN) {
		lightobj->m_type = RAS_ILightObject::LIGHT_SUN;
	}
	else if (la->type == LA_SPOT) {
		lightobj->m_type = RAS_ILightObject::LIGHT_SPOT;
	}
	else if (la->type == LA_HEMI) {
		lightobj->m_type = RAS_ILightObject::LIGHT_HEMI;
	}
	else {
		lightobj->m_type = RAS_ILightObject::LIGHT_NORMAL;
	}

	gamelight = new KX_LightObject(kxscene, KX_Scene::m_callbacks, rasterizer, lightobj);

	gamelight->SetShowShadowFrustum((la->mode & LA_SHOW_SHADOW_BOX) && (la->mode & LA_SHAD_RAY));

	return gamelight;
}

static KX_Camera *gamecamera_from_bcamera(Object *ob, KX_Scene *kxscene, BL_BlenderSceneConverter& converter)
{
	Camera* ca = static_cast<Camera*>(ob->data);
	RAS_CameraData camdata(ca->lens, ca->ortho_scale, ca->sensor_x, ca->sensor_y, ca->sensor_fit, ca->shiftx, ca->shifty, ca->clipsta, ca->clipend, ca->type == CAM_PERSP, ca->YF_dofdist);
	KX_Camera *gamecamera;
	
	gamecamera= new KX_Camera(kxscene, KX_Scene::m_callbacks, camdata);
	gamecamera->SetName(ca->id.name + 2);

	gamecamera->SetShowCameraFrustum(ca->gameflag & GAME_CAM_SHOW_FRUSTUM);
	gamecamera->SetLodDistanceFactor(ca->lodfactor);

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

static KX_GameObject *gameobject_from_blenderobject(Object *ob, KX_Scene *kxscene, RAS_Rasterizer *rendertools,
		BL_BlenderSceneConverter &converter) 
{
	KX_GameObject *gameobj = nullptr;
	Scene *blenderscene = kxscene->GetBlenderScene();
	
	switch (ob->type) {
	case OB_LAMP:
	{
		KX_LightObject* gamelight = gamelight_from_blamp(ob, static_cast<Lamp*>(ob->data), ob->lay, kxscene, rendertools, converter);
		gameobj = gamelight;
		gamelight->AddRef();
		kxscene->GetLightList()->Add(gamelight);

		break;
	}
	
	case OB_CAMERA:
	{
		KX_Camera* gamecamera = gamecamera_from_bcamera(ob, kxscene, converter);
		gameobj = gamecamera;
		
		//don't add a reference: the camera list in kxscene->m_cameras is not released at the end
		//gamecamera->AddRef();
		kxscene->GetCameraList()->Add(CM_AddRef(gamecamera));
		
		break;
	}
	
	case OB_MESH:
	{
		Mesh* mesh = static_cast<Mesh*>(ob->data);
		RAS_MeshObject* meshobj = BL_ConvertMesh(mesh,ob,kxscene,converter);
		
		// needed for python scripting
		kxscene->GetLogicManager()->RegisterMeshName(meshobj->GetName(),meshobj);

		if (ob->gameflag & OB_NAVMESH)
		{
			gameobj = new KX_NavMeshObject(kxscene,KX_Scene::m_callbacks);
			gameobj->AddMesh(meshobj);
			break;
		}

		gameobj = new BL_DeformableGameObject(kxscene, KX_Scene::m_callbacks);
	
		// set transformation
		gameobj->AddMesh(meshobj);

		// gather levels of detail
		KX_LodManager *lodManager = lodmanager_from_blenderobject(ob, kxscene, converter);
		gameobj->SetLodManager(lodManager);
		if (lodManager) {
			lodManager->Release();
		}

		// for all objects: check whether they want to
		// respond to updates
		bool ignoreActivityCulling =  
			((ob->gameflag2 & OB_NEVER_DO_ACTIVITY_CULLING)!=0);
		gameobj->SetIgnoreActivityCulling(ignoreActivityCulling);
		gameobj->SetOccluder((ob->gameflag & OB_OCCLUDER) != 0, false);

		// two options exists for deform: shape keys and armature
		// only support relative shape key
		bool bHasShapeKey = mesh->key != nullptr && mesh->key->type==KEY_RELATIVE;
		bool bHasDvert = mesh->dvert != nullptr && ob->defbase.first;
		bool bHasArmature = (BL_ModifierDeformer::HasArmatureDeformer(ob) && ob->parent && ob->parent->type == OB_ARMATURE && bHasDvert);
		bool bHasModifier = BL_ModifierDeformer::HasCompatibleDeformer(ob);
#ifdef WITH_BULLET
		bool bHasSoftBody = (!ob->parent && (ob->gameflag & OB_SOFT_BODY));
#endif

		RAS_Deformer *deformer = nullptr;
		BL_DeformableGameObject *deformableGameObj = (BL_DeformableGameObject *)gameobj;

		if (bHasModifier) {
			deformer = new BL_ModifierDeformer(deformableGameObj, kxscene->GetBlenderScene(), ob, meshobj);
		}
		else if (bHasShapeKey) {
			// not that we can have shape keys without dvert! 
			deformer = new BL_ShapeDeformer(deformableGameObj, ob, meshobj);
		}
		else if (bHasArmature) {
			deformer = new BL_SkinDeformer(deformableGameObj, ob, meshobj, nullptr);
		}
		else if (bHasDvert) {
			// this case correspond to a mesh that can potentially deform but not with the
			// object to which it is attached for the moment. A skin mesh was created in
			// BL_ConvertMesh() so must create a deformer too!
			deformer = new BL_MeshDeformer(deformableGameObj, ob, meshobj);
		}
#ifdef WITH_BULLET
		else if (bHasSoftBody) {
			deformer = new KX_SoftBodyDeformer(meshobj, deformableGameObj);
		}
#endif

		if (deformer) {
			deformableGameObj->SetDeformer(deformer);
		}
		break;
	}
	
	case OB_ARMATURE:
	{
		bArmature *arm = (bArmature*)ob->data;
		gameobj = new BL_ArmatureObject(
			kxscene,
			KX_Scene::m_callbacks,
			ob,
			kxscene->GetBlenderScene(), // handle
			arm->gevertdeformer
		);

		kxscene->AddAnimatedObject(gameobj);

		break;
	}
	
	case OB_EMPTY:
	{
		gameobj = new KX_EmptyObject(kxscene,KX_Scene::m_callbacks);
		// set transformation
		break;
	}

	case OB_FONT:
	{
		bool do_color_management = BKE_scene_check_color_management_enabled(blenderscene);
		/* font objects have no bounding box */
		KX_FontObject *fontobj = new KX_FontObject(kxscene,KX_Scene::m_callbacks, rendertools, kxscene->GetBoundingBoxManager(), ob, do_color_management);
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
	}
#endif

	}
	if (gameobj) 
	{
		gameobj->SetLayer(ob->lay);
		gameobj->SetBlenderObject(ob);
		gameobj->SetObjectColor(MT_Vector4(ob->col));
		/* set the visibility state based on the objects render option in the outliner */
		if (ob->restrictflag & OB_RESTRICT_RENDER) gameobj->SetVisible(0, 0);
	}
	return gameobj;
}

struct parentChildLink {
	struct Object* m_blenderchild;
	SG_Node* m_gamechildnode;
};

static bPoseChannel *get_active_posechannel2(Object *ob)
{
	bArmature *arm= (bArmature*)ob->data;
	bPoseChannel *pchan;
	
	/* find active */
	for (pchan= (bPoseChannel *)ob->pose->chanbase.first; pchan; pchan= pchan->next) {
		if (pchan->bone && (pchan->bone == arm->act_bone) && (pchan->bone->layer & arm->layer))
			return pchan;
	}
	
	return nullptr;
}

static ListBase *get_active_constraints2(Object *ob)
{
	if (!ob)
		return nullptr;

  // XXX - shouldnt we care about the pose data and not the mode???
	if (ob->mode & OB_MODE_POSE) { 
		bPoseChannel *pchan;

		pchan = get_active_posechannel2(ob);
		if (pchan)
			return &pchan->constraints;
	}
	else 
		return &ob->constraints;

	return nullptr;
}

static void UNUSED_FUNCTION(print_active_constraints2)(Object *ob) //not used, use to debug
{
	bConstraint* curcon;
	ListBase* conlist = get_active_constraints2(ob);

	if (conlist) {
		for (curcon = (bConstraint *)conlist->first; curcon; curcon = (bConstraint *)curcon->next) {
			CM_Debug(curcon->type);
		}
	}
}

// Copy base layer to object layer like in BKE_scene_set_background
static void blenderSceneSetBackground(Scene *blenderscene)
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

	CListValue<KX_PythonComponent> *components = new CListValue<KX_PythonComponent>();

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
		if (!PyType_Check(cls) || !PyObject_IsSubclass(cls, (PyObject*)&KX_PythonComponent::Type)) {
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
			KX_PythonComponent *comp = static_cast<KX_PythonComponent *>(BGE_PROXY_REF(pycomp));
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
static void bl_ConvertBlenderObject_Single(
        BL_BlenderSceneConverter& converter,
       Object *blenderobject,
        std::vector<parentChildLink> &vec_parent_child,
        CListValue<KX_GameObject> *logicbrick_conversionlist,
        CListValue<KX_GameObject> *objectlist, CListValue<KX_GameObject> *inactivelist,
		CListValue<KX_GameObject> *sumolist,
        KX_Scene* kxscene, KX_GameObject* gameobj,
        SCA_LogicManager* logicmgr, SCA_TimeEventManager* timemgr,
        bool isInActiveLayer
        )
{
	MT_Vector3 pos(
		blenderobject->loc[0]+blenderobject->dloc[0],
		blenderobject->loc[1]+blenderobject->dloc[1],
		blenderobject->loc[2]+blenderobject->dloc[2]
	);

	MT_Matrix3x3 rotation;
	float rotmat[3][3];
	BKE_object_rot_to_mat3(blenderobject, rotmat, false);
	rotation.setValue3x3((float*)rotmat);

	MT_Vector3 scale(blenderobject->size);

	gameobj->NodeSetLocalPosition(pos);
	gameobj->NodeSetLocalOrientation(rotation);
	gameobj->NodeSetLocalScale(scale);
	gameobj->NodeUpdateGS(0);

	sumolist->Add(CM_AddRef(gameobj));

	BL_ConvertProperties(blenderobject,gameobj,timemgr,kxscene,isInActiveLayer);

	gameobj->SetName(blenderobject->id.name + 2);

	// update children/parent hierarchy
	if (blenderobject->parent != 0)
	{
		// blender has an additional 'parentinverse' offset in each object
		SG_Callbacks callback(nullptr,nullptr,nullptr,KX_Scene::KX_ScenegraphUpdateFunc,KX_Scene::KX_ScenegraphRescheduleFunc);
		SG_Node* parentinversenode = new SG_Node(nullptr,kxscene,callback);

		// define a normal parent relationship for this node.
		KX_NormalParentRelation * parent_relation = KX_NormalParentRelation::New();
		parentinversenode->SetParentRelation(parent_relation);

		parentChildLink pclink;
		pclink.m_blenderchild = blenderobject;
		pclink.m_gamechildnode = parentinversenode;
		vec_parent_child.push_back(pclink);

		float* fl = (float*) blenderobject->parentinv;
		MT_Transform parinvtrans(fl);
		parentinversenode->SetLocalPosition(parinvtrans.getOrigin());
		// problem here: the parent inverse transform combines scaling and rotation
		// in the basis but the scenegraph needs separate rotation and scaling.
		// This is not important for OpenGL (it uses 4x4 matrix) but it is important
		// for the physic engine that needs a separate scaling
		//parentinversenode->SetLocalOrientation(parinvtrans.getBasis());

		// Extract the rotation and the scaling from the basis
		MT_Matrix3x3 ori(parinvtrans.getBasis());
		MT_Vector3 x(ori.getColumn(0));
		MT_Vector3 y(ori.getColumn(1));
		MT_Vector3 z(ori.getColumn(2));
		MT_Vector3 parscale(x.length(), y.length(), z.length());
		if (!MT_fuzzyZero(parscale[0]))
			x /= parscale[0];
		if (!MT_fuzzyZero(parscale[1]))
			y /= parscale[1];
		if (!MT_fuzzyZero(parscale[2]))
			z /= parscale[2];
		ori.setColumn(0, x);
		ori.setColumn(1, y);
		ori.setColumn(2, z);
		parentinversenode->SetLocalOrientation(ori);
		parentinversenode->SetLocalScale(parscale);

		parentinversenode->AddChild(gameobj->GetSGNode());
	}

	// needed for python scripting
	logicmgr->RegisterGameObjectName(gameobj->GetName(),gameobj);

	// needed for group duplication
	logicmgr->RegisterGameObj(blenderobject, gameobj);
	for (int i = 0; i < gameobj->GetMeshCount(); i++)
		logicmgr->RegisterGameMeshName(gameobj->GetMesh(i)->GetName(), blenderobject);

	converter.RegisterGameObject(gameobj, blenderobject);
	// this was put in rapidly, needs to be looked at more closely
	// only draw/use objects in active 'blender' layers

	logicbrick_conversionlist->Add(CM_AddRef(gameobj));

	if (isInActiveLayer)
	{
		objectlist->Add(CM_AddRef(gameobj));
		//tf.Add(gameobj->GetSGNode());

		gameobj->NodeUpdateGS(0);
		gameobj->AddMeshUser();
	}
	else
	{
		//we must store this object otherwise it will be deleted
		//at the end of this function if it is not a root object
		inactivelist->Add(CM_AddRef(gameobj));
	}
}


// convert blender objects into ketsji gameobjects
void BL_ConvertBlenderObjects(struct Main* maggie,
							  KX_Scene* kxscene,
							  KX_KetsjiEngine* ketsjiEngine,
							  e_PhysicsEngine	physics_engine,
							  RAS_Rasterizer* rendertools,
							  RAS_ICanvas* canvas,
							  BL_BlenderSceneConverter& converter,
							  bool alwaysUseExpandFraming,
							  bool libloading
							  )
{

#define BL_CONVERTBLENDEROBJECT_SINGLE                                 \
	bl_ConvertBlenderObject_Single(converter,                          \
	                               blenderobject,                      \
	                               vec_parent_child,                   \
	                               logicbrick_conversionlist,          \
	                               objectlist, inactivelist, sumolist, \
	                               kxscene, gameobj,                   \
	                               logicmgr, timemgr,                  \
	                               isInActiveLayer                     \
	                               )



	Scene *blenderscene = kxscene->GetBlenderScene();
	// for SETLOOPER
	Scene *sce_iter;
	Base *base;

	// Get the frame settings of the canvas.
	// Get the aspect ratio of the canvas as designed by the user.

	RAS_FrameSettings::RAS_FrameType frame_type;
	int aspect_width;
	int aspect_height;
	std::set<Group*> grouplist;	// list of groups to be converted
	std::set<Object*> allblobj;	// all objects converted
	std::set<Object*> groupobj;	// objects from groups (never in active layer)

	/* We have to ensure that group definitions are only converted once
	 * push all converted group members to this set.
	 * This will happen when a group instance is made from a linked group instance
	 * and both are on the active layer. */
	CListValue<KX_GameObject> *convertedlist = new CListValue<KX_GameObject>();

	if (alwaysUseExpandFraming) {
		frame_type = RAS_FrameSettings::e_frame_extend;
		aspect_width = canvas->GetWidth();
		aspect_height = canvas->GetHeight();
	} else {
		if (blenderscene->gm.framing.type == SCE_GAMEFRAMING_BARS) {
			frame_type = RAS_FrameSettings::e_frame_bars;
		} else if (blenderscene->gm.framing.type == SCE_GAMEFRAMING_EXTEND) {
			frame_type = RAS_FrameSettings::e_frame_extend;
		} else {
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
		aspect_height
	);
	kxscene->SetFramingType(frame_settings);

	kxscene->SetGravity(MT_Vector3(0,0, -blenderscene->gm.gravity));
	
	/* set activity culling parameters */
	kxscene->SetActivityCulling( (blenderscene->gm.mode & WO_ACTIVITY_CULLING) != 0);
	kxscene->SetActivityCullingRadius(blenderscene->gm.activityBoxRadius);
	kxscene->SetDbvtCulling((blenderscene->gm.mode & WO_DBVT_CULLING) != 0);
	
	// no occlusion culling by default
	kxscene->SetDbvtOcclusionRes(0);

	if (blenderscene->gm.lodflag & SCE_LOD_USE_HYST) {
		kxscene->SetLodHysteresis(true);
		kxscene->SetLodHysteresisValue(blenderscene->gm.scehysteresis);
	}

	// convert world
	KX_WorldInfo* worldinfo = new KX_WorldInfo(blenderscene, blenderscene->world);
	worldinfo->UpdateWorldSettings(rendertools);
	worldinfo->UpdateBackGround(rendertools);
	kxscene->SetWorldInfo(worldinfo);

	int activeLayerBitInfo = blenderscene->lay;
	
	// list of all object converted, active and inactive
	CListValue<KX_GameObject> *sumolist = new CListValue<KX_GameObject>();
	
	std::vector<parentChildLink> vec_parent_child;
	
	CListValue<KX_GameObject> *objectlist = kxscene->GetObjectList();
	CListValue<KX_GameObject> *inactivelist = kxscene->GetInactiveList();
	CListValue<KX_GameObject> *parentlist = kxscene->GetRootParentList();
	
	SCA_LogicManager* logicmgr = kxscene->GetLogicManager();
	SCA_TimeEventManager* timemgr = kxscene->GetTimeEventManager();
	
	CListValue<KX_GameObject> *logicbrick_conversionlist = new CListValue<KX_GameObject>();

	// Convert actions to actionmap
	bAction *curAct;
	for (curAct = (bAction*)maggie->action.first; curAct; curAct=(bAction*)curAct->id.next)
	{
		logicmgr->RegisterActionName(curAct->id.name + 2, curAct);
	}

	blenderSceneSetBackground(blenderscene);

	// Let's support scene set.
	// Beware of name conflict in linked data, it will not crash but will create confusion
	// in Python scripting and in certain actuators (replace mesh). Linked scene *should* have
	// no conflicting name for Object, Object data and Action.
	for (SETLOOPER(blenderscene, sce_iter, base))
	{
		Object* blenderobject = base->object;
		allblobj.insert(blenderobject);

		KX_GameObject* gameobj = gameobject_from_blenderobject(base->object, kxscene, rendertools, converter);

		bool isInActiveLayer = (blenderobject->lay & activeLayerBitInfo) !=0;
		if (gameobj)
		{
			/* macro calls object conversion funcs */
			BL_CONVERTBLENDEROBJECT_SINGLE;

			if (gameobj->IsDupliGroup()) {
				grouplist.insert(blenderobject->dup_group);
			}

			/* Note about memory leak issues:
			 * When a CValue derived class is created, m_refcount is initialized to 1
			 * so the class must be released after being used to make sure that it won't
			 * hang in memory. If the object needs to be stored for a long time,
			 * use AddRef() so that this Release() does not free the object.
			 * Make sure that for any AddRef() there is a Release()!!!!
			 * Do the same for any object derived from CValue, CExpression and NG_NetworkMessage
			 */
			gameobj->Release();
		}
	}

	if (!grouplist.empty())
	{
		// now convert the group referenced by dupli group object
		// keep track of all groups already converted
		std::set<Group*> allgrouplist = grouplist;
		std::set<Group*> tempglist;
		// recurse
		while (!grouplist.empty())
		{
			std::set<Group*>::iterator git;
			tempglist.clear();
			tempglist.swap(grouplist);
			for (git=tempglist.begin(); git!=tempglist.end(); git++)
			{
				Group* group = *git;
				GroupObject* go;
				for (go=(GroupObject*)group->gobject.first; go; go=(GroupObject*)go->next)
				{
					Object* blenderobject = go->ob;
					if (converter.FindGameObject(blenderobject) == nullptr)
					{
						allblobj.insert(blenderobject);
						groupobj.insert(blenderobject);
						KX_GameObject* gameobj = gameobject_from_blenderobject(blenderobject, kxscene, rendertools, converter);

						bool isInActiveLayer = false;
						if (gameobj) {
							/* Insert object to the constraint game object list
							 * so we can check later if there is a instance in the scene or
							 * an instance and its actual group definition. */
							convertedlist->Add((KX_GameObject*)gameobj->AddRef());

							/* macro calls object conversion funcs */
							BL_CONVERTBLENDEROBJECT_SINGLE;

							if (gameobj->IsDupliGroup())
							{
								if (allgrouplist.insert(blenderobject->dup_group).second)
								{
									grouplist.insert(blenderobject->dup_group);
								}
							}

							/* see comment above re: mem leaks */
							gameobj->Release();
						}
					}
				}
			}
		}
	}

	// non-camera objects not supported as camera currently
	if (blenderscene->camera && blenderscene->camera->type == OB_CAMERA) {
		KX_Camera *gamecamera= (KX_Camera*) converter.FindGameObject(blenderscene->camera);
		
		if (gamecamera)
			kxscene->SetActiveCamera(gamecamera);
	}

	//	Set up armatures
	std::set<Object*>::iterator oit;
	for (oit=allblobj.begin(); oit!=allblobj.end(); oit++)
	{
		Object* blenderobj = *oit;
		if (blenderobj->type==OB_MESH) {
			Mesh *me = (Mesh*)blenderobj->data;
	
			if (me->dvert) {
				BL_DeformableGameObject *obj = (BL_DeformableGameObject*)converter.FindGameObject(blenderobj);

				if (obj && BL_ModifierDeformer::HasArmatureDeformer(blenderobj) && blenderobj->parent && blenderobj->parent->type==OB_ARMATURE) {
					KX_GameObject *par = converter.FindGameObject(blenderobj->parent);
					if (par && obj->GetDeformer())
						((BL_SkinDeformer*)obj->GetDeformer())->SetArmature((BL_ArmatureObject*) par);
				}
			}
		}
	}
	
	// create hierarchy information
	std::vector<parentChildLink>::iterator pcit;
	
	for (pcit = vec_parent_child.begin();!(pcit==vec_parent_child.end());++pcit)
	{
	
		struct Object* blenderchild = pcit->m_blenderchild;
		struct Object* blenderparent = blenderchild->parent;
		KX_GameObject* parentobj = converter.FindGameObject(blenderparent);
		KX_GameObject* childobj = converter.FindGameObject(blenderchild);

		BLI_assert(childobj);

		if (!parentobj || objectlist->SearchValue(childobj) != objectlist->SearchValue(parentobj))
		{
			// special case: the parent and child object are not in the same layer. 
			// This weird situation is used in Apricot for test purposes.
			// Resolve it by not converting the child
			childobj->GetSGNode()->DisconnectFromParent();
			delete pcit->m_gamechildnode;
			// Now destroy the child object but also all its descendent that may already be linked
			// Remove the child reference in the local list!
			// Note: there may be descendents already if the children of the child were processed
			//       by this loop before the child. In that case, we must remove the children also
			CListValue<KX_GameObject> *childrenlist = childobj->GetChildrenRecursive();
			// The returned list by GetChildrenRecursive is not owned by anyone and must not own items, so no AddRef().
			childrenlist->Add(childobj);
			for (KX_GameObject *obj : childrenlist) {
				if (sumolist->RemoveValue(obj))
					obj->Release();
				if (logicbrick_conversionlist->RemoveValue(obj))
					obj->Release();
				if (convertedlist->RemoveValue(obj)) {
					obj->Release();
				}
			}
			childrenlist->Release();
			
			// now destroy recursively
			converter.UnregisterGameObject(childobj); // removing objects during conversion make sure this runs too
			kxscene->RemoveObject(childobj);
			
			continue;
		}

		switch (blenderchild->partype)
		{
			case PARVERT1:
			{
				// creat a new vertex parent relationship for this node.
				KX_VertexParentRelation * vertex_parent_relation = KX_VertexParentRelation::New();
				pcit->m_gamechildnode->SetParentRelation(vertex_parent_relation);
				break;
			}
			case PARSLOW:
			{
				// creat a new slow parent relationship for this node.
				KX_SlowParentRelation * slow_parent_relation = KX_SlowParentRelation::New(blenderchild->sf);
				pcit->m_gamechildnode->SetParentRelation(slow_parent_relation);
				break;
			}
			case PARBONE:
			{
				// parent this to a bone
				Bone *parent_bone = BKE_armature_find_bone_name(BKE_armature_from_object(blenderchild->parent),
				                                                blenderchild->parsubstr);

				if (parent_bone) {
					KX_BoneParentRelation *bone_parent_relation = KX_BoneParentRelation::New(parent_bone);
					pcit->m_gamechildnode->SetParentRelation(bone_parent_relation);
				}
			
				break;
			}
			case PARSKEL: // skinned - ignore
				break;
			case PAROBJECT:
			case PARVERT3:
			default:
				// unhandled
				break;
		}
	
		parentobj->	GetSGNode()->AddChild(pcit->m_gamechildnode);
	}
	vec_parent_child.clear();
	
	// find 'root' parents (object that has not parents in SceneGraph)
	for (KX_GameObject *gameobj : sumolist) {
		if (gameobj->GetSGNode()->GetSGParent() == 0) {
			parentlist->Add(CM_AddRef(gameobj));
			gameobj->NodeUpdateGS(0);
		}
	}

	// create graphic controller for culling
	if (kxscene->GetDbvtCulling())
	{
		bool occlusion = false;
		for (KX_GameObject *gameobj : sumolist) {
			// The object can be culled ?
			if (gameobj->GetMeshCount() || gameobj->GetGameObjectType() == SCA_IObject::OBJ_TEXT) {
				bool isactive = objectlist->SearchValue(gameobj);
				BL_CreateGraphicObjectNew(gameobj, kxscene, isactive, physics_engine);
				if (gameobj->GetOccluder())
					occlusion = true;
			}
		}
		if (occlusion)
			kxscene->SetDbvtOcclusionRes(blenderscene->gm.occlusionRes);
	}
	if (blenderscene->world)
		kxscene->GetPhysicsEnvironment()->SetNumTimeSubSteps(blenderscene->gm.physubstep);

	// now that the scenegraph is complete, let's instantiate the deformers.
	// We need that to create reusable derived mesh and physic shapes
	for (KX_GameObject *gameobj : sumolist) {
		if (gameobj->GetDeformer())
			gameobj->GetDeformer()->UpdateBuckets();
	}

	// Set up armature constraints and shapekey drivers
	for (KX_GameObject *gameobj : sumolist) {
		if (gameobj->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE) {
			BL_ArmatureObject *armobj = (BL_ArmatureObject*)gameobj;
			armobj->LoadConstraints(converter);

			CListValue<KX_GameObject> *children = armobj->GetChildren();
			for (KX_GameObject *child : children) {
				BL_ShapeDeformer *deform = dynamic_cast<BL_ShapeDeformer*>(child->GetDeformer());
				if (deform)
					deform->LoadShapeDrivers(armobj);
			}

			children->Release();
		}
	}

	bool processCompoundChildren = false;
	// create physics information
	for (KX_GameObject *gameobj : sumolist) {
		struct Object* blenderobject = gameobj->GetBlenderObject();
		int nummeshes = gameobj->GetMeshCount();
		RAS_MeshObject* meshobj = 0;
		if (nummeshes > 0)
		{
			meshobj = gameobj->GetMesh(0);
		}
		int layerMask = (groupobj.find(blenderobject) == groupobj.end()) ? activeLayerBitInfo : 0;
		BL_CreatePhysicsObjectNew(gameobj,blenderobject,meshobj,kxscene,layerMask,converter,processCompoundChildren);
	}

	processCompoundChildren = true;
	// create physics information
	for (KX_GameObject *gameobj : sumolist) {
		struct Object* blenderobject = gameobj->GetBlenderObject();
		int nummeshes = gameobj->GetMeshCount();
		RAS_MeshObject* meshobj = 0;
		if (nummeshes > 0)
		{
			meshobj = gameobj->GetMesh(0);
		}
		int layerMask = (groupobj.find(blenderobject) == groupobj.end()) ? activeLayerBitInfo : 0;
		BL_CreatePhysicsObjectNew(gameobj,blenderobject,meshobj,kxscene,layerMask,converter,processCompoundChildren);
	}

	// Look at every material texture and ask to create realtime cube map.
	for (KX_GameObject *gameobj : sumolist) {
		for (unsigned short i = 0, meshcount = gameobj->GetMeshCount(); i < meshcount; ++i) {
			RAS_MeshObject *mesh = gameobj->GetMesh(i);

			for (unsigned short j = 0, matcount = mesh->GetNumMaterials(); j < matcount; ++j) {
				RAS_MeshMaterial *meshmat = mesh->GetMeshMaterial(j);
				RAS_IPolyMaterial *polymat = meshmat->GetBucket()->GetPolyMaterial();

				for (unsigned short k = 0; k < RAS_Texture::MaxUnits; ++k) {
					RAS_Texture *tex = polymat->GetTexture(k);
					if (!tex || !tex->Ok()) {
						continue;
					}

					EnvMap *env = tex->GetTex()->env;
					if (!env || env->stype != ENV_REALT) {
						continue;
					}

					KX_GameObject *viewpoint = gameobj;
					if (env->object) {
						KX_GameObject *obj = converter.FindGameObject(env->object);
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

	// Create and set bounding volume.
	for (KX_GameObject *gameobj : sumolist) {
		Object *blenderobject = gameobj->GetBlenderObject();
		Mesh *predifinedBoundMesh = blenderobject->gamePredefinedBound;

		if (predifinedBoundMesh) {
			RAS_MeshObject *meshobj = converter.FindGameMesh(predifinedBoundMesh);
			// In case of mesh taken in a other scene.
			if (!meshobj) {
				continue;
			}

			gameobj->SetAutoUpdateBounds(false);

			// AABB Box : min/max.
			MT_Vector3 aabbMin;
			MT_Vector3 aabbMax;
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

	// create physics joints
	for (KX_GameObject *gameobj : sumolist) {
		PHY_IPhysicsEnvironment *physEnv = kxscene->GetPhysicsEnvironment();
		struct Object *blenderobject = gameobj->GetBlenderObject();
		ListBase *conlist = get_active_constraints2(blenderobject);
		bConstraint *curcon;

		if (!conlist)
			continue;

		for (curcon = (bConstraint *)conlist->first; curcon; curcon = (bConstraint *)curcon->next) {
			if (curcon->type != CONSTRAINT_TYPE_RIGIDBODYJOINT)
				continue;

			bRigidBodyJointConstraint *dat = (bRigidBodyJointConstraint *)curcon->data;
					
			/* Skip if no target or a child object is selected or constraints are deactivated */
			if (!dat->tar || dat->child || (curcon->flag & CONSTRAINT_OFF))
				continue;

			/* Store constraints of grouped and instanced objects for all layers */
			gameobj->AddConstraint(dat);

			/** if it's during libload we only add constraints in the object but
			 * doesn't create it. Constraint will be replicated later in scene->MergeScene
			 */
			if (libloading)
				continue;

			/* Skipped already converted constraints. 
			 * This will happen when a group instance is made from a linked group instance
			 * and both are on the active layer. */
			if (convertedlist->FindValue(gameobj->GetName())) {
				continue;
			}

			KX_GameObject *gotar = sumolist->FindValue(dat->tar->id.name + 2);

			if (gotar && (gotar->GetLayer()&activeLayerBitInfo) && gotar->GetPhysicsController() &&
				(gameobj->GetLayer()&activeLayerBitInfo) && gameobj->GetPhysicsController())
			{
				physEnv->SetupObjectConstraints(gameobj, gotar, dat);
			}
		}
	}

	//create object representations for obstacle simulation
	KX_ObstacleSimulation* obssimulation = kxscene->GetObstacleSimulation();
	if (obssimulation)
	{
		for (KX_GameObject *gameobj : objectlist) {
			struct Object* blenderobject = gameobj->GetBlenderObject();
			if (blenderobject->gameflag & OB_HASOBSTACLE)
			{
				obssimulation->AddObstacleForObj(gameobj);
			}
		}
	}

	//process navigation mesh objects
	for (KX_GameObject *gameobj : objectlist) {
		struct Object* blenderobject = gameobj->GetBlenderObject();
		if (blenderobject->type==OB_MESH && (blenderobject->gameflag & OB_NAVMESH))
		{
			KX_NavMeshObject* navmesh = static_cast<KX_NavMeshObject*>(gameobj);
			navmesh->SetVisible(0, true);
			navmesh->BuildNavMesh();
			if (obssimulation)
				obssimulation->AddObstaclesForNavMesh(navmesh);
		}
	}
	for (KX_GameObject *gameobj : inactivelist) {
		struct Object* blenderobject = gameobj->GetBlenderObject();
		if (blenderobject->type==OB_MESH && (blenderobject->gameflag & OB_NAVMESH))
		{
			KX_NavMeshObject* navmesh = static_cast<KX_NavMeshObject*>(gameobj);
			navmesh->SetVisible(0, true);
		}
	}

	// convert logic bricks, sensors, controllers and actuators
	for (KX_GameObject *gameobj : logicbrick_conversionlist) {
		struct Object* blenderobj = gameobj->GetBlenderObject();
		int layerMask = (groupobj.find(blenderobj) == groupobj.end()) ? activeLayerBitInfo : 0;
		bool isInActiveLayer = (blenderobj->lay & layerMask)!=0;
		BL_ConvertActuators(maggie->name, blenderobj,gameobj,logicmgr,kxscene,ketsjiEngine,layerMask,isInActiveLayer,converter);
	}
	for (KX_GameObject *gameobj : logicbrick_conversionlist) {
		struct Object* blenderobj = gameobj->GetBlenderObject();
		int layerMask = (groupobj.find(blenderobj) == groupobj.end()) ? activeLayerBitInfo : 0;
		bool isInActiveLayer = (blenderobj->lay & layerMask)!=0;
		BL_ConvertControllers(blenderobj,gameobj,logicmgr, layerMask,isInActiveLayer,converter, libloading);
	}
	for (KX_GameObject *gameobj : logicbrick_conversionlist) {
		struct Object* blenderobj = gameobj->GetBlenderObject();
		int layerMask = (groupobj.find(blenderobj) == groupobj.end()) ? activeLayerBitInfo : 0;
		bool isInActiveLayer = (blenderobj->lay & layerMask)!=0;
		BL_ConvertSensors(blenderobj,gameobj,logicmgr,kxscene,ketsjiEngine,layerMask,isInActiveLayer,canvas,converter);
		// set the init state to all objects
		gameobj->SetInitState((blenderobj->init_state)?blenderobj->init_state:blenderobj->state);
	}
	// apply the initial state to controllers, only on the active objects as this registers the sensors
	for (KX_GameObject *gameobj : objectlist) {
		gameobj->ResetState();
	}

	// Convert the python components of each object.
	for (KX_GameObject *gameobj : sumolist) {
		Object *blenderobj = gameobj->GetBlenderObject();
		BL_ConvertComponentsObject(gameobj, blenderobj);
	}

	// cleanup converted set of group objects
	convertedlist->Release();
	sumolist->Release();
	logicbrick_conversionlist->Release();
	
	// Calculate the scene btree -
	// too slow - commented out.
	//kxscene->SetNodeTree(tf.MakeTree());

	// instantiate dupli group, we will loop trough the object
	// that are in active layers. Note that duplicating group
	// has the effect of adding objects at the end of objectlist.
	// Only loop through the first part of the list.
	int objcount = objectlist->GetCount();
	for (unsigned int i = 0; i < objcount; ++i) {
		KX_GameObject* gameobj = objectlist->GetValue(i);
		if (gameobj->IsDupliGroup())
		{
			kxscene->DupliGroupRecurse(gameobj, 0);
		}
	}
}

