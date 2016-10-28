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

#include "PHY_Pro.h"
#include "PHY_IPhysicsEnvironment.h"

#include "RAS_MeshObject.h"
#include "RAS_IRasterizer.h"
#include "RAS_ILightObject.h"

#include "KX_ConvertActuators.h"
#include "KX_ConvertControllers.h"
#include "KX_ConvertSensors.h"
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

#include "RAS_ICanvas.h"
#include "RAS_Polygon.h"
#include "RAS_TexVert.h"
#include "RAS_BucketManager.h"
#include "RAS_IPolygonMaterial.h"
#include "KX_BlenderMaterial.h"
#include "KX_CubeMapManager.h"
#include "KX_CubeMap.h"
#include "BL_Texture.h"

#include "BKE_main.h"
#include "BKE_global.h"
#include "BKE_object.h"
#include "BL_ModifierDeformer.h"
#include "BL_ShapeDeformer.h"
#include "BL_SkinDeformer.h"
#include "BL_MeshDeformer.h"
#include "KX_SoftBodyDeformer.h"
#include "BLI_utildefines.h"
#include "BLI_listbase.h"

#include "KX_WorldInfo.h"

#include "KX_KetsjiEngine.h"
#include "KX_BlenderSceneConverter.h"

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

#include "KX_ConvertProperties.h"

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

static bool default_light_mode = 0;

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

/* Now the real converting starts... */
static unsigned int KX_Mcol2uint_new(MCol col)
{
	/* color has to be converted without endian sensitivity. So no shifting! */
	union
	{
		MCol col;
		unsigned int integer;
		unsigned char cp[4];
	} out_color, in_color;

	in_color.col = col;
	out_color.cp[0] = in_color.cp[3]; // red
	out_color.cp[1] = in_color.cp[2]; // green
	out_color.cp[2] = in_color.cp[1]; // blue
	out_color.cp[3] = in_color.cp[0]; // alpha
	
	return out_color.integer;
}

static void SetDefaultLightMode(Scene* scene)
{
	default_light_mode = false;
	Scene *sce_iter;
	Base *base;

	for (SETLOOPER(scene, sce_iter, base))
	{
		if (base->object->type == OB_LAMP)
		{
			default_light_mode = true;
			return;
		}
	}
}

static void GetRGB(
        MFace* mface,
		const RAS_MeshObject::LayerList& layers,
        unsigned int c[4][RAS_ITexVert::MAX_UNIT])
{
	for (RAS_MeshObject::LayerList::const_iterator it = layers.begin(), end = layers.end(); it != end; ++it) {
		const RAS_MeshObject::Layer& layer = *it;
		if (!layer.color) {
			continue;
		}

		c[0][layer.index] = KX_Mcol2uint_new(layer.color[0]);
		c[1][layer.index] = KX_Mcol2uint_new(layer.color[1]);
		c[2][layer.index] = KX_Mcol2uint_new(layer.color[2]);
		if (mface->v4) {
			c[3][layer.index] = KX_Mcol2uint_new(layer.color[3]);
		}
	}
}

static void GetUVs(const RAS_MeshObject::LayerList& layers, MFace *mface, MTFace *tface, MT_Vector2 uvs[4][RAS_Texture::MaxUnits])
{
	if (tface) {
		uvs[0][0].setValue(tface->uv[0]);
		uvs[1][0].setValue(tface->uv[1]);
		uvs[2][0].setValue(tface->uv[2]);

		if (mface->v4)
			uvs[3][0].setValue(tface->uv[3]);
	}
	else {
		uvs[0][0] = uvs[1][0] = uvs[2][0] = uvs[3][0] = MT_Vector2(0.0f, 0.0f);
	}

	for (RAS_MeshObject::LayerList::const_iterator it = layers.begin(), end = layers.end(); it != end; ++it) {
		const RAS_MeshObject::Layer& layer = *it;
		if (!layer.face) {
			continue;
		}

		uvs[0][layer.index].setValue(layer.face->uv[0]);
		uvs[1][layer.index].setValue(layer.face->uv[1]);
		uvs[2][layer.index].setValue(layer.face->uv[2]);

		if (mface->v4) {
			uvs[3][layer.index].setValue(layer.face->uv[3]);
		}
		else {
			uvs[3][layer.index].setValue(0.0f, 0.0f);
		}
	}
}

static KX_BlenderMaterial *ConvertMaterial(
	Material *mat,
	MTFace *tface,
	int lightlayer,
	KX_Scene *scene)
{
	KX_BlenderMaterial *kx_blmat = new KX_BlenderMaterial(scene, mat, (mat ? &mat->game : NULL), tface, lightlayer);

	return kx_blmat;
}

static RAS_MaterialBucket *material_from_mesh(
	Material *ma, MFace *mface, MTFace *tface, const RAS_MeshObject::LayerList& layers, int lightlayer,
	unsigned int rgb[4][RAS_ITexVert::MAX_UNIT], MT_Vector2 uvs[4][RAS_ITexVert::MAX_UNIT],
	KX_Scene* scene, KX_BlenderSceneConverter *converter)
{
	RAS_IPolyMaterial* polymat = converter->FindCachedPolyMaterial(scene, ma);

	if (mface) {
		GetRGB(mface, layers, rgb);

		GetUVs(layers, mface, tface, uvs);
	}

	if (!polymat) {
		polymat = ConvertMaterial(ma, tface, lightlayer, scene);
		converter->CachePolyMaterial(scene, ma, polymat);
	}
	
	// see if a bucket was reused or a new one was created
	// this way only one KX_BlenderMaterial object has to exist per bucket
	bool bucketCreated; 
	RAS_MaterialBucket* bucket = scene->FindBucket(polymat, bucketCreated);

	// this is needed to free up memory afterwards.
	// the converter will also prevent duplicates from being registered,
	// so just register everything.
	converter->RegisterPolyMaterial(scene, polymat);

	return bucket;
}

/* blenderobj can be NULL, make sure its checked for */
RAS_MeshObject* BL_ConvertMesh(Mesh* mesh, Object* blenderobj, KX_Scene* scene, KX_BlenderSceneConverter *converter, bool libloading)
{
	RAS_MeshObject *meshobj;
	int lightlayer = blenderobj ? blenderobj->lay:(1<<20)-1; // all layers if no object.

	// Without checking names, we get some reuse we don't want that can cause
	// problems with material LoDs.
	if (blenderobj && ((meshobj = converter->FindGameMesh(mesh/*, ob->lay*/)) != NULL)) {
		const char *bge_name = meshobj->GetName().ReadPtr();
		const char *blender_name = ((ID *)blenderobj->data)->name + 2;
		if (STREQ(bge_name, blender_name)) {
			return meshobj;
		}
	}

	// Get DerivedMesh data
	DerivedMesh *dm = CDDM_from_mesh(mesh);
	DM_ensure_tessface(dm);

	MVert *mvert = dm->getVertArray(dm);
	int totvert = dm->getNumVerts(dm);

	MFace *mface = dm->getTessFaceArray(dm);
	MTFace *tface = static_cast<MTFace*>(dm->getTessFaceDataArray(dm, CD_MTFACE));
	MPoly *mpolyarray = (MPoly *)dm->getPolyArray(dm);
	MLoop *mlooparray = (MLoop *)dm->getLoopArray(dm);
	MEdge *medgearray = (MEdge *)dm->getEdgeArray(dm);
	int *mfaceTompoly = (int *)dm->getTessFaceDataArray(dm, CD_ORIGINDEX);
	float (*tangent)[4] = NULL;
	int totface = dm->getNumTessFaces(dm);

	/* needs to be rewritten for loopdata */
	if (tface) {
		if (CustomData_get_layer_index(&dm->faceData, CD_TANGENT) == -1) {
			bool generate_data = false;
			if (CustomData_get_layer_index(&dm->loopData, CD_TANGENT) == -1) {
				DM_calc_loop_tangents(dm, true, NULL, 0);
				generate_data = true;
			}
			DM_generate_tangent_tessface_data(dm, generate_data);
		}
		tangent = (float(*)[4])dm->getTessFaceDataArray(dm, CD_TANGENT);
	}

	// Extract avaiable layers
	RAS_MeshObject::LayerList layers;

	unsigned short uvLayers = 0;
	unsigned short colorLayers = 0;
	for (int i=0; i<dm->faceData.totlayer; i++)
	{
		if (dm->faceData.layers[i].type == CD_MTFACE || dm->faceData.layers[i].type == CD_MCOL)
		{
			if (uvLayers > MAX_MTFACE) {
				CM_Warning(__func__ << ": corrupted mesh " << mesh->id.name << " - too many CD_MTFACE layers");
				break;
			}

			if (colorLayers > MAX_MCOL) {
				CM_Warning(__func__ << ": corrupted mesh " << mesh->id.name << " - too many CD_MCOL layers");
				break;
			}

			RAS_MeshObject::Layer layer = {NULL, NULL, 0, dm->faceData.layers[i].name};

			if (dm->faceData.layers[i].type == CD_MCOL) {
				layer.color = (MCol *)(dm->faceData.layers[i].data);
				layer.index = colorLayers;
				++colorLayers;
			}
			else {
				layer.face = (MTFace*)(dm->faceData.layers[i].data);
				layer.index = uvLayers;
				++uvLayers;
			}

			layers.push_back(layer);
		}
	}

	meshobj = new RAS_MeshObject(mesh, layers);

	meshobj->m_sharedvertex_map.resize(totvert);

	RAS_TexVertFormat vertformat;
	vertformat.uvSize = max_ii(1, uvLayers);
	vertformat.colorSize = max_ii(1, colorLayers);

	Material* ma = 0;
	MT_Vector2 uvs[4][RAS_ITexVert::MAX_UNIT];
	unsigned int rgb[4][RAS_ITexVert::MAX_UNIT];

	MT_Vector3 pt[4];
	MT_Vector3 no[4];
	MT_Vector4 tan[4];

	/* ugh, if there is a less annoying way to do this please use that.
	 * since these are converted from floats to floats, theres no real
	 * advantage to use MT_ types - campbell */
	for (unsigned int i = 0; i < 4; i++) {
		const float zero_vec[4] = {0.0f};
		pt[i].setValue(zero_vec);
		no[i].setValue(zero_vec);
		tan[i].setValue(zero_vec);
	}

	/* we need to manually initialize the uvs (MoTo doesn't do that) [#34550] */
	for (unsigned int i = 0; i < RAS_ITexVert::MAX_UNIT; i++) {
		uvs[0][i] = uvs[1][i] = uvs[2][i] = uvs[3][i] = MT_Vector2(0.f, 0.f);
		rgb[0][i] = rgb[1][i] = rgb[2][i] = rgb[3][i] = 0xffffffffL;
	}

	if (totface == 0) {
		ma = mesh->mat ? mesh->mat[0] : NULL;
		// Check for blender material
		if (!ma) {
			ma = &defmaterial;
		}

		RAS_MaterialBucket *bucket = material_from_mesh(ma, mface, tface, layers, lightlayer, rgb, uvs, scene, converter);
		meshobj->AddMaterial(bucket, 0, vertformat);
	}

	for (int f=0;f<totface;f++,mface++)
	{
		/* get coordinates, normals and tangents */
		pt[0].setValue(mvert[mface->v1].co);
		pt[1].setValue(mvert[mface->v2].co);
		pt[2].setValue(mvert[mface->v3].co);
		if (mface->v4) pt[3].setValue(mvert[mface->v4].co);

		if (mface->flag & ME_SMOOTH) {
			float n0[3], n1[3], n2[3], n3[3];

			normal_short_to_float_v3(n0, mvert[mface->v1].no);
			normal_short_to_float_v3(n1, mvert[mface->v2].no);
			normal_short_to_float_v3(n2, mvert[mface->v3].no);
			no[0] = MT_Vector3(n0);
			no[1] = MT_Vector3(n1);
			no[2] = MT_Vector3(n2);

			if (mface->v4) {
				normal_short_to_float_v3(n3, mvert[mface->v4].no);
				no[3] = MT_Vector3(n3);
			}
		}
		else {
			float fno[3];

			if (mface->v4)
				normal_quad_v3(fno,mvert[mface->v1].co, mvert[mface->v2].co, mvert[mface->v3].co, mvert[mface->v4].co);
			else
				normal_tri_v3(fno,mvert[mface->v1].co, mvert[mface->v2].co, mvert[mface->v3].co);

			no[0] = no[1] = no[2] = no[3] = MT_Vector3(fno);
		}

		if (tangent) {
			tan[0] = MT_Vector4(tangent[f*4 + 0]);
			tan[1] = MT_Vector4(tangent[f*4 + 1]);
			tan[2] = MT_Vector4(tangent[f*4 + 2]);

			if (mface->v4)
				tan[3] = MT_Vector4(tangent[f*4 + 3]);
		}
		if (blenderobj)
			ma = give_current_material(blenderobj, mface->mat_nr+1);
		else
			ma = mesh->mat ? mesh->mat[mface->mat_nr]:NULL;

		// Check for blender material
		if (ma == NULL) {
			ma= &defmaterial;
		}

		{

			RAS_MaterialBucket* bucket = material_from_mesh(ma, mface, tface, layers, lightlayer, rgb, uvs, scene, converter);
			RAS_MeshMaterial *meshmat = meshobj->AddMaterial(bucket, mface->mat_nr, vertformat);

			// set render flags
			bool visible = ((ma->game.flag & GEMAT_INVISIBLE)==0);
			bool twoside = ((ma->game.flag  & GEMAT_BACKCULL)==0);
			bool collider = ((ma->game.flag & GEMAT_NOPHYSICS)==0);

			/* mark face as flat, so vertices are split */
			bool flat = (mface->flag & ME_SMOOTH) == 0;
				
			int nverts = (mface->v4)? 4: 3;

			unsigned int indices[4]; // all indices of the poly, can be a tri or quad.

			indices[0] = meshobj->AddVertex(meshmat, pt[0], uvs[0], tan[0], rgb[0], no[0], flat, mface->v1);
			indices[1] = meshobj->AddVertex(meshmat, pt[1], uvs[1], tan[1], rgb[1], no[1], flat, mface->v2);
			indices[2] = meshobj->AddVertex(meshmat, pt[2], uvs[2], tan[2], rgb[2], no[2], flat, mface->v3);

			if (nverts == 4) {
				indices[3] = meshobj->AddVertex(meshmat, pt[3], uvs[3], tan[3], rgb[3], no[3], flat, mface->v4);
			}

			if (bucket->IsWire() && visible) {
				// The fourth value can be uninitialized.
				unsigned int mfaceindices[4] = {mface->v1, mface->v2, mface->v3, mface->v4};
				MPoly *mpoly = mpolyarray + mfaceTompoly[f];
				unsigned int lpstart = mpoly->loopstart;
				unsigned int totlp = mpoly->totloop;
				// Iterate on all edges (=loops) of the MPoly which contains the current MFace.
				for (unsigned int i = lpstart; i < lpstart + totlp; ++i) {
					MLoop *mloop = mlooparray + i;
					// Get the edge.
					MEdge *medge = medgearray + mloop->e;
					// Iterate on all MFace vertices index.
					for (unsigned short j = (nverts - 1), k = 0; k < nverts; j = k++) {
						// If 2 vertices are the same as an edge, we add a line in the mesh.
						if (ELEM(medge->v1, mfaceindices[j], mfaceindices[k]) &&
							ELEM(medge->v2, mfaceindices[j], mfaceindices[k])) {
							meshobj->AddLine(meshmat, indices[j], indices[k]);
							break;
						}
					}
				}
			}
			meshobj->AddPolygon(meshmat, nverts, indices, visible, collider, twoside);
		}

		if (tface) 
			tface++;

		for (RAS_MeshObject::LayerList::iterator it = layers.begin(), end = layers.end(); it != end; ++it) {
			RAS_MeshObject::Layer &layer = *it;

			if (layer.face) {
				++layer.face;
			}
			if (layer.color) {
				layer.color += 4;
			}
		}
	}
	// keep meshobj->m_sharedvertex_map for reinstance phys mesh.
	// 2.49a and before it did: meshobj->m_sharedvertex_map.clear();
	// but this didnt save much ram. - Campbell
	meshobj->EndConversion();

	// pre calculate texture generation
	// However, we want to delay this if we're libloading so we can make sure we have the right scene.
	if (!libloading) {
		for (std::vector<RAS_MeshMaterial *>::iterator mit = meshobj->GetFirstMaterial();
			mit != meshobj->GetLastMaterial(); ++ mit) {
			(*mit)->m_bucket->GetPolyMaterial()->OnConstruction();
		}
	}

	// Find attributes layer (currently only UVs) by materials for this mesh.
	meshobj->GenerateAttribLayers();

	dm->release(dm);

	converter->RegisterGameMesh(scene, meshobj, mesh);
	return meshobj;
}

	
	
static PHY_MaterialProps *CreateMaterialFromBlenderObject(struct Object* blenderobject)
{
	PHY_MaterialProps *materialProps = new PHY_MaterialProps;
	
	BLI_assert(materialProps && "Create physics material properties failed");
		
	Material* blendermat = give_current_material(blenderobject, 1);
		
	if (blendermat)
	{
		BLI_assert(0.0f <= blendermat->reflect && blendermat->reflect <= 1.0f);
	
		materialProps->m_restitution = blendermat->reflect;
		materialProps->m_friction = blendermat->friction;
		materialProps->m_rollingFriction = blendermat->rolling_friction;
		materialProps->m_fh_spring = blendermat->fh;
		materialProps->m_fh_damping = blendermat->xyfrict;
		materialProps->m_fh_distance = blendermat->fhdist;
		materialProps->m_fh_normal = (blendermat->dynamode & MA_FH_NOR) != 0;
	}
	else {
		//give some defaults
		materialProps->m_restitution = 0.f;
		materialProps->m_friction = 0.5;
		materialProps->m_rollingFriction = 0.0f;
		materialProps->m_fh_spring = 0.f;
		materialProps->m_fh_damping = 0.f;
		materialProps->m_fh_distance = 0.f;
		materialProps->m_fh_normal = false;

	}
	
	return materialProps;
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
	
	shapeProps->m_friction_scaling[0] = blenderobject->anisotropicFriction[0]; 
	shapeProps->m_friction_scaling[1] = blenderobject->anisotropicFriction[1];
	shapeProps->m_friction_scaling[2] = blenderobject->anisotropicFriction[2];
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

	return shapeProps;
}

//////////////////////////////////////////////////////


static void BL_CreateGraphicObjectNew(KX_GameObject* gameobj,
                                      KX_Scene* kxscene,
                                      bool isActive,
                                      e_PhysicsEngine physics_engine)
{
	if (gameobj->GetMeshCount() > 0)
	{
		switch (physics_engine)
		{
#ifdef WITH_BULLET
		case UseBullet:
			{
				CcdPhysicsEnvironment* env = (CcdPhysicsEnvironment*)kxscene->GetPhysicsEnvironment();
				assert(env);
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
}

static void BL_CreatePhysicsObjectNew(KX_GameObject* gameobj,
                                      struct Object* blenderobject,
                                      RAS_MeshObject* meshobj,
                                      KX_Scene* kxscene,
                                      int activeLayerBitInfo,
                                      KX_BlenderSceneConverter *converter,
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

	// get Root Parent of blenderobject
	struct Object* parent= blenderobject->parent;
	while (parent && parent->parent) {
		parent= parent->parent;
	}

	bool isCompoundChild = false;
	bool hasCompoundChildren = !parent && (blenderobject->gameflag & OB_CHILD) && !(blenderobject->gameflag & OB_SOFT_BODY);

	/* When the parent is not OB_DYNAMIC and has no OB_COLLISION then it gets no bullet controller
	 * and cant be apart of the parents compound shape, same goes for OB_SOFT_BODY */
	if (parent && (parent->gameflag & (OB_DYNAMIC | OB_COLLISION))) {
		if ((parent->gameflag & OB_CHILD)!=0 && (blenderobject->gameflag & OB_CHILD) && !(parent->gameflag & OB_SOFT_BODY)) {
			isCompoundChild = true;
		}
	}
	if (processCompoundChildren != isCompoundChild)
		return;


	PHY_ShapeProps* shapeprops =
			CreateShapePropsFromBlenderObject(blenderobject);

	
	PHY_MaterialProps* smmaterial = 
		CreateMaterialFromBlenderObject(blenderobject);

	DerivedMesh* dm = NULL;
	if (gameobj->GetDeformer())
		dm = gameobj->GetDeformer()->GetPhysicsMesh();

	class PHY_IMotionState* motionstate = new KX_MotionState(gameobj->GetSGNode());

	kxscene->GetPhysicsEnvironment()->ConvertObject(gameobj, meshobj, dm, kxscene, shapeprops, smmaterial, motionstate, activeLayerBitInfo, isCompoundChild, hasCompoundChildren);

	bool isActor = (blenderobject->gameflag & OB_ACTOR)!=0;
	bool isSensor = (blenderobject->gameflag & OB_SENSOR) != 0;
	gameobj->getClientInfo()->m_type =
		(isSensor) ? ((isActor) ? KX_ClientObjectInfo::OBACTORSENSOR : KX_ClientObjectInfo::OBSENSOR) :
		(isActor) ? KX_ClientObjectInfo::ACTOR : KX_ClientObjectInfo::STATIC;

	delete shapeprops;
	delete smmaterial;
	if (dm) {
		dm->needsFree = 1;
		dm->release(dm);
	}
}

static KX_LodManager *lodmanager_from_blenderobject(Object *ob, KX_Scene *scene, KX_BlenderSceneConverter *converter, bool libloading)
{
	if (BLI_listbase_count_ex(&ob->lodlevels, 2) <= 1) {
		return NULL;
	}

	KX_LodManager *lodManager = new KX_LodManager(ob, scene, converter, libloading);
	// The lod manager is useless ?
	if (lodManager->GetLevelCount() <= 1) {
		lodManager->Release();
		return NULL;
	}

	return lodManager;
}

static KX_LightObject *gamelight_from_blamp(Object *ob, Lamp *la, unsigned int layerflag, KX_Scene *kxscene, RAS_IRasterizer *rasterizer, KX_BlenderSceneConverter *converter)
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

	return gamelight;
}

static KX_Camera *gamecamera_from_bcamera(Object *ob, KX_Scene *kxscene, KX_BlenderSceneConverter *converter)
{
	Camera* ca = static_cast<Camera*>(ob->data);
	RAS_CameraData camdata(ca->lens, ca->ortho_scale, ca->sensor_x, ca->sensor_y, ca->sensor_fit, ca->shiftx, ca->shifty, ca->clipsta, ca->clipend, ca->type == CAM_PERSP, ca->YF_dofdist);
	KX_Camera *gamecamera;
	
	gamecamera= new KX_Camera(kxscene, KX_Scene::m_callbacks, camdata);
	gamecamera->SetName(ca->id.name + 2);
	
	return gamecamera;
}

static KX_GameObject *gameobject_from_blenderobject(
								Object *ob, 
								KX_Scene *kxscene, 
								RAS_IRasterizer *rendertools,
								KX_BlenderSceneConverter *converter,
								bool libloading) 
{
	KX_GameObject *gameobj = NULL;
	Scene *blenderscene = kxscene->GetBlenderScene();
	
	switch (ob->type) {
	case OB_LAMP:
	{
		KX_LightObject* gamelight = gamelight_from_blamp(ob, static_cast<Lamp*>(ob->data), ob->lay, kxscene, rendertools, converter);
		gameobj = gamelight;
		
		if (blenderscene->lay & ob->lay)
		{
			gamelight->AddRef();
			kxscene->GetLightList()->Add(gamelight);
		}

		break;
	}
	
	case OB_CAMERA:
	{
		KX_Camera* gamecamera = gamecamera_from_bcamera(ob, kxscene, converter);
		gameobj = gamecamera;
		
		//don't add a reference: the camera list in kxscene->m_cameras is not released at the end
		//gamecamera->AddRef();
		kxscene->GetCameraList()->Add(gamecamera->AddRef());
		
		break;
	}
	
	case OB_MESH:
	{
		Mesh* mesh = static_cast<Mesh*>(ob->data);
		RAS_MeshObject* meshobj = BL_ConvertMesh(mesh,ob,kxscene,converter, libloading);
		
		// needed for python scripting
		kxscene->GetLogicManager()->RegisterMeshName(meshobj->GetName(),meshobj);

		if (ob->gameflag & OB_NAVMESH)
		{
			gameobj = new KX_NavMeshObject(kxscene,KX_Scene::m_callbacks);
			gameobj->AddMesh(meshobj);
			break;
		}

		gameobj = new BL_DeformableGameObject(ob,kxscene,KX_Scene::m_callbacks);
	
		// set transformation
		gameobj->AddMesh(meshobj);

		// gather levels of detail
		KX_LodManager *lodManager = lodmanager_from_blenderobject(ob, kxscene, converter, libloading);
		gameobj->SetLodManager(lodManager);

		// for all objects: check whether they want to
		// respond to updates
		bool ignoreActivityCulling =  
			((ob->gameflag2 & OB_NEVER_DO_ACTIVITY_CULLING)!=0);
		gameobj->SetIgnoreActivityCulling(ignoreActivityCulling);
		gameobj->SetOccluder((ob->gameflag & OB_OCCLUDER) != 0, false);

		// two options exists for deform: shape keys and armature
		// only support relative shape key
		bool bHasShapeKey = mesh->key != NULL && mesh->key->type==KEY_RELATIVE;
		bool bHasDvert = mesh->dvert != NULL && ob->defbase.first;
		bool bHasArmature = (BL_ModifierDeformer::HasArmatureDeformer(ob) && ob->parent && ob->parent->type == OB_ARMATURE && bHasDvert);
		bool bHasModifier = BL_ModifierDeformer::HasCompatibleDeformer(ob);
#ifdef WITH_BULLET
		bool bHasSoftBody = (!ob->parent && (ob->gameflag & OB_SOFT_BODY));
#endif

		RAS_Deformer *deformer = NULL;
		BL_DeformableGameObject *deformableGameObj = (BL_DeformableGameObject *)gameobj;

		if (bHasModifier) {
			deformer = new BL_ModifierDeformer(deformableGameObj, kxscene->GetBlenderScene(), ob, meshobj);
		}
		else if (bHasShapeKey) {
			// not that we can have shape keys without dvert! 
			deformer = new BL_ShapeDeformer(deformableGameObj, ob, meshobj);
		}
		else if (bHasArmature) {
			deformer = new BL_SkinDeformer(deformableGameObj, ob, meshobj);
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
		/* Get the current pose from the armature object and apply it as the rest pose */
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
		gameobj = new KX_FontObject(kxscene,KX_Scene::m_callbacks, rendertools, ob, do_color_management);

		kxscene->GetFontList()->Add(gameobj->AddRef());
		break;
	}

#ifdef THREADED_DAG_WORKAROUND
	case OB_CURVE:
	{
		if (ob->curve_cache == NULL) {
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
	
	return NULL;
}

static ListBase *get_active_constraints2(Object *ob)
{
	if (!ob)
		return NULL;

  // XXX - shouldnt we care about the pose data and not the mode???
	if (ob->mode & OB_MODE_POSE) { 
		bPoseChannel *pchan;

		pchan = get_active_posechannel2(ob);
		if (pchan)
			return &pchan->constraints;
	}
	else 
		return &ob->constraints;

	return NULL;
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

static KX_GameObject* getGameOb(STR_String busc,CListValue* sumolist)
{

	for (int j=0;j<sumolist->GetCount();j++)
	{
		KX_GameObject* gameobje = (KX_GameObject*) sumolist->GetValue(j);
		if (gameobje->GetName()==busc)
			return gameobje;
	}
	
	return 0;

}

/* helper for BL_ConvertBlenderObjects, avoids code duplication
 * note: all var names match args are passed from the caller */
static void bl_ConvertBlenderObject_Single(
        KX_BlenderSceneConverter *converter,
       Object *blenderobject,
        vector<parentChildLink> &vec_parent_child,
        CListValue* logicbrick_conversionlist,
        CListValue* objectlist, CListValue* inactivelist, CListValue*	sumolist,
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

	sumolist->Add(gameobj->AddRef());

	BL_ConvertProperties(blenderobject,gameobj,timemgr,kxscene,isInActiveLayer);

	gameobj->SetName(blenderobject->id.name + 2);

	// update children/parent hierarchy
	if (blenderobject->parent != 0)
	{
		// blender has an additional 'parentinverse' offset in each object
		SG_Callbacks callback(NULL,NULL,NULL,KX_Scene::KX_ScenegraphUpdateFunc,KX_Scene::KX_ScenegraphRescheduleFunc);
		SG_Node* parentinversenode = new SG_Node(NULL,kxscene,callback);

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

	converter->RegisterGameObject(gameobj, blenderobject);
	// this was put in rapidly, needs to be looked at more closely
	// only draw/use objects in active 'blender' layers

	logicbrick_conversionlist->Add(gameobj->AddRef());

	if (isInActiveLayer)
	{
		objectlist->Add(gameobj->AddRef());
		//tf.Add(gameobj->GetSGNode());

		gameobj->NodeUpdateGS(0);
		gameobj->AddMeshUser();
	}
	else
	{
		//we must store this object otherwise it will be deleted
		//at the end of this function if it is not a root object
		inactivelist->Add(gameobj->AddRef());
	}
}


// convert blender objects into ketsji gameobjects
void BL_ConvertBlenderObjects(struct Main* maggie,
							  KX_Scene* kxscene,
							  KX_KetsjiEngine* ketsjiEngine,
							  e_PhysicsEngine	physics_engine,
							  RAS_IRasterizer* rendertools,
							  RAS_ICanvas* canvas,
							  KX_BlenderSceneConverter* converter,
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
	set<Group*> grouplist;	// list of groups to be converted
	set<Object*> allblobj;	// all objects converted
	set<Object*> groupobj;	// objects from groups (never in active layer)

	/* We have to ensure that group definitions are only converted once
	 * push all converted group members to this set.
	 * This will happen when a group instance is made from a linked group instance
	 * and both are on the active layer. */
	CListValue *convertedlist = new CListValue();

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
	kxscene->SetWorldInfo(worldinfo);

	int activeLayerBitInfo = blenderscene->lay;
	
	// list of all object converted, active and inactive
	CListValue*	sumolist = new CListValue();
	
	vector<parentChildLink> vec_parent_child;
	
	CListValue* objectlist = kxscene->GetObjectList();
	CListValue* inactivelist = kxscene->GetInactiveList();
	CListValue* parentlist = kxscene->GetRootParentList();
	
	SCA_LogicManager* logicmgr = kxscene->GetLogicManager();
	SCA_TimeEventManager* timemgr = kxscene->GetTimeEventManager();
	
	CListValue* logicbrick_conversionlist = new CListValue();

	// Convert actions to actionmap
	bAction *curAct;
	for (curAct = (bAction*)maggie->action.first; curAct; curAct=(bAction*)curAct->id.next)
	{
		logicmgr->RegisterActionName(curAct->id.name + 2, curAct);
	}

	SetDefaultLightMode(blenderscene);

	blenderSceneSetBackground(blenderscene);

	// Let's support scene set.
	// Beware of name conflict in linked data, it will not crash but will create confusion
	// in Python scripting and in certain actuators (replace mesh). Linked scene *should* have
	// no conflicting name for Object, Object data and Action.
	for (SETLOOPER(blenderscene, sce_iter, base))
	{
		Object* blenderobject = base->object;
		allblobj.insert(blenderobject);

		KX_GameObject* gameobj = gameobject_from_blenderobject(
										base->object, 
										kxscene, 
										rendertools, 
										converter,
										libloading);

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
		set<Group*> allgrouplist = grouplist;
		set<Group*> tempglist;
		// recurse
		while (!grouplist.empty())
		{
			set<Group*>::iterator git;
			tempglist.clear();
			tempglist.swap(grouplist);
			for (git=tempglist.begin(); git!=tempglist.end(); git++)
			{
				Group* group = *git;
				GroupObject* go;
				for (go=(GroupObject*)group->gobject.first; go; go=(GroupObject*)go->next)
				{
					Object* blenderobject = go->ob;
					if (converter->FindGameObject(blenderobject) == NULL)
					{
						allblobj.insert(blenderobject);
						groupobj.insert(blenderobject);
						KX_GameObject* gameobj = gameobject_from_blenderobject(
														blenderobject, 
														kxscene, 
														rendertools, 
														converter,
														libloading);

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
		KX_Camera *gamecamera= (KX_Camera*) converter->FindGameObject(blenderscene->camera);
		
		if (gamecamera)
			kxscene->SetActiveCamera(gamecamera);
	}

	//	Set up armatures
	set<Object*>::iterator oit;
	for (oit=allblobj.begin(); oit!=allblobj.end(); oit++)
	{
		Object* blenderobj = *oit;
		if (blenderobj->type==OB_MESH) {
			Mesh *me = (Mesh*)blenderobj->data;
	
			if (me->dvert) {
				BL_DeformableGameObject *obj = (BL_DeformableGameObject*)converter->FindGameObject(blenderobj);

				if (obj && BL_ModifierDeformer::HasArmatureDeformer(blenderobj) && blenderobj->parent && blenderobj->parent->type==OB_ARMATURE) {
					KX_GameObject *par = converter->FindGameObject(blenderobj->parent);
					if (par && obj->GetDeformer())
						((BL_SkinDeformer*)obj->GetDeformer())->SetArmature((BL_ArmatureObject*) par);
				}
			}
		}
	}
	
	// create hierarchy information
	int i;
	vector<parentChildLink>::iterator pcit;
	
	for (pcit = vec_parent_child.begin();!(pcit==vec_parent_child.end());++pcit)
	{
	
		struct Object* blenderchild = pcit->m_blenderchild;
		struct Object* blenderparent = blenderchild->parent;
		KX_GameObject* parentobj = converter->FindGameObject(blenderparent);
		KX_GameObject* childobj = converter->FindGameObject(blenderchild);

		assert(childobj);

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
			CListValue* childrenlist = childobj->GetChildrenRecursive();
			// The returned list by GetChildrenRecursive is not owned by anyone and must not own items, so no AddRef().
			childrenlist->Add(childobj);
			for ( i=0;i<childrenlist->GetCount();i++)
			{
				KX_GameObject* obj = static_cast<KX_GameObject*>(childrenlist->GetValue(i));
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
			converter->UnregisterGameObject(childobj); // removing objects during conversion make sure this runs too
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
	for (i=0;i<sumolist->GetCount();++i)
	{
		KX_GameObject* gameobj = (KX_GameObject*) sumolist->GetValue(i);
		if (gameobj->GetSGNode()->GetSGParent() == 0)
		{
			parentlist->Add(gameobj->AddRef());
			gameobj->NodeUpdateGS(0);
		}
	}

	// create graphic controller for culling
	if (kxscene->GetDbvtCulling())
	{
		bool occlusion = false;
		for (i=0; i<sumolist->GetCount();i++)
		{
			KX_GameObject* gameobj = (KX_GameObject*) sumolist->GetValue(i);
			if (gameobj->GetMeshCount() > 0) 
			{
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
	for (i=0;i<sumolist->GetCount();++i)
	{
		KX_GameObject* gameobj = (KX_GameObject*) sumolist->GetValue(i);
		if (gameobj->GetDeformer())
			gameobj->GetDeformer()->UpdateBuckets();
	}

	// Set up armature constraints and shapekey drivers
	for (i=0;i<sumolist->GetCount();++i)
	{
		KX_GameObject* gameobj = (KX_GameObject*) sumolist->GetValue(i);
		if (gameobj->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE)
		{
			BL_ArmatureObject *armobj = (BL_ArmatureObject*)gameobj;
			armobj->LoadConstraints(converter);

			CListValue *children = armobj->GetChildren();
			for (int j=0; j<children->GetCount();++j)
			{
				BL_ShapeDeformer *deform = dynamic_cast<BL_ShapeDeformer*>(((KX_GameObject*)children->GetValue(j))->GetDeformer());
				if (deform)
					deform->LoadShapeDrivers(armobj);
			}

			children->Release();
		}
	}

	bool processCompoundChildren = false;
	// create physics information
	for (i=0;i<sumolist->GetCount();i++)
	{
		KX_GameObject* gameobj = (KX_GameObject*) sumolist->GetValue(i);
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
	for (i=0;i<sumolist->GetCount();i++)
	{
		KX_GameObject* gameobj = (KX_GameObject*) sumolist->GetValue(i);
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
	for (CListValue::iterator it = sumolist->GetBegin(), end = sumolist->GetEnd(); it != end; ++it) {
		KX_GameObject *gameobj = (KX_GameObject*)*it;

		for (unsigned short i = 0, meshcount = gameobj->GetMeshCount(); i < meshcount; ++i) {
			RAS_MeshObject *mesh = gameobj->GetMesh(i);

			for (unsigned short j = 0, matcount = mesh->NumMaterials(); j < matcount; ++j) {
				RAS_MeshMaterial *meshmat = mesh->GetMeshMaterial(j);
				RAS_IPolyMaterial *polymat = meshmat->m_bucket->GetPolyMaterial();

				for (unsigned short k = 0; k < RAS_Texture::MaxUnits; ++k) {
					RAS_Texture *tex = polymat->GetTexture(k);

					if (tex && tex->Ok() && tex->IsCubeMap() && tex->GetTex()->env->stype == ENV_REALT) {
						EnvMap *env = tex->GetTex()->env;
						KX_GameObject *viewpoint = gameobj;

						if (env->object) {
							KX_GameObject *obj = converter->FindGameObject(env->object);
							if (obj) {
								viewpoint = obj;
							}
						}

						kxscene->GetCubeMapManager()->AddCubeMap(tex, viewpoint);
					}
				}
			}
		}
	}

	// Create and set bounding volume.
	for (i = 0; i < sumolist->GetCount(); ++i) {
		KX_GameObject *gameobj = (KX_GameObject *)sumolist->GetValue(i);
		Object *blenderobject = gameobj->GetBlenderObject();
		Mesh *predifinedBoundMesh = blenderobject->gamePredefinedBound;

		if (predifinedBoundMesh) {
			RAS_MeshObject *meshobj = converter->FindGameMesh(predifinedBoundMesh);
			// In case of mesh taken in a other scene.
			if (!meshobj) {
				continue;
			}

			gameobj->SetAutoUpdateBounds(false);

			// AABB Box : min/max.
			MT_Vector3 aabbMin;
			MT_Vector3 aabbMax;
			// Get the AABB.
			meshobj->GetAabb(aabbMin, aabbMax);
			gameobj->SetBoundsAabb(aabbMin, aabbMax);
		}
		else if (gameobj->GetMeshCount() > 0) {
			// The object allow AABB auto update only if there's no predefined bound.
			gameobj->SetAutoUpdateBounds(true);

			gameobj->UpdateBounds(true);
		}
	}

	// create physics joints
	for (i=0;i<sumolist->GetCount();i++)
	{
		PHY_IPhysicsEnvironment *physEnv = kxscene->GetPhysicsEnvironment();
		KX_GameObject *gameobj = (KX_GameObject *)sumolist->GetValue(i);
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

			KX_GameObject *gotar = getGameOb(dat->tar->id.name + 2, sumolist);

			if (gotar && (gotar->GetLayer()&activeLayerBitInfo) && gotar->GetPhysicsController() &&
				(gameobj->GetLayer()&activeLayerBitInfo) && gameobj->GetPhysicsController())
			{
				physEnv->SetupObjectConstraints(gameobj, gotar, dat);
			}
		}
	}

	/* cleanup converted set of group objects */
	convertedlist->Release();
	sumolist->Release();

	// Set the physics environment so KX_PythonComponent.start() can use bge.constraints
	KX_Scene *currentScene = KX_GetActiveScene();
	PHY_IPhysicsEnvironment *currentEnv = PHY_GetActiveEnvironment();

	KX_SetActiveScene(kxscene);
	PHY_SetActiveEnvironment(kxscene->GetPhysicsEnvironment());

	//create object representations for obstacle simulation
	KX_ObstacleSimulation* obssimulation = kxscene->GetObstacleSimulation();
	if (obssimulation)
	{
		for ( i=0;i<objectlist->GetCount();i++)
		{
			KX_GameObject* gameobj = static_cast<KX_GameObject*>(objectlist->GetValue(i));
			struct Object* blenderobject = gameobj->GetBlenderObject();
			if (blenderobject->gameflag & OB_HASOBSTACLE)
			{
				obssimulation->AddObstacleForObj(gameobj);
			}
		}
	}

	/* Restore the current scene and physics engine yet it was changed to 
	 * allow python components using the current scene and physics engine.
	 */

	KX_SetActiveScene(currentScene);
	PHY_SetActiveEnvironment(currentEnv);

	//process navigation mesh objects
	for ( i=0; i<objectlist->GetCount();i++)
	{
		KX_GameObject* gameobj = static_cast<KX_GameObject*>(objectlist->GetValue(i));
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
	for ( i=0; i<inactivelist->GetCount();i++)
	{
		KX_GameObject* gameobj = static_cast<KX_GameObject*>(inactivelist->GetValue(i));
		struct Object* blenderobject = gameobj->GetBlenderObject();
		if (blenderobject->type==OB_MESH && (blenderobject->gameflag & OB_NAVMESH))
		{
			KX_NavMeshObject* navmesh = static_cast<KX_NavMeshObject*>(gameobj);
			navmesh->SetVisible(0, true);
		}
	}

	// convert logic bricks, sensors, controllers and actuators
	for (i=0;i<logicbrick_conversionlist->GetCount();i++)
	{
		KX_GameObject* gameobj = static_cast<KX_GameObject*>(logicbrick_conversionlist->GetValue(i));
		struct Object* blenderobj = gameobj->GetBlenderObject();
		int layerMask = (groupobj.find(blenderobj) == groupobj.end()) ? activeLayerBitInfo : 0;
		bool isInActiveLayer = (blenderobj->lay & layerMask)!=0;
		BL_ConvertActuators(maggie->name, blenderobj,gameobj,logicmgr,kxscene,ketsjiEngine,layerMask,isInActiveLayer,converter);
	}
	for ( i=0;i<logicbrick_conversionlist->GetCount();i++)
	{
		KX_GameObject* gameobj = static_cast<KX_GameObject*>(logicbrick_conversionlist->GetValue(i));
		struct Object* blenderobj = gameobj->GetBlenderObject();
		int layerMask = (groupobj.find(blenderobj) == groupobj.end()) ? activeLayerBitInfo : 0;
		bool isInActiveLayer = (blenderobj->lay & layerMask)!=0;
		BL_ConvertControllers(blenderobj,gameobj,logicmgr, layerMask,isInActiveLayer,converter, libloading);
	}
	for ( i=0;i<logicbrick_conversionlist->GetCount();i++)
	{
		KX_GameObject* gameobj = static_cast<KX_GameObject*>(logicbrick_conversionlist->GetValue(i));
		struct Object* blenderobj = gameobj->GetBlenderObject();
		int layerMask = (groupobj.find(blenderobj) == groupobj.end()) ? activeLayerBitInfo : 0;
		bool isInActiveLayer = (blenderobj->lay & layerMask)!=0;
		BL_ConvertSensors(blenderobj,gameobj,logicmgr,kxscene,ketsjiEngine,layerMask,isInActiveLayer,canvas,converter);
		// set the init state to all objects
		gameobj->SetInitState((blenderobj->init_state)?blenderobj->init_state:blenderobj->state);
	}
	// apply the initial state to controllers, only on the active objects as this registers the sensors
	for ( i=0;i<objectlist->GetCount();i++)
	{
		KX_GameObject* gameobj = static_cast<KX_GameObject*>(objectlist->GetValue(i));
		gameobj->ResetState();
	}

	logicbrick_conversionlist->Release();
	
	// Calculate the scene btree -
	// too slow - commented out.
	//kxscene->SetNodeTree(tf.MakeTree());

	// instantiate dupli group, we will loop trough the object
	// that are in active layers. Note that duplicating group
	// has the effect of adding objects at the end of objectlist.
	// Only loop through the first part of the list.
	int objcount = objectlist->GetCount();
	for (i=0;i<objcount;i++)
	{
		KX_GameObject* gameobj = (KX_GameObject*) objectlist->GetValue(i);
		if (gameobj->IsDupliGroup())
		{
			kxscene->DupliGroupRecurse(gameobj, 0);
		}
	}

	/* Initialize python components, use a fixed size because some component can add object
	 * and these objects are only at the end of the list. Never use iterato here because the
	 * begining iterator can be changed and then pointed to a fake game object.
	 */
	for (unsigned int i = 0, size = objectlist->GetCount(); i < size; ++i) {
		KX_GameObject *gameobj = (KX_GameObject *)objectlist->GetValue(i);
		gameobj->InitComponents();
	}
}

