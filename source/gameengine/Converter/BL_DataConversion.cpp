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

/** \file gameengine/Converter/BL_DataConversion.cpp
 *  \ingroup bgeconv
 */

#ifdef _MSC_VER
#  pragma warning(disable : 4786)
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

/* TODO: Disabled for now, because of eval_ctx. */
#define THREADED_DAG_WORKAROUND

#include "BL_DataConversion.h"

#include <fmt/format.h>

/* This little block needed for linking to Blender... */
#ifdef WIN32
#  include "BLI_winstuff.h"
#endif

/* This list includes only data type definitions */
#include "BKE_armature.hh"
#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_main.hh"
#include "BKE_material.hh" /* give_current_material */
#include "BKE_mesh.hh"
#include "BKE_mesh_legacy_convert.hh"
#include "BKE_mesh_runtime.hh"
#include "BKE_mesh_tangent.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BKE_scene.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_actuator_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_python_proxy_types.h"
#include "wm_event_types.hh"

/* end of blender include block */

#include "BL_ArmatureObject.h"
#include "BL_SceneConverter.h"
#include "BL_ConvertActuators.h"
#include "BL_ConvertControllers.h"
#include "BL_ConvertProperties.h"
#include "BL_ConvertSensors.h"
#include "KX_BlenderMaterial.h"
#include "KX_BoneParentNodeRelationship.h"
#include "KX_Camera.h"
#include "KX_ClientObjectInfo.h"
#include "KX_EmptyObject.h"
#include "KX_FontObject.h"
#include "KX_Light.h"
#include "KX_LodManager.h"
#include "KX_MotionState.h"
#include "KX_NavMeshObject.h"
#include "KX_NodeRelationships.h"
#include "KX_ObstacleSimulation.h"
#include "KX_PyConstraintBinding.h"
#include "KX_PythonComponent.h"
#include "RAS_ICanvas.h"
#include "RAS_Vertex.h"
#ifdef WITH_BULLET
#  include "CcdPhysicsEnvironment.h"
#endif

using namespace blender;
using namespace blender::bke;

/* The reverse table. In order to not confuse ourselves, we
 * immediately convert all events that come in to KX codes. */
static std::map<int, SCA_IInputDevice::SCA_EnumInputs> gReverseKeyTranslateTable = {
    {LEFTMOUSE, SCA_IInputDevice::LEFTMOUSE},
    {MIDDLEMOUSE, SCA_IInputDevice::MIDDLEMOUSE},
    {RIGHTMOUSE, SCA_IInputDevice::RIGHTMOUSE},
    {BUTTON4MOUSE, SCA_IInputDevice::BUTTON4MOUSE},
    {BUTTON5MOUSE, SCA_IInputDevice::BUTTON5MOUSE},
    {BUTTON6MOUSE, SCA_IInputDevice::BUTTON6MOUSE},
    {BUTTON7MOUSE, SCA_IInputDevice::BUTTON7MOUSE},
    {WHEELUPMOUSE, SCA_IInputDevice::WHEELUPMOUSE},
    {WHEELDOWNMOUSE, SCA_IInputDevice::WHEELDOWNMOUSE},
    {MOUSEMOVE, SCA_IInputDevice::MOUSEX},
    {ACTIONMOUSE, SCA_IInputDevice::MOUSEY},
    // Standard keyboard.
    {EVT_AKEY, SCA_IInputDevice::AKEY},
    {EVT_BKEY, SCA_IInputDevice::BKEY},
    {EVT_CKEY, SCA_IInputDevice::CKEY},
    {EVT_DKEY, SCA_IInputDevice::DKEY},
    {EVT_EKEY, SCA_IInputDevice::EKEY},
    {EVT_FKEY, SCA_IInputDevice::FKEY},
    {EVT_GKEY, SCA_IInputDevice::GKEY},
    {EVT_HKEY, SCA_IInputDevice::HKEY_},
    {EVT_IKEY, SCA_IInputDevice::IKEY},
    {EVT_JKEY, SCA_IInputDevice::JKEY},
    {EVT_KKEY, SCA_IInputDevice::KKEY},
    {EVT_LKEY, SCA_IInputDevice::LKEY},
    {EVT_MKEY, SCA_IInputDevice::MKEY},
    {EVT_NKEY, SCA_IInputDevice::NKEY},
    {EVT_OKEY, SCA_IInputDevice::OKEY},
    {EVT_PKEY, SCA_IInputDevice::PKEY},
    {EVT_QKEY, SCA_IInputDevice::QKEY},
    {EVT_RKEY, SCA_IInputDevice::RKEY},
    {EVT_SKEY, SCA_IInputDevice::SKEY},
    {EVT_TKEY, SCA_IInputDevice::TKEY},
    {EVT_UKEY, SCA_IInputDevice::UKEY},
    {EVT_VKEY, SCA_IInputDevice::VKEY},
    {EVT_WKEY, SCA_IInputDevice::WKEY},
    {EVT_XKEY, SCA_IInputDevice::XKEY},
    {EVT_YKEY, SCA_IInputDevice::YKEY},
    {EVT_ZKEY, SCA_IInputDevice::ZKEY},

    {EVT_ZEROKEY, SCA_IInputDevice::ZEROKEY},
    {EVT_ONEKEY, SCA_IInputDevice::ONEKEY},
    {EVT_TWOKEY, SCA_IInputDevice::TWOKEY},
    {EVT_THREEKEY, SCA_IInputDevice::THREEKEY},
    {EVT_FOURKEY, SCA_IInputDevice::FOURKEY},
    {EVT_FIVEKEY, SCA_IInputDevice::FIVEKEY},
    {EVT_SIXKEY, SCA_IInputDevice::SIXKEY},
    {EVT_SEVENKEY, SCA_IInputDevice::SEVENKEY},
    {EVT_EIGHTKEY, SCA_IInputDevice::EIGHTKEY},
    {EVT_NINEKEY, SCA_IInputDevice::NINEKEY},

    {EVT_CAPSLOCKKEY, SCA_IInputDevice::CAPSLOCKKEY},

    {EVT_LEFTCTRLKEY, SCA_IInputDevice::LEFTCTRLKEY},
    {EVT_LEFTALTKEY, SCA_IInputDevice::LEFTALTKEY},
    {EVT_RIGHTALTKEY, SCA_IInputDevice::RIGHTALTKEY},
    {EVT_RIGHTCTRLKEY, SCA_IInputDevice::RIGHTCTRLKEY},
    {EVT_RIGHTSHIFTKEY, SCA_IInputDevice::RIGHTSHIFTKEY},
    {EVT_LEFTSHIFTKEY, SCA_IInputDevice::LEFTSHIFTKEY},

    {EVT_ESCKEY, SCA_IInputDevice::ESCKEY},
    {EVT_TABKEY, SCA_IInputDevice::TABKEY},
    {EVT_RETKEY, SCA_IInputDevice::RETKEY},
    {EVT_SPACEKEY, SCA_IInputDevice::SPACEKEY},
    {EVT_LINEFEEDKEY, SCA_IInputDevice::LINEFEEDKEY},
    {EVT_BACKSPACEKEY, SCA_IInputDevice::BACKSPACEKEY},
    {EVT_DELKEY, SCA_IInputDevice::DELKEY},
    {EVT_SEMICOLONKEY, SCA_IInputDevice::SEMICOLONKEY},
    {EVT_PERIODKEY, SCA_IInputDevice::PERIODKEY},
    {EVT_COMMAKEY, SCA_IInputDevice::COMMAKEY},
    {EVT_QUOTEKEY, SCA_IInputDevice::QUOTEKEY},
    {EVT_ACCENTGRAVEKEY, SCA_IInputDevice::ACCENTGRAVEKEY},
    {EVT_MINUSKEY, SCA_IInputDevice::MINUSKEY},
    {EVT_SLASHKEY, SCA_IInputDevice::SLASHKEY},
    {EVT_BACKSLASHKEY, SCA_IInputDevice::BACKSLASHKEY},
    {EVT_EQUALKEY, SCA_IInputDevice::EQUALKEY},
    {EVT_LEFTBRACKETKEY, SCA_IInputDevice::LEFTBRACKETKEY},
    {EVT_RIGHTBRACKETKEY, SCA_IInputDevice::RIGHTBRACKETKEY},

    {EVT_LEFTARROWKEY, SCA_IInputDevice::LEFTARROWKEY},
    {EVT_DOWNARROWKEY, SCA_IInputDevice::DOWNARROWKEY},
    {EVT_RIGHTARROWKEY, SCA_IInputDevice::RIGHTARROWKEY},
    {EVT_UPARROWKEY, SCA_IInputDevice::UPARROWKEY},

    {EVT_PAD2, SCA_IInputDevice::PAD2},
    {EVT_PAD4, SCA_IInputDevice::PAD4},
    {EVT_PAD6, SCA_IInputDevice::PAD6},
    {EVT_PAD8, SCA_IInputDevice::PAD8},

    {EVT_PAD1, SCA_IInputDevice::PAD1},
    {EVT_PAD3, SCA_IInputDevice::PAD3},
    {EVT_PAD5, SCA_IInputDevice::PAD5},
    {EVT_PAD7, SCA_IInputDevice::PAD7},
    {EVT_PAD9, SCA_IInputDevice::PAD9},

    {EVT_PADPERIOD, SCA_IInputDevice::PADPERIOD},
    {EVT_PADSLASHKEY, SCA_IInputDevice::PADSLASHKEY},
    {EVT_PADASTERKEY, SCA_IInputDevice::PADASTERKEY},

    {EVT_PAD0, SCA_IInputDevice::PAD0},
    {EVT_PADMINUS, SCA_IInputDevice::PADMINUS},
    {EVT_PADENTER, SCA_IInputDevice::PADENTER},
    {EVT_PADPLUSKEY, SCA_IInputDevice::PADPLUSKEY},

    {EVT_F1KEY, SCA_IInputDevice::F1KEY},
    {EVT_F2KEY, SCA_IInputDevice::F2KEY},
    {EVT_F3KEY, SCA_IInputDevice::F3KEY},
    {EVT_F4KEY, SCA_IInputDevice::F4KEY},
    {EVT_F5KEY, SCA_IInputDevice::F5KEY},
    {EVT_F6KEY, SCA_IInputDevice::F6KEY},
    {EVT_F7KEY, SCA_IInputDevice::F7KEY},
    {EVT_F8KEY, SCA_IInputDevice::F8KEY},
    {EVT_F9KEY, SCA_IInputDevice::F9KEY},
    {EVT_F10KEY, SCA_IInputDevice::F10KEY},
    {EVT_F11KEY, SCA_IInputDevice::F11KEY},
    {EVT_F12KEY, SCA_IInputDevice::F12KEY},
    {EVT_F13KEY, SCA_IInputDevice::F13KEY},
    {EVT_F14KEY, SCA_IInputDevice::F14KEY},
    {EVT_F15KEY, SCA_IInputDevice::F15KEY},
    {EVT_F16KEY, SCA_IInputDevice::F16KEY},
    {EVT_F17KEY, SCA_IInputDevice::F17KEY},
    {EVT_F18KEY, SCA_IInputDevice::F18KEY},
    {EVT_F19KEY, SCA_IInputDevice::F19KEY},

    {EVT_OSKEY, SCA_IInputDevice::OSKEY},

    {EVT_PAUSEKEY, SCA_IInputDevice::PAUSEKEY},
    {EVT_INSERTKEY, SCA_IInputDevice::INSERTKEY},
    {EVT_HOMEKEY, SCA_IInputDevice::HOMEKEY},
    {EVT_PAGEUPKEY, SCA_IInputDevice::PAGEUPKEY},
    {EVT_PAGEDOWNKEY, SCA_IInputDevice::PAGEDOWNKEY},
    {EVT_ENDKEY, SCA_IInputDevice::ENDKEY}};

SCA_IInputDevice::SCA_EnumInputs BL_ConvertKeyCode(int key_code)
{
  return gReverseKeyTranslateTable[key_code];
}

static void BL_GetUvRgba(const RAS_MeshObject::LayerList &layers,
                         unsigned int loop,
                         MT_Vector2 uvs[RAS_Texture::MaxUnits],
                         unsigned int rgba[RAS_IVertex::MAX_UNIT],
                         unsigned short uvLayers,
                         unsigned short colorLayers)
{
  // No need to initialize layers to zero as all the converted layer are all the layers needed.

  for (const RAS_MeshObject::Layer &layer : layers) {
    const unsigned short index = layer.index;
    if (layer.color) {
      const MLoopCol &col = layer.color[loop];

      union Convert {
        // Color isn't swapped in MLoopCol.
        MLoopCol col;
        unsigned int val;
      };
      Convert con;
      con.col = col;

      rgba[index] = con.val;
    }
    else if (layer.luvs) {
      const float *uv = &layer.luvs[loop][0];
      uvs[index].setValue(uv);
    }
  }

  /* All vertices have at least one uv and color layer accessible to the user
   * even if it they are not used in any shaders. Initialize this layer to zero
   * when no uv or color layer exist.
   */
  if (uvLayers == 0) {
    uvs[0] = MT_Vector2(0.0f, 0.0f);
  }
  if (colorLayers == 0) {
    rgba[0] = 0xFFFFFFFF;
  }
}

static KX_BlenderMaterial *BL_ConvertMaterial(Material *mat,
                                              int lightlayer,
                                              KX_Scene *scene,
                                              RAS_Rasterizer *rasty,
                                              bool converting_during_runtime)
{
  std::string name = mat->id.name;
  // Always ensure that the name of a material start with "MA" prefix due to video texture name
  // check.
  if (name.empty()) {
    name = "MA";
  }

  KX_BlenderMaterial *kx_blmat = new KX_BlenderMaterial(rasty,
                                                        scene,
                                                        mat,
                                                        name,
                                                        (mat ? &mat->game : nullptr),
                                                        lightlayer,
                                                        converting_during_runtime);

  return kx_blmat;
}

static RAS_MaterialBucket *BL_material_from_mesh(Material *ma,
                                                 int lightlayer,
                                                 KX_Scene *scene,
                                                 RAS_Rasterizer *rasty,
                                                 BL_SceneConverter *converter,
                                                 bool converting_during_runtime)
{
  KX_BlenderMaterial *mat = converter->FindMaterial(ma);

  if (!mat) {
    mat = BL_ConvertMaterial(ma, lightlayer, scene, rasty, converting_during_runtime);
    // this is needed to free up memory afterwards.
    converter->RegisterMaterial(mat, ma);
  }

  // see if a bucket was reused or a new one was created
  // this way only one KX_BlenderMaterial object has to exist per bucket
  bool bucketCreated;
  RAS_MaterialBucket *bucket = scene->FindBucket(mat, bucketCreated);

  return bucket;
}

static int GetPolygonMaterialIndex(const VArray<int> mat_indices, const Mesh *me, int polyid)
{
  int r = mat_indices[polyid];

  /* Random attempt to fix issue related to boolean exact solver
   * https://github.com/UPBGE/upbge/issues/1789 */
  if (r > me->totcol - 1) {
    r = 0;
  }
  return r;
}

/* blenderobj can be nullptr, make sure its checked for */
RAS_MeshObject *BL_ConvertMesh(Mesh *mesh,
                               Object *blenderobj,
                               KX_Scene *scene,
                               RAS_Rasterizer *rasty,
                               BL_SceneConverter *converter,
                               bool libloading,
                               bool converting_during_runtime)
{
  RAS_MeshObject *meshobj;
  int lightlayer = blenderobj ? blenderobj->lay : (1 << 20) - 1;  // all layers if no object.

  // Without checking names, we get some reuse we don't want that can cause
  // problems with material LoDs.
  if (blenderobj && ((meshobj = converter->FindGameMesh(mesh /*, ob->lay*/)) != nullptr)) {
    const std::string bge_name = meshobj->GetName();
    const std::string blender_name = ((ID *)blenderobj->data)->name + 2;
    if (bge_name == blender_name) {
      return meshobj;
    }
  }

  // Get Mesh data
  bContext *C = KX_GetActiveEngine()->GetContext();
  Depsgraph *depsgraph = CTX_data_depsgraph_on_load(C);
  Object *ob_eval = DEG_get_evaluated_object(depsgraph, blenderobj);
  Mesh *final_me = (Mesh *)ob_eval->data;

  BKE_mesh_tessface_ensure(final_me);

  const blender::Span<blender::float3> positions = final_me->vert_positions();
  const int totverts = final_me->verts_num;

  const MFace *faces = (MFace *)CustomData_get_layer(&final_me->fdata_legacy, CD_MFACE);
  const int totfaces = final_me->totface_legacy;
  const int *mfaceToMpoly = (int *)CustomData_get_layer(&final_me->fdata_legacy, CD_ORIGINDEX);

  /* Extract available layers.
   * Get the active color and uv layer. */
  const short activeUv = CustomData_get_active_layer(&final_me->corner_data, CD_PROP_FLOAT2);
  const short activeColor = CustomData_get_active_layer(&final_me->corner_data,
                                                        CD_PROP_BYTE_COLOR);

  RAS_MeshObject::LayersInfo layersInfo;
  layersInfo.activeUv = (activeUv == -1) ? 0 : activeUv;
  layersInfo.activeColor = (activeColor == -1) ? 0 : activeColor;

  const unsigned short uvLayers = CustomData_number_of_layers(&final_me->corner_data,
                                                              CD_PROP_FLOAT2);
  const unsigned short colorLayers = CustomData_number_of_layers(&final_me->corner_data,
                                                                 CD_PROP_BYTE_COLOR);

  // Extract UV loops.
  for (unsigned short i = 0; i < uvLayers; ++i) {
    const std::string name = CustomData_get_layer_name(&final_me->corner_data, CD_PROP_FLOAT2, i);
    const float(*uv)[2] = (const float(*)[2])CustomData_get_layer_n(
        &final_me->corner_data, CD_PROP_FLOAT2, i);
    layersInfo.layers.push_back({uv, nullptr, i, name});
  }
  // Extract color loops.
  for (unsigned short i = 0; i < colorLayers; ++i) {
    const std::string name = CustomData_get_layer_name(
        &final_me->corner_data, CD_PROP_BYTE_COLOR, i);
    MLoopCol *col = (MLoopCol *)CustomData_get_layer_n(
        &final_me->corner_data, CD_PROP_BYTE_COLOR, i);
    layersInfo.layers.push_back({nullptr, col, i, name});
  }

  blender::Span<float3> loop_nors_dst;
  float(*loop_normals)[3] = (float(*)[3])CustomData_get_layer(&final_me->corner_data, CD_NORMAL);
  const bool do_loop_nors = (loop_normals == nullptr);
  if (do_loop_nors) {
    loop_nors_dst = final_me->corner_normals();
  }

  const bke::AttributeAccessor attributes = final_me->attributes();

  float(*tangent)[4] = nullptr;
  if (uvLayers > 0) {
    if (CustomData_get_layer_index(&final_me->corner_data, CD_TANGENT) == -1) {
      short tangent_mask = 0;
      const blender::Span<int3> corner_tris = final_me->corner_tris();
      const VArraySpan sharp_face = *attributes.lookup<bool>("sharp_face", AttrDomain::Face);
      const float3 *orco = static_cast<const float3 *>(
          CustomData_get_layer(&final_me->vert_data, CD_ORCO));
      BKE_mesh_calc_loop_tangent_ex(
          final_me->vert_positions(),
          final_me->faces(),
          final_me->corner_verts().data(),
          corner_tris.data(),
          final_me->corner_tri_faces().data(),
          uint(corner_tris.size()),
          sharp_face,
          &final_me->corner_data,
          true,
          nullptr,
          0,
          final_me->vert_normals(),
          final_me->face_normals(),
          final_me->corner_normals(),
          /* may be nullptr */
          orco ? Span(orco, final_me->verts_num) : Span<float3>(),
          /* result */
          &final_me->corner_data,
          uint(final_me->corners_num),
          &tangent_mask);
    }
    tangent = (float(*)[4])CustomData_get_layer(&final_me->corner_data, CD_TANGENT);
  }

  meshobj = new RAS_MeshObject(mesh, final_me->verts_num, blenderobj, layersInfo);
  meshobj->m_sharedvertex_map.resize(totverts);

  // Initialize vertex format with used uv and color layers.
  RAS_VertexFormat vertformat;
  vertformat.uvSize = max_ii(1, uvLayers);
  vertformat.colorSize = max_ii(1, colorLayers);

  struct ConvertedMaterial {
    Material *ma;
    RAS_MeshMaterial *meshmat;
    bool visible;
    bool twoside;
    bool collider;
    bool wire;
  };

  const unsigned short totmat = max_ii(final_me->totcol, 1);
  std::vector<ConvertedMaterial> convertedMats(totmat);

  // Convert all the materials contained in the mesh.
  for (unsigned short i = 0; i < totmat; ++i) {
    Material *ma = nullptr;
    if (blenderobj) {
      ma = BKE_object_material_get(ob_eval, i + 1);
    }
    else {
      ma = final_me->mat ? final_me->mat[i] : nullptr;
    }
    // Check for blender material
    if (!ma) {
      ma = BKE_material_default_empty();
    }

    RAS_MaterialBucket *bucket = BL_material_from_mesh(
        ma, lightlayer, scene, rasty, converter, converting_during_runtime);
    RAS_MeshMaterial *meshmat = meshobj->AddMaterial(bucket, i, vertformat);

    convertedMats[i] = {ma,
                        meshmat,
                        ((ma->game.flag & GEMAT_INVISIBLE) == 0),
                        ((ma->game.flag & GEMAT_BACKCULL) == 0),
                        ((ma->game.flag & GEMAT_NOPHYSICS) == 0),
                        bucket->IsWire()};
  }

  std::vector<std::vector<unsigned int>> mpolyToMface(final_me->faces().size());
  // Generate a list of all mfaces wrapped by a mpoly.
  for (unsigned int i = 0; i < totfaces; ++i) {
    mpolyToMface[mfaceToMpoly[i]].push_back(i);
  }

  // Tracked vertices during a mpoly conversion, should never be used by the next mpoly.
  std::vector<unsigned int> vertices(totverts, -1);

  const VArray<int> material_indices = *attributes.lookup_or_default<int>(
      "material_index", AttrDomain::Face, 0);

  const bool *sharp_faces = static_cast<const bool *>(
      CustomData_get_layer_named(&final_me->face_data, CD_PROP_BOOL, "sharp_face"));

  const Span<int> corner_verts = final_me->corner_verts();
  const Span<int> corner_edges = final_me->corner_edges();
  const Span<int2> edges = final_me->edges();

  const OffsetIndices polys = final_me->faces();

  for (const unsigned int i : polys.index_range()) {
    /* Try to get evaluated mesh poly material index */
    /* Old code was: const ConvertedMaterial &mat = convertedMats[mpoly.mat_nr_legacy]; */
    /* There is still an issue with boolean exact solver with polygon material indice */
    int mat_nr = GetPolygonMaterialIndex(material_indices, final_me, i);

    const ConvertedMaterial &mat = convertedMats[mat_nr];

    RAS_MeshMaterial *meshmat = mat.meshmat;

    // Mark face as flat, so vertices are split.
    const bool flat = (sharp_faces && sharp_faces[i]);

    for (const unsigned int vert_i : corner_verts.slice(polys[i])) {
      const float *vp = &positions[vert_i][0];

      const MT_Vector3 pt(vp);
      const MT_Vector3 no(do_loop_nors ? MT_Vector3(loop_nors_dst[vert_i].x, loop_nors_dst[vert_i].y, loop_nors_dst[vert_i].z) :
                         MT_Vector3(loop_normals[vert_i][0], loop_normals[vert_i][1], loop_normals[vert_i][2]));
      const MT_Vector4 tan = tangent ? MT_Vector4(tangent[vert_i]) : MT_Vector4(0.0f, 0.0f, 0.0f, 0.0f);
      MT_Vector2 uvs[RAS_Texture::MaxUnits];
      unsigned int rgba[RAS_Texture::MaxUnits];

      BL_GetUvRgba(layersInfo.layers, vert_i, uvs, rgba, uvLayers, colorLayers);

      // Add tracked vertices by the mpoly.
      vertices[vert_i] = meshobj->AddVertex(meshmat, pt, uvs, tan, rgba, no, flat, vert_i);
    }

    // Convert to edges of material is rendering wire.
    if (mat.wire && mat.visible) {
      for (const unsigned int edge_i : corner_edges.slice(polys[i])) {
        const int2 &edge = edges[edge_i];
        meshobj->AddLine(meshmat, vertices[edge[0]], vertices[edge[1]]);
      }
    }

    // Convert all faces (triangles of quad).
    for (unsigned int j : mpolyToMface[i]) {
      const MFace &face = faces[j];
      const unsigned short nverts = (face.v4) ? 4 : 3;
      unsigned int indices[4];
      indices[0] = vertices[face.v1];
      indices[1] = vertices[face.v2];
      indices[2] = vertices[face.v3];
      if (face.v4) {
        indices[3] = vertices[face.v4];
      }

      meshobj->AddPolygon(meshmat, nverts, indices, mat.visible, mat.collider, mat.twoside);
    }
  }

  // keep meshobj->m_sharedvertex_map for reinstance phys mesh.
  // 2.49a and before it did: meshobj->m_sharedvertex_map.clear();
  // but this didnt save much ram. - Campbell
  meshobj->EndConversion();

  // Finalize materials.
  // However, we want to delay this if we're libloading so we can make sure we have the right
  // scene.
  if (!libloading) {
    for (unsigned short i = 0, num = meshobj->NumMaterials(); i < num; ++i) {
      RAS_MeshMaterial *mmat = meshobj->GetMeshMaterial(i);
      mmat->GetBucket()->GetPolyMaterial()->OnConstruction();
    }
  }

  converter->RegisterGameMesh(meshobj, mesh);
  return meshobj;
}

//////////////////////////////////////////////////////
static void BL_CreatePhysicsObjectNew(KX_GameObject *gameobj,
                                      Object *blenderobject,
                                      RAS_MeshObject *meshobj,
                                      KX_Scene *kxscene,
                                      int activeLayerBitInfo,
                                      BL_SceneConverter *converter,
                                      bool processCompoundChildren)

{
  // Object has physics representation?
  if (!(blenderobject->gameflag & OB_COLLISION)) {
    return;
  }

  Object *parent = blenderobject->parent;

  bool isCompoundChild = false;
  bool hasCompoundChildren = false;

  // Pretend for compound parent or child if the object has compound option and use a physics type
  // with solid shape.
  if ((blenderobject->gameflag & (OB_CHILD)) &&
      (blenderobject->gameflag & (OB_DYNAMIC | OB_COLLISION | OB_RIGID_BODY)) &&
      !(blenderobject->gameflag & OB_SOFT_BODY)) {
    hasCompoundChildren = true;
    while (parent) {
      if ((parent->gameflag & OB_CHILD) &&
          (parent->gameflag & (OB_COLLISION | OB_DYNAMIC | OB_RIGID_BODY)) &&
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

  PHY_IMotionState *motionstate = new KX_MotionState(gameobj->GetSGNode());

  PHY_IPhysicsEnvironment *phyenv = kxscene->GetPhysicsEnvironment();
  phyenv->ConvertObject(converter,
                        gameobj,
                        meshobj,
                        kxscene,
                        motionstate,
                        activeLayerBitInfo,
                        isCompoundChild,
                        hasCompoundChildren);

  bool isActor = (blenderobject->gameflag & OB_ACTOR) != 0;
  bool isSensor = (blenderobject->gameflag & OB_SENSOR) != 0;
  gameobj->getClientInfo()->m_type = (isSensor) ? ((isActor) ? KX_ClientObjectInfo::OBACTORSENSOR :
                                                               KX_ClientObjectInfo::OBSENSOR) :
                                     (isActor)  ? KX_ClientObjectInfo::ACTOR :
                                                  KX_ClientObjectInfo::STATIC;
}

static KX_LodManager *BL_lodmanager_from_blenderobject(Object *ob,
                                                       KX_Scene *scene,
                                                       RAS_Rasterizer *rasty,
                                                       BL_SceneConverter *converter,
                                                       bool libloading,
                                                       bool converting_during_runtime)
{
  if (BLI_listbase_count_at_most(&ob->lodlevels, 2) <= 1) {
    return nullptr;
  }

  KX_LodManager *lodManager = new KX_LodManager(
      ob, scene, rasty, converter, libloading, converting_during_runtime);
  // The lod manager is useless ?
  if (lodManager->GetLevelCount() <= 1) {
    lodManager->Release();
    return nullptr;
  }

  return lodManager;
}

/** Convert the object activity culling settings from blender to a
 * KX_GameObject::ActivityCullingInfo. \param ob The object to convert the activity culling
 * settings from.
 */
static KX_GameObject::ActivityCullingInfo activityCullingInfoFromBlenderObject(Object *ob)
{
  KX_GameObject::ActivityCullingInfo cullingInfo;
  const ObjectActivityCulling &blenderInfo = ob->activityCulling;
  // Convert the flags.
  if (blenderInfo.flags & OB_ACTIVITY_PHYSICS) {
    // Enable physics culling.
    cullingInfo.m_flags = (KX_GameObject::ActivityCullingInfo::
                               Flag)(cullingInfo.m_flags |
                                     KX_GameObject::ActivityCullingInfo::ACTIVITY_PHYSICS);
  }
  if (blenderInfo.flags & OB_ACTIVITY_LOGIC) {
    // Enable logic culling.
    cullingInfo.m_flags = (KX_GameObject::ActivityCullingInfo::
                               Flag)(cullingInfo.m_flags |
                                     KX_GameObject::ActivityCullingInfo::ACTIVITY_LOGIC);
  }

  // Set culling radius.
  cullingInfo.m_physicsRadius = blenderInfo.physicsRadius * blenderInfo.physicsRadius;
  cullingInfo.m_logicRadius = blenderInfo.logicRadius * blenderInfo.logicRadius;

  return cullingInfo;
}

#ifdef WITH_PYTHON
static KX_GameObject *BL_gameobject_from_customobject(Object *ob,
                                                      PyTypeObject *type,
                                                      KX_Scene *kxscene)
{
  KX_GameObject *gameobj = nullptr;

  PythonProxy *pp = ob->custom_object;

  if (!pp) {
    return nullptr;
  }

  PyObject *arg_dict = NULL, *args = NULL, *mod = NULL, *cls = NULL, *pyobj = NULL, *ret = NULL;

  args = arg_dict = mod = cls = pyobj = ret = NULL;

  // Grab the module
  mod = PyImport_ImportModule(pp->module);

  bool valid = false;

  if (mod == NULL) {
    std::string msg = fmt::format("Failed to import the module {}", pp->module);
    kxscene->LogError(msg);
  }
  else {
    // Grab the class object
    cls = PyObject_GetAttrString(mod, pp->name);

    if (cls == NULL) {
      std::string msg = fmt::format("Python module found, but failed to find the object {}", pp->name);
      kxscene->LogError(msg);
    }
    else if (!PyType_Check(cls) || !PyObject_IsSubclass(cls, (PyObject *)type)) {
      std::string msg = fmt::format("{}.{} is not a subclass of {}", pp->name, pp->name, type->tp_name);
      kxscene->LogError(msg);
    }
    else {
      valid = true;
    }
  }

  if (valid) {
    // Every thing checks out, now generate the args dictionary and init the component
    args = PyTuple_Pack(0);

    pyobj = PyObject_Call(cls, args, NULL);

    if (PyErr_Occurred()) {
      // The component is invalid, drop it
      std::string msg = fmt::format("Failed to instantiate the class {}", pp->name);
      kxscene->LogError(msg);
    }
    else {
      gameobj = static_cast<KX_GameObject *>(EXP_PROXY_REF(pyobj));
    }
  }

  if (gameobj) {
    gameobj->SetPrototype(pp);
  }

  Py_XDECREF(args);
  Py_XDECREF(mod);
  Py_XDECREF(cls);
  Py_XDECREF(pyobj);

  return gameobj;
}
#endif

static KX_GameObject *BL_gameobject_from_blenderobject(Object *ob,
                                                       KX_Scene *kxscene,
                                                       RAS_Rasterizer *rasty,
                                                       BL_SceneConverter *converter,
                                                       bool libloading,
                                                       bool converting_during_runtime)
{
  KX_GameObject *gameobj = nullptr;

  switch (ob->type) {
    case OB_LAMP: {
      KX_LightObject *gamelight = nullptr;
#ifdef WITH_PYTHON
      KX_GameObject *customobj = BL_gameobject_from_customobject(ob, &KX_LightObject::Type, kxscene);

      if (customobj) {
        gamelight = dynamic_cast<KX_LightObject *>(customobj);
      }
#endif
      if (!gamelight) {
        gamelight = new KX_LightObject();
      }

      gameobj = gamelight;
      gamelight->AddRef();
      kxscene->GetLightList()->Add(gamelight);

      break;
    }

    case OB_CAMERA: {
      KX_Camera *gamecamera = nullptr;
#ifdef WITH_PYTHON
      KX_GameObject *customobj = BL_gameobject_from_customobject(ob, &KX_Camera::Type, kxscene);

      if (customobj) {
        gamecamera = dynamic_cast<KX_Camera *>(customobj);
      }
#endif
      if (!gamecamera) {
        gamecamera = new KX_Camera();
      }

      // don't add a reference: the camera list in kxscene->m_cameras is not released at the end
      // gamecamera->AddRef();
      kxscene->GetCameraList()->Add(CM_AddRef(gamecamera));

      gameobj = gamecamera;

      break;
    }

    case OB_MESH: {
      Mesh *mesh = static_cast<Mesh *>(ob->data);
      RAS_MeshObject *meshobj = BL_ConvertMesh(
          mesh, ob, kxscene, rasty, converter, libloading, converting_during_runtime);

      // needed for python scripting
      kxscene->GetLogicManager()->RegisterMeshName(meshobj->GetName(), meshobj);

      if (ob->gameflag & OB_NAVMESH) {
#ifdef WITH_PYTHON
        gameobj = BL_gameobject_from_customobject(ob, &KX_NavMeshObject::Type, kxscene);
#endif
        if (!gameobj) {
          gameobj = new KX_NavMeshObject();
        }

        gameobj->AddMesh(meshobj);
        break;
      }
      else {
#ifdef WITH_PYTHON
        gameobj = BL_gameobject_from_customobject(ob, &KX_GameObject::Type, kxscene);
#endif
        if (!gameobj) {
          gameobj = new KX_EmptyObject();
        }
      }

      // set transformation
      gameobj->AddMesh(meshobj);

      // gather levels of detail
      KX_LodManager *lodManager = BL_lodmanager_from_blenderobject(
          ob, kxscene, rasty, converter, libloading, converting_during_runtime);
      gameobj->SetLodManager(lodManager);
      if (lodManager) {
        lodManager->Release();
        kxscene->AddObjToLodObjList(gameobj);
      }
      else {
        /* Just in case */
        kxscene->RemoveObjFromLodObjList(gameobj);
      }

      gameobj->SetOccluder((ob->gameflag & OB_OCCLUDER) != 0, false);
      break;
    }

    case OB_ARMATURE: {
#ifdef WITH_PYTHON
      gameobj = BL_gameobject_from_customobject(ob, &BL_ArmatureObject::Type, kxscene);
#endif
      if (!gameobj) {
        gameobj = new BL_ArmatureObject();
      }

      kxscene->AddAnimatedObject(gameobj);

      break;
    }

    case OB_EMPTY:
    case OB_LIGHTPROBE:
    case OB_MBALL:
    case OB_SURF:
    case OB_GREASE_PENCIL:
    case OB_SPEAKER: {
#ifdef WITH_PYTHON
      gameobj = BL_gameobject_from_customobject(ob, &KX_GameObject::Type, kxscene);
#endif

      if (!gameobj) {
        gameobj = new KX_EmptyObject();
      }
      // set transformation
      break;
    }

    case OB_FONT: {
      /* font objects have no bounding box */
      KX_FontObject *fontobj = nullptr;
#ifdef WITH_PYTHON
      KX_GameObject *customobj = BL_gameobject_from_customobject(ob, &KX_FontObject::Type, kxscene);

      if (customobj) {
        fontobj = dynamic_cast<KX_FontObject *>(customobj);
      }
#endif
      if (!fontobj) {
        fontobj = new KX_FontObject();
      }

      fontobj->SetRasterizer(rasty);

      gameobj = fontobj;

      kxscene->GetFontList()->Add(CM_AddRef(fontobj));
      break;
    }

#ifdef THREADED_DAG_WORKAROUND
    case OB_CURVES_LEGACY: {
      /*bContext *C = KX_GetActiveEngine()->GetContext();
      if (ob->runtime.curve_cache == nullptr) {
        Depsgraph *depsgraph = CTX_data_depsgraph_on_load(C);
        BKE_displist_make_curveTypes(
            depsgraph, blenderscene, DEG_get_evaluated_object(depsgraph, ob), false, false);
      }*/
      // eevee add curves to scene.objects list
#ifdef WITH_PYTHON
      gameobj = BL_gameobject_from_customobject(ob, &KX_GameObject::Type, kxscene);
#endif
      if (!gameobj) {
        gameobj = new KX_EmptyObject();
      }
      // set transformation
      break;
    }
#endif
  }

  if (gameobj) {
    if (ob->type != OB_CAMERA) {
      gameobj->SetActivityCullingInfo(activityCullingInfoFromBlenderObject(ob));
    }

    gameobj->SetLayer(ob->lay);
    gameobj->SetScene(kxscene);
    gameobj->SetBlenderObject(ob);

    /* Bakup Objects object_to_world to restore at scene exit */
    if (kxscene->GetBlenderScene()->gm.flag & GAME_USE_UNDO) {
      if (!converting_during_runtime) {
        BackupObj *backup = new BackupObj();  // Can't allocate on stack
        backup->ob = ob;
        backup->obtfm = BKE_object_tfm_backup(ob);
        kxscene->BackupObjectsMatToWorld(backup);
      }
    }

    gameobj->SetObjectColor(MT_Vector4(ob->color));
    /* set the visibility state based on the objects render option in the outliner */
    /* I think this flag was used as visibility option for physics shape in 2.7,
     * and it seems it can still be used for this purpose in checking it in outliner
     * even if I removed the button from physics tab. (youle)
     */
    if (ob->visibility_flag & OB_HIDE_RENDER)
      gameobj->SetVisible(0, 0);
  }
  return gameobj;
}

struct BL_parentChildLink {
  struct Object *m_blenderchild;
  SG_Node *m_gamechildnode;
};

static ListBase *BL_GetActiveConstraint(Object *ob)
{
  if (!ob)
    return nullptr;

  return &ob->constraints;
}

static void BL_ConvertComponentsObject(KX_GameObject *gameobj, Object *blenderobj)
{
#ifdef WITH_PYTHON
  PythonProxy *pp = (PythonProxy *)blenderobj->components.first;
  PyObject *arg_dict = NULL, *args = NULL, *mod = NULL, *cls = NULL, *pycomp = NULL, *ret = NULL;

  if (!pp) {
    return;
  }

  EXP_ListValue<KX_PythonComponent> *components = new EXP_ListValue<KX_PythonComponent>();

  while (pp) {
    // Make sure to clean out anything from previous loops
    Py_XDECREF(args);
    Py_XDECREF(arg_dict);
    Py_XDECREF(mod);
    Py_XDECREF(cls);
    Py_XDECREF(ret);
    Py_XDECREF(pycomp);
    args = arg_dict = mod = cls = pycomp = ret = NULL;

    // Grab the module
    mod = PyImport_ImportModule(pp->module);

    if (mod == NULL) {
      std::string msg = fmt::format("Failed to import the module {}", pp->module);
      gameobj->LogError(msg);

      pp = pp->next;
      continue;
    }

    // Grab the class object
    cls = PyObject_GetAttrString(mod, pp->name);
    if (cls == NULL) {
      std::string msg =
          fmt::format("Python module found, but failed to find the component {}", pp->name);
      gameobj->LogError(msg);

      pp = pp->next;
      continue;
    }

    // Lastly make sure we have a class and it's an appropriate sub type
    if (!PyType_Check(cls) || !PyObject_IsSubclass(cls, (PyObject *)&KX_PythonComponent::Type)) {
      std::string msg = fmt::format("{}.{} is not a KX_PythonComponent subclass", pp->module, pp->name);

      gameobj->LogError(msg);

      pp = pp->next;
      continue;
    }

    // Every thing checks out, now generate the args dictionary and init the component
    args = PyTuple_Pack(1, gameobj->GetProxy());

    pycomp = PyObject_Call(cls, args, NULL);

    if (PyErr_Occurred()) {
      std::string msg = fmt::format("Failed to instantiate the class {}", pp->name);
      gameobj->LogError(msg);
    }
    else {
      KX_PythonComponent *comp = static_cast<KX_PythonComponent *>(EXP_PROXY_REF(pycomp));
      comp->SetPrototype(pp);
      comp->SetGameObject(gameobj);
      components->Add(comp);
    }

    pp = pp->next;
  }

  Py_XDECREF(args);
  Py_XDECREF(mod);
  Py_XDECREF(cls);
  Py_XDECREF(pycomp);

  gameobj->SetComponents(components);
#else
  (void)gameobj;
  (void)blenderobj;
#endif
}

static std::vector<Object *> lod_level_object_list(ViewLayer *view_layer)
{
  Base *base = (Base *)view_layer->object_bases.first;
  std::vector<Object *> lod_objs = {};

  while (base) {
    Object *ob = base->object;
    if (ob) {
      LISTBASE_FOREACH (LodLevel *, level, &ob->lodlevels) {
        if (level->source) {
          lod_objs.push_back(level->source);
        }
      }
    }
    base = base->next;
  }
  return lod_objs;
}

static bool is_lod_level(std::vector<Object *> lod_objs, Object *blenderobject)
{
  return std::find(lod_objs.begin(), lod_objs.end(), blenderobject) != lod_objs.end();
}

/* helper for BL_ConvertBlenderObjects, avoids code duplication
 * note: all var names match args are passed from the caller */
static void bl_ConvertBlenderObject_Single(BL_SceneConverter *converter,
                                           Object *blenderobject,
                                           std::vector<BL_parentChildLink> &vec_parent_child,
                                           EXP_ListValue<KX_GameObject> *logicbrick_conversionlist,
                                           EXP_ListValue<KX_GameObject> *objectlist,
                                           EXP_ListValue<KX_GameObject> *inactivelist,
                                           EXP_ListValue<KX_GameObject> *sumolist,
                                           KX_Scene *kxscene,
                                           KX_GameObject *gameobj,
                                           SCA_LogicManager *logicmgr,
                                           SCA_TimeEventManager *timemgr,
                                           bool isInActiveLayer)
{
  MT_Vector3 pos(blenderobject->loc[0] + blenderobject->dloc[0],
                 blenderobject->loc[1] + blenderobject->dloc[1],
                 blenderobject->loc[2] + blenderobject->dloc[2]);

  MT_Matrix3x3 rotation;
  float rotmat[3][3];
  BKE_object_rot_to_mat3(blenderobject, rotmat, false);
  rotation.setValue3x3((float *)rotmat);

  MT_Vector3 scale(blenderobject->scale);

  gameobj->NodeSetLocalPosition(pos);
  gameobj->NodeSetLocalOrientation(rotation);
  gameobj->NodeSetLocalScale(scale);
  gameobj->NodeUpdateGS(0);

  sumolist->Add(CM_AddRef(gameobj));

  BL_ConvertProperties(blenderobject, gameobj, timemgr, kxscene, isInActiveLayer);

  gameobj->SetName(blenderobject->id.name + 2);

  // Update children/parent hierarchy
  if (blenderobject->parent != 0) {
    // Blender has an additional 'parentinverse' offset in each object
    SG_Callbacks callback(nullptr,
                          nullptr,
                          nullptr,
                          KX_Scene::KX_ScenegraphUpdateFunc,
                          KX_Scene::KX_ScenegraphRescheduleFunc);
    SG_Node *parentinversenode = new SG_Node(nullptr, kxscene, callback);

    // Define a normal parent relationship for this node.
    KX_NormalParentRelation *parent_relation = new KX_NormalParentRelation();
    parentinversenode->SetParentRelation(parent_relation);

    BL_parentChildLink pclink;
    pclink.m_blenderchild = blenderobject;
    pclink.m_gamechildnode = parentinversenode;
    vec_parent_child.push_back(pclink);

    float *fl = (float *)blenderobject->parentinv;
    MT_Transform parinvtrans(fl);
    parentinversenode->SetLocalPosition(parinvtrans.getOrigin());
    // problem here: the parent inverse transform combines scaling and rotation
    // in the basis but the scenegraph needs separate rotation and scaling.
    // This is not important for OpenGL (it uses 4x4 matrix) but it is important
    // for the physic engine that needs a separate scaling
    // parentinversenode->SetLocalOrientation(parinvtrans.getBasis());

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

  // Needed for python scripting
  logicmgr->RegisterGameObjectName(gameobj->GetName(), gameobj);

  // Needed for group duplication
  logicmgr->RegisterGameObj(blenderobject, gameobj);
  for (int i = 0; i < gameobj->GetMeshCount(); i++)
    logicmgr->RegisterGameMeshName(gameobj->GetMesh(i)->GetName(), blenderobject);

  converter->RegisterGameObject(gameobj, blenderobject);
  // this was put in rapidly, needs to be looked at more closely
  // only draw/use objects in active 'blender' layers

  logicbrick_conversionlist->Add(CM_AddRef(gameobj));

  if (isInActiveLayer) {
    objectlist->Add(CM_AddRef(gameobj));
    // tf.Add(gameobj->GetSGNode());

    gameobj->NodeUpdateGS(0);
  }
  else {
    // we must store this object otherwise it will be deleted
    // at the end of this function if it is not a root object
    inactivelist->Add(CM_AddRef(gameobj));
  }
}

// convert blender objects into ketsji gameobjects
void BL_ConvertBlenderObjects(struct Main *maggie,
                              struct Depsgraph *depsgraph,
                              KX_Scene *kxscene,
                              KX_KetsjiEngine *ketsjiEngine,
                              e_PhysicsEngine physics_engine,
                              RAS_Rasterizer *rendertools,
                              RAS_ICanvas *canvas,
                              BL_SceneConverter *converter,
                              Object *single_object,
                              bool alwaysUseExpandFraming,
                              bool libloading)
{

#define BL_CONVERTBLENDEROBJECT_SINGLE \
  bl_ConvertBlenderObject_Single(converter, \
                                 blenderobject, \
                                 vec_parent_child, \
                                 logicbrick_conversionlist, \
                                 objectlist, \
                                 inactivelist, \
                                 sumolist, \
                                 kxscene, \
                                 gameobj, \
                                 logicmgr, \
                                 timemgr, \
                                 isInActiveLayer)

  Scene *blenderscene = kxscene->GetBlenderScene();
  Scene *sce_iter;
  Base *base;

  // Get the frame settings of the canvas.
  // Get the aspect ratio of the canvas as designed by the user.

  RAS_FrameSettings::RAS_FrameType frame_type;
  int aspect_width;
  int aspect_height;
  std::set<Collection *> grouplist;  // list of groups to be converted
  std::set<Object *> groupobj;       // objects from groups (never in active layer)

  /* We have to ensure that group definitions are only converted once
   * push all converted group members to this set.
   * This will happen when a group instance is made from a linked group instance
   * and both are on the active layer. */
  EXP_ListValue<KX_GameObject> *convertedlist = new EXP_ListValue<KX_GameObject>();

  if (!single_object) {

    if (alwaysUseExpandFraming) {
      frame_type = RAS_FrameSettings::e_frame_extend;
      aspect_width = canvas->GetWidth();
      aspect_height = canvas->GetHeight();
    }
    else {
      // if (blenderscene->gm.framing.type == SCE_GAMEFRAMING_BARS) {
      frame_type = RAS_FrameSettings::e_frame_extend;  // RAS_FrameSettings::e_frame_bars;
      //} else if (blenderscene->gm.framing.type == SCE_GAMEFRAMING_EXTEND) {
      // frame_type = RAS_FrameSettings::e_frame_extend;
      //} else {
      // frame_type = RAS_FrameSettings::e_frame_scale;
      //}

      aspect_width = (int)(blenderscene->r.xsch * blenderscene->r.xasp);
      aspect_height = (int)(blenderscene->r.ysch * blenderscene->r.yasp);
    }

    RAS_FrameSettings frame_settings(frame_type,
                                     blenderscene->gm.framing.col[0],
                                     blenderscene->gm.framing.col[1],
                                     blenderscene->gm.framing.col[2],
                                     aspect_width,
                                     aspect_height);
    kxscene->SetFramingType(frame_settings);

    kxscene->SetGravity(MT_Vector3(0, 0, -blenderscene->gm.gravity));

    /* set activity culling parameters */
    kxscene->SetActivityCulling((blenderscene->gm.mode & WO_ACTIVITY_CULLING) != 0);
    kxscene->SetDbvtCulling(false);

    // no occlusion culling by default
    kxscene->SetDbvtOcclusionRes(0);

    if (blenderscene->gm.lodflag & SCE_LOD_USE_HYST) {
      kxscene->SetLodHysteresis(true);
      kxscene->SetLodHysteresisValue(blenderscene->gm.scehysteresis);
    }
  }

  int activeLayerBitInfo = blenderscene->lay;

  // list of all object converted, active and inactive
  EXP_ListValue<KX_GameObject> *sumolist = new EXP_ListValue<KX_GameObject>();

  std::vector<BL_parentChildLink> vec_parent_child;

  EXP_ListValue<KX_GameObject> *objectlist = kxscene->GetObjectList();
  EXP_ListValue<KX_GameObject> *inactivelist = kxscene->GetInactiveList();
  EXP_ListValue<KX_GameObject> *parentlist = kxscene->GetRootParentList();

  SCA_LogicManager *logicmgr = kxscene->GetLogicManager();
  SCA_TimeEventManager *timemgr = kxscene->GetTimeEventManager();

  EXP_ListValue<KX_GameObject> *logicbrick_conversionlist = new EXP_ListValue<KX_GameObject>();

  if (!single_object) {
    // Convert actions to actionmap
    bAction *curAct;
    for (curAct = (bAction *)maggie->actions.first; curAct; curAct = (bAction *)curAct->id.next) {
      logicmgr->RegisterActionName(curAct->id.name + 2, curAct);
    }
  }
  else {
    LISTBASE_FOREACH (bActuator *, actu, &single_object->actuators) {
      if (actu->type == ACT_ACTION) {
        bActionActuator *actionActu = (bActionActuator *)actu->data;
        if (actionActu->act != nullptr) {
          if (!logicmgr->GetActionByName(actionActu->act->id.name + 2)) {
            logicmgr->RegisterActionName(actionActu->act->id.name + 2, (void *)actionActu->act);
          }
        }
      }
    }
  }

  /* Ensure objects base flags are up to date each time we call BL_ConvertObjects */
  BKE_scene_base_flag_to_objects(blenderscene, BKE_view_layer_default_view(blenderscene));

  std::vector<Object *> lod_objects = lod_level_object_list(
      BKE_view_layer_default_view(blenderscene));

  bool converting_during_runtime = single_object != nullptr;
  bool converting_instance_col_at_runtime = single_object && single_object->instance_collection && converter->FindGameObject(single_object) == nullptr;

  // Let's support scene set.
  // Beware of name conflict in linked data, it will not crash but will create confusion
  // in Python scripting and in certain actuators (replace mesh). Linked scene *should* have
  // no conflicting name for Object, Object data and Action.
  for (SETLOOPER(blenderscene, sce_iter, base)) {
    Object *blenderobject = base->object;

    if (converter->FindGameObject(blenderobject) != nullptr) {
      if (single_object && single_object == blenderobject) {
        CM_Warning("Attempt to convert the same Object several times: " << blenderobject->id.name + 2);
      }
      continue;
    }

    if (blenderobject == kxscene->GetGameDefaultCamera()) {
      continue;
    }

    if (single_object) {
      if (blenderobject != single_object) {
        continue;
      }
    }

    bool isInActiveLayer = (blenderobject->base_flag &
                            (BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT |
                             BASE_ENABLED_AND_VISIBLE_IN_DEFAULT_VIEWPORT)) != 0;
    blenderobject->lay = isInActiveLayer ? blenderscene->lay : 0;

    /* Force OB_RESTRICT_VIEWPORT to avoid not needed depsgraph operations in some cases,
     * unless blenderobject is a lodlevel because we want to be abled to get
     * evaluated meshes from lodlevels and restrict viewport prevents meshes to be evaluated
     */
    if (!isInActiveLayer && !is_lod_level(lod_objects, blenderobject)) {
      kxscene->BackupRestrictFlag(blenderobject, blenderobject->visibility_flag);
      blenderobject->visibility_flag |= OB_HIDE_VIEWPORT;
      BKE_main_collection_sync_remap(maggie);
      DEG_relations_tag_update(maggie);
    }

    BKE_view_layer_synced_ensure(blenderscene, BKE_view_layer_default_view(blenderscene));

    KX_GameObject *gameobj = BL_gameobject_from_blenderobject(
        blenderobject, kxscene, rendertools, converter, libloading, converting_during_runtime);

    if (gameobj && converting_during_runtime) {
      gameobj->SetIsReplicaObject();
    }

    if (gameobj) {
      /* macro calls object conversion funcs */
      BL_CONVERTBLENDEROBJECT_SINGLE;

      if (gameobj->IsDupliGroup()) {  // Don't bother with groups during single object conversion
        grouplist.insert(blenderobject->instance_collection);
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

  if (!grouplist.empty()) {  // always empty during single object conversion
    // now convert the group referenced by dupli group object
    // keep track of all groups already converted
    std::set<Collection *> allgrouplist = grouplist;
    std::set<Collection *> tempglist;
    // recurse
    while (!grouplist.empty()) {
      std::set<Collection *>::iterator git;
      tempglist.clear();
      tempglist.swap(grouplist);
      for (git = tempglist.begin(); git != tempglist.end(); git++) {
        Collection *group = *git;
        FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (group, blenderobject) {
          if (converter->FindGameObject(blenderobject) == nullptr) {
            groupobj.insert(blenderobject);
            KX_GameObject *gameobj = BL_gameobject_from_blenderobject(blenderobject,
                                                                      kxscene,
                                                                      rendertools,
                                                                      converter,
                                                                      libloading,
                                                                      converting_during_runtime);

            bool isInActiveLayer = false;
            if (gameobj) {
              /* Insert object to the constraint game object list
               * so we can check later if there is a instance in the scene or
               * an instance and its actual group definition. */
              convertedlist->Add((KX_GameObject *)gameobj->AddRef());

              /* macro calls object conversion funcs */
              BL_CONVERTBLENDEROBJECT_SINGLE;

              if (gameobj->IsDupliGroup()) {
                if (allgrouplist.insert(blenderobject->instance_collection).second) {
                  grouplist.insert(blenderobject->instance_collection);
                }
              }

              /* see comment above re: mem leaks */
              gameobj->Release();
            }
          }
        }
        FOREACH_COLLECTION_OBJECT_RECURSIVE_END;
      }
    }
  }

  // non-camera objects not supported as camera currently
  if (blenderscene->camera && blenderscene->camera->type == OB_CAMERA &&
      CTX_wm_region_view3d(KX_GetActiveEngine()->GetContext())->persp == RV3D_CAMOB) {
    KX_Camera *gamecamera = (KX_Camera *)converter->FindGameObject(blenderscene->camera);

    if (gamecamera && !single_object)
      kxscene->SetActiveCamera(gamecamera);
  }

  // create hierarchy information
  std::vector<BL_parentChildLink>::iterator pcit;

  for (pcit = vec_parent_child.begin(); !(pcit == vec_parent_child.end()); ++pcit) {

    if (single_object && !converting_instance_col_at_runtime) {
      /* Don't bother with object children during single object conversion */
      std::cout << "Warning: Object's children are not converted during runtime" << std::endl;
      break;
    }

    struct Object *blenderchild = pcit->m_blenderchild;
    struct Object *blenderparent = blenderchild->parent;
    KX_GameObject *parentobj = converter->FindGameObject(blenderparent);
    KX_GameObject *childobj = converter->FindGameObject(blenderchild);

    BLI_assert(childobj);

    if (!parentobj || objectlist->SearchValue(childobj) != objectlist->SearchValue(parentobj)) {
      // special case: the parent and child object are not in the same layer.
      // This weird situation is used in Apricot for test purposes.
      // Resolve it by not converting the child
      /* When this is happening, and it can happen more often in 0.3+ due to
       * active/inactive layers organisation from outliner which can be
       * a bit confusing, display a message to say which child is being removed /
       * will not be converted. */
      if (parentobj) {
        CM_Warning("Parent object "
                   << parentobj->GetName() << " and Child object " << childobj->GetName()
                   << " are not in the same layer (active / inactive objects lists).");
        CM_Warning("Child object " << childobj->GetName() << " will not be converted.");
        CM_Warning("Please ensure that parents and children are in the same layer.");
      }
      childobj->GetSGNode()->DisconnectFromParent();
      delete pcit->m_gamechildnode;
      // Now destroy the child object but also all its descendent that may already be linked
      // Remove the child reference in the local list!
      // Note: there may be descendents already if the children of the child were processed
      //       by this loop before the child. In that case, we must remove the children also
      std::vector<KX_GameObject *> childrenlist = childobj->GetChildrenRecursive();
      // The returned list by GetChildrenRecursive is not owned by anyone and must not own items,
      // so no AddRef().
      childrenlist.push_back(childobj);
      for (KX_GameObject *obj : childrenlist) {
        if (sumolist->RemoveValue(obj))
          obj->Release();
        if (logicbrick_conversionlist->RemoveValue(obj))
          obj->Release();
        if (convertedlist->RemoveValue(obj)) {
          obj->Release();
        }
      }

      // now destroy recursively
      converter->UnregisterGameObject(childobj);
      // removing objects during conversion make sure this runs too
      kxscene->RemoveObject(childobj);

      continue;
    }

    switch (blenderchild->partype) {
      case PARVERT1: {
        // creat a new vertex parent relationship for this node.
        KX_VertexParentRelation *vertex_parent_relation = new KX_VertexParentRelation();
        pcit->m_gamechildnode->SetParentRelation(vertex_parent_relation);
        break;
      }
      case PARSLOW: {
        // creat a new slow parent relationship for this node.
        KX_SlowParentRelation *slow_parent_relation = new KX_SlowParentRelation(blenderchild->sf);
        pcit->m_gamechildnode->SetParentRelation(slow_parent_relation);
        break;
      }
      case PARBONE: {
        // parent this to a bone
        Bone *parent_bone = BKE_armature_find_bone_name(
            BKE_armature_from_object(blenderchild->parent), blenderchild->parsubstr);

        if (parent_bone) {
          KX_BoneParentRelation *bone_parent_relation = new KX_BoneParentRelation(parent_bone);
          pcit->m_gamechildnode->SetParentRelation(bone_parent_relation);
        }

        break;
      }
      case PARSKEL:  // skinned - ignore
        break;
      case PAROBJECT:
      case PARVERT3:
      default:
        // unhandled
        break;
    }

    parentobj->GetSGNode()->AddChild(pcit->m_gamechildnode);
  }
  vec_parent_child.clear();

  // find 'root' parents (object that has not parents in SceneGraph)
  for (KX_GameObject *gameobj : sumolist) {
    if (single_object && !converting_instance_col_at_runtime) {
      if (gameobj->GetBlenderObject() != single_object) {
        continue;
      }
    }
    if (gameobj->GetSGNode()->GetSGParent() == 0) {
      parentlist->Add(CM_AddRef(gameobj));
      gameobj->NodeUpdateGS(0);
    }
  }

  if (!single_object) {
    if (blenderscene->world)
      kxscene->GetPhysicsEnvironment()->SetNumTimeSubSteps(blenderscene->gm.physubstep);
  }

  // Create physics information.
  for (unsigned short i = 0; i < 2; ++i) {
    const bool processCompoundChildren = (i == 1);
    for (KX_GameObject *gameobj : sumolist) {
      Object *blenderobject = gameobj->GetBlenderObject();

      if (single_object && !converting_instance_col_at_runtime) {
        if (blenderobject != single_object) {
          continue;
        }
      }

      int nummeshes = gameobj->GetMeshCount();
      RAS_MeshObject *meshobj = 0;
      if (nummeshes > 0) {
        meshobj = gameobj->GetMesh(0);
      }

      int layerMask = (groupobj.find(blenderobject) == groupobj.end()) ? activeLayerBitInfo : 0;
      BL_CreatePhysicsObjectNew(
          gameobj, blenderobject, meshobj, kxscene, layerMask, converter, processCompoundChildren);
    }
  }

  // create physics joints
  for (KX_GameObject *gameobj : sumolist) {
    PHY_IPhysicsEnvironment *physEnv = kxscene->GetPhysicsEnvironment();
    struct Object *blenderobject = gameobj->GetBlenderObject();
    if (single_object && !converting_instance_col_at_runtime) {
      if (blenderobject != single_object) {
        continue;
      }
    }
    ListBase *conlist = BL_GetActiveConstraint(blenderobject);
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

      if (gotar && (gotar->GetLayer() & activeLayerBitInfo) && gotar->GetPhysicsController() &&
          (gameobj->GetLayer() & activeLayerBitInfo) && gameobj->GetPhysicsController()) {
        physEnv->SetupObjectConstraints(gameobj, gotar, dat, false);
      }
    }
  }

  if (!single_object) {
    KX_SetActiveScene(kxscene);
  }

  // create object representations for obstacle simulation
  KX_ObstacleSimulation *obssimulation = kxscene->GetObstacleSimulation();
  if (obssimulation) {
    for (KX_GameObject *gameobj : objectlist) {
      struct Object *blenderobject = gameobj->GetBlenderObject();
      if (single_object && !converting_instance_col_at_runtime) {
        if (blenderobject != single_object) {
          continue;
        }
      }
      if (blenderobject->gameflag & OB_HASOBSTACLE) {
        obssimulation->AddObstacleForObj(gameobj);
      }
    }
  }

  // process navigation mesh objects
  for (KX_GameObject *gameobj : objectlist) {
    struct Object *blenderobject = gameobj->GetBlenderObject();
    if (single_object && !converting_instance_col_at_runtime) {
      if (blenderobject != single_object) {
        continue;
      }
    }
    if (blenderobject->type == OB_MESH && (blenderobject->gameflag & OB_NAVMESH)) {
      KX_NavMeshObject *navmesh = static_cast<KX_NavMeshObject *>(gameobj);
      navmesh->SetVisible(0, true);
      navmesh->BuildNavMesh();
      if (obssimulation)
        obssimulation->AddObstaclesForNavMesh(navmesh);
    }
  }
  for (KX_GameObject *gameobj : inactivelist) {
    struct Object *blenderobject = gameobj->GetBlenderObject();
    if (single_object && !converting_instance_col_at_runtime) {
      if (blenderobject != single_object) {
        continue;
      }
    }
    if (blenderobject->type == OB_MESH && (blenderobject->gameflag & OB_NAVMESH)) {
      KX_NavMeshObject *navmesh = static_cast<KX_NavMeshObject *>(gameobj);
      navmesh->SetVisible(0, true);
    }
  }

  // convert logic bricks, sensors, controllers and actuators
  for (KX_GameObject *gameobj : logicbrick_conversionlist) {
    struct Object *blenderobj = gameobj->GetBlenderObject();
    if (single_object && !converting_instance_col_at_runtime) {
      if (blenderobj != single_object) {
        continue;
      }
    }
    int layerMask = (groupobj.find(blenderobj) == groupobj.end()) ? activeLayerBitInfo : 0;
    bool isInActiveLayer = (blenderobj->lay & layerMask) != 0;
    BL_ConvertActuators(maggie->filepath,
                        blenderobj,
                        gameobj,
                        logicmgr,
                        kxscene,
                        ketsjiEngine,
                        layerMask,
                        isInActiveLayer,
                        converter);
  }
  for (KX_GameObject *gameobj : logicbrick_conversionlist) {
    struct Object *blenderobj = gameobj->GetBlenderObject();
    if (single_object && !converting_instance_col_at_runtime) {
      if (blenderobj != single_object) {
        continue;
      }
    }
    int layerMask = (groupobj.find(blenderobj) == groupobj.end()) ? activeLayerBitInfo : 0;
    bool isInActiveLayer = (blenderobj->lay & layerMask) != 0;
    BL_ConvertControllers(
        blenderobj, gameobj, logicmgr, layerMask, isInActiveLayer, converter, libloading);
  }
  for (KX_GameObject *gameobj : logicbrick_conversionlist) {
    struct Object *blenderobj = gameobj->GetBlenderObject();
    if (single_object && !converting_instance_col_at_runtime) {
      if (blenderobj != single_object) {
        continue;
      }
    }
    int layerMask = (groupobj.find(blenderobj) == groupobj.end()) ? activeLayerBitInfo : 0;
    bool isInActiveLayer = (blenderobj->lay & layerMask) != 0;
    BL_ConvertSensors(blenderobj,
                      gameobj,
                      logicmgr,
                      kxscene,
                      ketsjiEngine,
                      layerMask,
                      isInActiveLayer,
                      canvas,
                      converter);
    // set the init state to all objects
    gameobj->SetInitState((blenderobj->init_state) ? blenderobj->init_state : blenderobj->state);
  }
  // apply the initial state to controllers, only on the active objects as this registers the
  // sensors
  for (KX_GameObject *gameobj : objectlist) {
    if (single_object && !converting_instance_col_at_runtime) {
      if (gameobj->GetBlenderObject() != single_object) {
        continue;
      }
    }
    gameobj->ResetState();
  }

  // Convert the python components of each object.
  for (KX_GameObject *gameobj : sumolist) {
    Object *blenderobj = gameobj->GetBlenderObject();
    if (single_object && !converting_instance_col_at_runtime) {
      if (blenderobj != single_object) {
        continue;
      }
    }
    BL_ConvertComponentsObject(gameobj, blenderobj);
  }

  for (KX_GameObject *gameobj : objectlist) {
    Object *blenderobj = gameobj->GetBlenderObject();
    if (single_object && !converting_instance_col_at_runtime) {
      if (blenderobj != single_object) {
        continue;
      }
    }
    if (gameobj->GetPrototype() || gameobj->GetComponents()) {
      // Register object for component update.
      kxscene->GetPythonProxyManager().Register(gameobj);
    }
  }

  // cleanup converted set of group objects
  convertedlist->Release();
  sumolist->Release();
  logicbrick_conversionlist->Release();

  // Calculate the scene btree -
  // too slow - commented out.
  // kxscene->SetNodeTree(tf.MakeTree());

  // instantiate dupli group, we will loop trough the object
  // that are in active layers. Note that duplicating group
  // has the effect of adding objects at the end of objectlist.
  // Only loop through the first part of the list.
  if (!converting_instance_col_at_runtime) {
    int objcount = objectlist->GetCount();
    for (unsigned int i = 0; i < objcount; ++i) {
      KX_GameObject *gameobj = objectlist->GetValue(i);
      if (gameobj->IsDupliGroup()) {
        /* In 2.8+, hide blenderobjects->instance_collection,
         * they are not meant to be displayed, they only contain
         * instances which are meant to be displayed */
        /* BTW, i'm wondering if adding logic bricks on instance_collections
         * can lead to a crash */
        gameobj->SetVisible(false, false);

        /* Don't bother with groups during single object conversion */
        if (!single_object) {
          kxscene->DupliGroupRecurse(gameobj, 0);
        }
      }
    }
  }
  else {
    /* If we are converting instance collections during runtime only */
    if (single_object) {
      /* If we are converting an instance collection at runtime, don't loop through
       * all objects in active layer to avoid creating again previously created
       * dupligroups */
      KX_GameObject *gameobj = converter->FindGameObject(single_object);
      /* If instance collection is in an Active layer */
      if (gameobj->GetLayer() == 1) {
        gameobj->SetVisible(false, false);
        kxscene->DupliGroupRecurse(gameobj, 0);
      }
    }
  }
}
