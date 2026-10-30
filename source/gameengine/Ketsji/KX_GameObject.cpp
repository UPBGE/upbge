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
 * Game object wrapper
 */

/** \file gameengine/Ketsji/KX_GameObject.cpp
 *  \ingroup ketsji
 */

#ifdef _MSC_VER
/* This warning tells us about truncation of __long__ stl-generated names.
 * It can occasionally cause DevStudio to have internal compiler warnings. */
#  pragma warning(disable : 4786)
#endif

#include "KX_GameObject.h"

#include "EXP_ArrayValue.h"
#include "EXP_BoolValue.h"
#include "EXP_DictValue.h"
#include "EXP_FloatValue.h"
#include "EXP_IntValue.h"
#include "EXP_StringValue.h"
#include "EXP_Vector2Value.h"
#include "EXP_Vector3Value.h"
#include "EXP_Vector4Value.h"
#include "KX_ObjectReferenceValue.h"

#include "BKE_context.hh"
#include "BKE_game_persistence.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_material.hh"
#include "BKE_mball.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BLI_hash.h"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_anim_types.h"
#include "DNA_key_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_force_types.h"
#include "DNA_scene_types.h"
#include "WM_api.hh"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

#include "BL_Action.h"
#include "BL_ActionManager.h"
#include "BL_ArmatureObject.h"
#include "BL_SceneConverter.h"
#include "KX_BoneParentNodeRelationship.h"
#include "KX_ClientObjectInfo.h"
#include "KX_CollisionContactPoints.h"
#include "KX_Globals.h"
#include "KX_LodLevel.h"
#include "KX_LodManager.h"
#include "KX_MeshProxy.h"
#include "KX_NetworkMessageScene.h"  //Needed for sendMessage()
#include "KX_NodeRelationships.h"
#include "KX_PhysicsEngineEnums.h"
#include "KX_PolyProxy.h"
#include "KX_PyMath.h"
#include "KX_PythonComponent.h"
#include "KX_RayCast.h"
#include "MT_Quaternion.h"
#include "SCA_ISensor.h"
#include "SG_Controller.h"

#ifdef WITH_PYTHON
#  include "EXP_PythonCallBack.h"
#  include "bpy_rna.hh"
#  include "python_utildefines.hh"
#endif

using namespace blender;

static MT_Vector3 dummy_point = MT_Vector3(0.0f, 0.0f, 0.0f);
static PHY_RigidBodyConstraintSettings RigidBodyConstraintSettingsFromBlender(
    const blender::RigidBodyCon &rbc)
{
  PHY_RigidBodyConstraintSettings settings;
  settings.type = static_cast<PHY_RigidBodyConstraintType>(rbc.type);
  settings.spring_type = static_cast<PHY_RigidBodyConstraintSpringType>(rbc.spring_type);
  settings.flags = 0;
  if (rbc.flag & RBC_FLAG_ENABLED) {
    settings.flags |= PHY_RB_CONSTRAINT_ENABLED;
  }
  if (rbc.flag & RBC_FLAG_DISABLE_COLLISIONS) {
    settings.flags |= PHY_RB_CONSTRAINT_DISABLE_COLLISIONS;
  }
  if (rbc.flag & RBC_FLAG_USE_BREAKING) {
    settings.flags |= PHY_RB_CONSTRAINT_USE_BREAKING;
  }
  if (rbc.flag & RBC_FLAG_OVERRIDE_SOLVER_ITERATIONS) {
    settings.flags |= PHY_RB_CONSTRAINT_OVERRIDE_SOLVER_ITERATIONS;
  }
  if (rbc.flag & RBC_FLAG_JOLT_OVERRIDE_SOLVER_ITERATIONS) {
    settings.flags |= PHY_RB_CONSTRAINT_JOLT_OVERRIDE_SOLVER_ITERATIONS;
  }
  if (rbc.flag & RBC_FLAG_USE_LIMIT_LIN_X) {
    settings.flags |= PHY_RB_CONSTRAINT_USE_LIMIT_LIN_X;
  }
  if (rbc.flag & RBC_FLAG_USE_LIMIT_LIN_Y) {
    settings.flags |= PHY_RB_CONSTRAINT_USE_LIMIT_LIN_Y;
  }
  if (rbc.flag & RBC_FLAG_USE_LIMIT_LIN_Z) {
    settings.flags |= PHY_RB_CONSTRAINT_USE_LIMIT_LIN_Z;
  }
  if (rbc.flag & RBC_FLAG_USE_LIMIT_ANG_X) {
    settings.flags |= PHY_RB_CONSTRAINT_USE_LIMIT_ANG_X;
  }
  if (rbc.flag & RBC_FLAG_USE_LIMIT_ANG_Y) {
    settings.flags |= PHY_RB_CONSTRAINT_USE_LIMIT_ANG_Y;
  }
  if (rbc.flag & RBC_FLAG_USE_LIMIT_ANG_Z) {
    settings.flags |= PHY_RB_CONSTRAINT_USE_LIMIT_ANG_Z;
  }
  if (rbc.flag & RBC_FLAG_USE_SPRING_X) {
    settings.flags |= PHY_RB_CONSTRAINT_USE_SPRING_X;
  }
  if (rbc.flag & RBC_FLAG_USE_SPRING_Y) {
    settings.flags |= PHY_RB_CONSTRAINT_USE_SPRING_Y;
  }
  if (rbc.flag & RBC_FLAG_USE_SPRING_Z) {
    settings.flags |= PHY_RB_CONSTRAINT_USE_SPRING_Z;
  }
  if (rbc.flag & RBC_FLAG_USE_SPRING_ANG_X) {
    settings.flags |= PHY_RB_CONSTRAINT_USE_SPRING_ANG_X;
  }
  if (rbc.flag & RBC_FLAG_USE_SPRING_ANG_Y) {
    settings.flags |= PHY_RB_CONSTRAINT_USE_SPRING_ANG_Y;
  }
  if (rbc.flag & RBC_FLAG_USE_SPRING_ANG_Z) {
    settings.flags |= PHY_RB_CONSTRAINT_USE_SPRING_ANG_Z;
  }
  if (rbc.flag & RBC_FLAG_USE_MOTOR_LIN) {
    settings.flags |= PHY_RB_CONSTRAINT_USE_MOTOR_LIN;
  }
  if (rbc.flag & RBC_FLAG_USE_MOTOR_ANG) {
    settings.flags |= PHY_RB_CONSTRAINT_USE_MOTOR_ANG;
  }

  settings.num_solver_iterations = rbc.num_solver_iterations;
  settings.jolt_velocity_solver_iterations = rbc.jolt_velocity_solver_iterations;
  settings.jolt_position_solver_iterations = rbc.jolt_position_solver_iterations;
  settings.breaking_threshold = rbc.breaking_threshold;
  settings.limit_lin_x_lower = rbc.limit_lin_x_lower;
  settings.limit_lin_x_upper = rbc.limit_lin_x_upper;
  settings.limit_lin_y_lower = rbc.limit_lin_y_lower;
  settings.limit_lin_y_upper = rbc.limit_lin_y_upper;
  settings.limit_lin_z_lower = rbc.limit_lin_z_lower;
  settings.limit_lin_z_upper = rbc.limit_lin_z_upper;
  settings.limit_ang_x_lower = rbc.limit_ang_x_lower;
  settings.limit_ang_x_upper = rbc.limit_ang_x_upper;
  settings.limit_ang_y_lower = rbc.limit_ang_y_lower;
  settings.limit_ang_y_upper = rbc.limit_ang_y_upper;
  settings.limit_ang_z_lower = rbc.limit_ang_z_lower;
  settings.limit_ang_z_upper = rbc.limit_ang_z_upper;
  settings.spring_stiffness_x = rbc.spring_stiffness_x;
  settings.spring_stiffness_y = rbc.spring_stiffness_y;
  settings.spring_stiffness_z = rbc.spring_stiffness_z;
  settings.spring_stiffness_ang_x = rbc.spring_stiffness_ang_x;
  settings.spring_stiffness_ang_y = rbc.spring_stiffness_ang_y;
  settings.spring_stiffness_ang_z = rbc.spring_stiffness_ang_z;
  settings.spring_damping_x = rbc.spring_damping_x;
  settings.spring_damping_y = rbc.spring_damping_y;
  settings.spring_damping_z = rbc.spring_damping_z;
  settings.spring_damping_ang_x = rbc.spring_damping_ang_x;
  settings.spring_damping_ang_y = rbc.spring_damping_ang_y;
  settings.spring_damping_ang_z = rbc.spring_damping_ang_z;
  settings.motor_lin_target_velocity = rbc.motor_lin_target_velocity;
  settings.motor_ang_target_velocity = rbc.motor_ang_target_velocity;
  settings.motor_lin_max_impulse = rbc.motor_lin_max_impulse;
  settings.motor_ang_max_impulse = rbc.motor_ang_max_impulse;
  return settings;
}

static MT_Vector3 dummy_scaling = MT_Vector3(1.0f, 1.0f, 1.0f);
static MT_Matrix3x3 dummy_orientation = MT_Matrix3x3(
    1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);

KX_GameObject::ActivityCullingInfo::ActivityCullingInfo()
    : m_flags(ACTIVITY_NONE), m_physicsRadius(0.0f), m_logicRadius(0.0f)
{
}

KX_GameObject::KX_GameObject()
    : SCA_IObject(),
      m_isReplica(false),             // eevee
      m_forceIgnoreParentTx(false),   // eevee
      m_previousLodLevel(-1),         // eevee
      m_isUpbgeDupliInstance(false),  // eevee
      m_cachedRenderPosition(0.0f, 0.0f, 0.0f),
      m_cachedRenderOrientation(MT_Matrix3x3::Identity()),
      m_cachedRenderScale(1.0f, 1.0f, 1.0f),
      m_cachedInterpolatedTransform(MT_Transform::Identity()),
      m_useRenderInterpolation(false),
      m_interpolationListRegistered(false),
      m_renderSyncedMatrix{},
      m_renderSyncedValid(false),
      m_renderSyncedFrame(0),
      m_evaluatedPoseDue(false),
      m_smoothMotion(OB_SMOOTH_MOTION_SCENE),
      m_smoothResetPending(false),
      m_smoothResetBlend(0.0f),
      m_smoothCorrectionOffset(0.0f, 0.0f, 0.0f),
      m_smoothCorrectionTurn(0.0f, 0.0f, 0.0f, 1.0f),
      m_smoothCorrectionLeft(0.0f),
      m_smoothCorrectionDuration(0.0f),
      m_smoothDrawnPosition(0.0f, 0.0f, 0.0f),
      m_smoothDrawnOrientation(MT_Matrix3x3::Identity()),
      m_smoothDrawnFrame(0),
      m_persistenceSourceUuid(blender::BLI_uuid_nil()),
      m_persistenceInstanceUuid(blender::BLI_uuid_nil()),
      m_saveGameObjectState(false),
      m_parentCompoundIntent(false),
      m_parentGhostIntent(false),
      m_parentPhysicsManaged(false),
      m_layer(0),
      m_lodManager(nullptr),
      m_currentLodLevel(0),
      m_pBlenderObject(nullptr),
      m_pBlenderGroupObject(nullptr),
      m_bIsNegativeScaling(false),
      m_objectColor(1.0f, 1.0f, 1.0f, 1.0f),
      m_bVisible(true),
      m_overlayVisible(false),
      m_collectionVisible(true),
      m_presentationVisible(true),
      m_viewClipVisible(true),
      m_uiInteractableOverride(-1),
      m_bOccluder(false),
      m_pPhysicsController(nullptr),
      m_pSGNode(nullptr),
      m_pInstanceObjects(nullptr),
      m_pDupliGroupObject(nullptr),
      m_actionManager(nullptr)
#ifdef WITH_PYTHON
      ,
      m_components(NULL),
      m_attr_dict(nullptr),
      m_collisionCallbacks(nullptr),
      m_removeCallbacks(nullptr)
#endif
{
  m_pClient_info = new KX_ClientObjectInfo(this, KX_ClientObjectInfo::ACTOR);
};

KX_GameObject::~KX_GameObject()
{
  ClearReferenceValues();
  if (m_drawCull.distance_value != nullptr) {
    m_drawCull.distance_value->Release();
    m_drawCull.distance_value = nullptr;
  }
#ifdef WITH_PYTHON

  /* onRemove callbacks */
  RunOnRemoveCallbacks();
  Py_CLEAR(m_removeCallbacks);

  if (m_attr_dict) {
    PyDict_Clear(m_attr_dict); /* in case of circular refs or other weird cases */
    /* Py_CLEAR: Py_DECREF's and nullptr's */
    Py_CLEAR(m_attr_dict);
  }
  // Unregister collision callbacks
  // Do this before we start freeing physics information like m_pClient_info
  if (m_collisionCallbacks) {
    UnregisterCollisionCallbacks();
    Py_CLEAR(m_collisionCallbacks);
  }

  if (m_components) {
    m_components->Release();
  }
#endif  // WITH_PYTHON

  /* EEVEE INTEGRATION */

  blender::Object *ob = GetBlenderObject();

  if (m_replaceMeshEval && ob) {
    /* The scene frees its mesh copies at exit: stop drawing one and evaluate the own mesh. */
    blender::bContext *C = KX_GetActiveEngine() ? KX_GetActiveEngine()->GetContext() : nullptr;
    blender::Depsgraph *depsgraph = C ? CTX_data_depsgraph_pointer(C) : nullptr;
    blender::Object *ob_eval = depsgraph ? DEG_get_evaluated(depsgraph, ob) : nullptr;
    if (ob_eval && ob_eval != ob && ob_eval->runtime->data_eval == &m_replaceMeshEval->id) {
      BKE_object_free_derived_caches(ob_eval);
      DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
    }
    m_replaceMeshEval = nullptr;
  }

  if (m_pSGNode) {
    /* Discard rendered blender::Object */
    DiscardRenderedObject();

    /* At KX_Scene exit */
    KX_Scene *scene = GetScene();
    if (!scene->m_isRuntime) {
      if (ob && ob->type == OB_MBALL) {
        DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
      }
    }

    scene->GetBlenderSceneConverter()->UnregisterGameObject(this);
  }

  /* END OF EEVEE INTEGRATION */

  RemoveMeshes();

  // is this delete somewhere ?
  // if (m_sumoObj)
  //	delete m_sumoObj;
  delete m_pClient_info;

  // Remove rigid body constraints from physics environment while GetScene() still works
  // Must be called before m_pSGNode is invalidated
  if (m_pSGNode) {
    RemoveRigidBodyConstraints();
  }

  // if (m_pSGNode)
  //	delete m_pSGNode;
  if (m_pSGNode) {
    // must go through controllers and make sure they will not use us anymore
    // This is important for KX_BulletPhysicsControllers that unregister themselves
    // from the object when they are deleted.
    SGControllerList::iterator contit;
    SGControllerList &controllers = m_pSGNode->GetSGControllerList();
    for (contit = controllers.begin(); contit != controllers.end(); ++contit) {
      (*contit)->ClearNode();
    }
    m_pSGNode->SetSGClientObject(nullptr);

    /* m_pSGNode is freed in KX_Scene::RemoveNodeDestructObject */
  }

  if (m_pPhysicsController) {
    delete m_pPhysicsController;
  }

  if (m_actionManager) {
    delete m_actionManager;
  }

  if (m_pDupliGroupObject) {
    m_pDupliGroupObject->Release();
  }

  if (m_pInstanceObjects) {
    m_pInstanceObjects->Release();
  }
  if (m_lodManager) {
    m_lodManager->Release();
    GetScene()->RemoveObjFromLodObjList(this);
  }
}

/************************EEVEE_INTEGRATION**********************/
void KX_GameObject::SetBlenderObject(blender::Object *obj)
{
  m_pBlenderObject = obj;
}

void KX_GameObject::SyncTransformWithDepsgraph()
{
  blender::Object *ob = GetBlenderObject();
  if (ob) {
    float loc[3], rot[3][3], size[3];
    mat4_to_loc_rot_size(loc, rot, size, ob->object_to_world().ptr());
    MT_Matrix3x3 orientation;
    NodeSetWorldPosition(MT_Vector3(loc));
    // MT_Matrix3x3's constructor expects a 4x4 matrix
    orientation = MT_Matrix3x3();
    orientation.setValue3x3(*rot);
    NodeSetGlobalOrientation(orientation);
    NodeSetWorldScale(MT_Vector3(size));
  }
}

void KX_GameObject::ForceIgnoreParentTx()
{
  m_forceIgnoreParentTx = true;
}

/** Blender computes the evaluated pose from other things than the original's own transform. */
static bool pose_computed_by_blender(const blender::Object *ob)
{
  return (ob->transflag & OB_TRANSFLAG_OVERRIDE_GAME_PRIORITY) || ob->parent != nullptr ||
         !BLI_listbase_is_empty(&ob->constraints) ||
         (ob->adt != nullptr && !BLI_listbase_is_empty(&ob->adt->drivers));
}

bool KX_BlenderSimulatesObject(const blender::Object *ob)
{
  if (ELEM(ob->type, OB_ARMATURE, OB_MBALL) || ob->instance_collection != nullptr ||
      (ob->transflag & OB_DUPLI) || ob->rigidbody_object != nullptr ||
      ob->rigidbody_constraint != nullptr || !BLI_listbase_is_empty(&ob->particlesystem) ||
      (ob->pd != nullptr && ob->pd->forcefield != 0))
  {
    return true;
  }
  for (const blender::ModifierData *md = static_cast<const blender::ModifierData *>(
           ob->modifiers.first);
       md != nullptr;
       md = md->next)
  {
    if (ELEM(md->type,
             eModifierType_Collision,
             eModifierType_Cloth,
             eModifierType_Softbody,
             eModifierType_Fluid,
             eModifierType_DynamicPaint,
             eModifierType_ParticleSystem,
             eModifierType_Surface))
    {
      return true;
    }
  }
  return false;
}

/** Nothing of Blender's reads the object's position (other than its own geometry, which the
 * first moves find out): its moves can skip Blender's evaluation. */
static bool pose_can_skip_evaluation(const blender::Depsgraph *depsgraph,
                                     const blender::Object *ob)
{
  if (KX_BlenderSimulatesObject(ob)) {
    return false;
  }
  /* Another object, a modifier's or geometry nodes' object, a material, a child... */
  bool read_elsewhere = false;
  DEG_foreach_dependent_ID_component(
      depsgraph,
      &ob->id,
      DEG_OB_COMP_TRANSFORM,
      DEG_FOREACH_COMPONENT_IGNORE_TRANSFORM_SOLVERS,
      [&](blender::ID * /*id*/, blender::eDepsObjectComponentType /*component*/) {
        read_elsewhere = true;
      });
  return !read_elsewhere;
}

bool KX_GameObject::UsesDirectPose(const RenderSyncPass &pass, blender::Object *ob_orig)
{
  /* A placed Dupli Base is never marked (upstream UPBGE): its creator chose it. */
  if (m_isUpbgeDupliBase) {
    return true;
  }
  if (m_posePath != PosePath::Unknown) {
    return m_posePath == PosePath::Direct;
  }
  if (m_poseFromBlender || !pose_can_skip_evaluation(pass.depsgraph, ob_orig)) {
    m_posePath = PosePath::Tagged;
    return false;
  }
  /* Marked once more, to see whether Blender evaluates the object's geometry with its transform
   * (its modifiers or geometry nodes read its position): TagForTransformUpdateEvaluated decides. */
  m_posePathProbeStart = pass.start_update;
  return false;
}

void KX_GameObject::NotePoseCheck(const RenderSyncPass &pass)
{
  if (m_poseCheckPass != pass.pass) {
    m_poseCheckPass = pass.pass;
    GetScene()->NotePoseCheck(this);
  }
}

void KX_GameObject::TagForTransformUpdate(const RenderSyncPass &pass, bool is_overlay_pass)
{
  blender::Object *ob_orig = GetBlenderObject();

  if (m_posePathRelations != pass.relations) {
    /* Blender's relations changed since the path was chosen (a spawn, a removal, a visibility
     * change): something may read this object's position now, or no longer. */
    m_posePathRelations = pass.relations;
    m_posePath = PosePath::Unknown;
    m_posePathProbes = 0;
    m_posePathProbeStart = 0;
    m_poseFromBlender = !m_isUpbgeDupliInstance && pose_computed_by_blender(ob_orig);
  }
  if (m_poseFromBlender) {
    NotePoseCheck(pass);
  }

  bool skip_transform = !m_isUpbgeDupliInstance &&
                        (ob_orig->transflag & OB_TRANSFLAG_OVERRIDE_GAME_PRIORITY);
  /* Don't tag non overlay collection objects in overlay collection render pass */
  skip_transform = skip_transform ||
                   ((ob_orig->gameflag & OB_OVERLAY_COLLECTION) == 0 && is_overlay_pass);
  /* Don't tag overlay collection objects in non overlay collection render pass */
  skip_transform = skip_transform ||
                   (ob_orig->gameflag & OB_OVERLAY_COLLECTION && !is_overlay_pass);

  /* Most objects are not drawn by a pass (the world's in the menu/HUD pass, the menu's in the
   * others): they cost nothing more. */
  if (skip_transform) {
    m_forceIgnoreParentTx = false;
    return;
  }

  /* Nothing moves between the camera passes of a frame (split views, minimaps, a HUD): the first
   * pass that draws the object compared it, and Blender evaluated it there if it changed. */
  if (m_renderSyncedFrame == pass.frame) {
    return;
  }
  m_renderSyncedFrame = pass.frame;

  /* Nothing wrote the node's world transform since the last compare: the same pose, as the
   * compare would find (it built and read every still object's matrix at every frame). */
  const bool use_interpolated = KX_GetActiveEngine()->UseViewportRender() &&
                                m_useRenderInterpolation;
  if (m_renderSyncedValid && !m_forceIgnoreParentTx && !use_interpolated &&
      m_renderSyncedNode == m_pSGNode && m_pSGNode->GetWorldStamp() == m_renderSyncedStamp)
  {
    return;
  }
  m_renderSyncedNode = m_pSGNode;
  m_renderSyncedStamp = m_pSGNode->GetWorldStamp();

  MT_Transform syncTransform = NodeGetWorldTransform();

  if (use_interpolated) {
    syncTransform = m_cachedInterpolatedTransform;
  }

  float object_to_world[4][4];
  syncTransform.getValue(&object_to_world[0][0]);

  /* Blender is told of a pose only when it differs from the last one it got, once: its
   * evaluation stays for the frame's later passes, and EEVEE compares every object's update stamp
   * with the one it drew last, in each camera's renderer. A node being marked changed is not
   * enough (physics, Smooth Motion and logic write the same pose again every frame), and every
   * update makes Blender wake its worker threads and restart the renderers' anti-aliasing. */
  const bool staticObject = m_renderSyncedValid &&
                            equals_m4m4(m_renderSyncedMatrix, object_to_world);
  if (!staticObject) {
    copy_m4_m4(m_renderSyncedMatrix, object_to_world);
    m_renderSyncedValid = true;
  }

  if (m_isUpbgeDupliInstance) {
    if (!staticObject) {
      TagUpbgeDupliInstanceForTaaReset();
    }
    return;
  }
  /* Merged still scenery that moves leaves its batch: its own Blender object is drawn again. */
  if (!staticObject && m_mergedBatch >= 0) {
    LeaveMergedBatch();
  }

  /* An unchanged pose needs nothing: Blender's objects hold it since it last changed. (Writing
   * the original object again each pass cost more than the rest of the sync in a big level.) */
  if (staticObject && !m_forceIgnoreParentTx) {
    return;
  }

  const bool applyTransformToOrig = GetScene()->OrigObCanBeTransformedInRealtime(ob_orig);

  if (applyTransformToOrig && !staticObject) {
    /* Written on every path: Blender evaluates the object from it whenever it rebuilds. */
    copy_m4_m4(ob_orig->runtime->object_to_world.ptr(), object_to_world);
    BKE_object_apply_mat4(ob_orig,
                          ob_orig->object_to_world().ptr(),
                          false,
                          ob_orig->parent && ob_orig->partype != PARVERT1);
    if (UsesDirectPose(pass, ob_orig)) {
      GetScene()->NoteDirectPose(this);
      m_forceIgnoreParentTx = false;
      return;
    }
  }
  if (!staticObject) {
    m_evaluatedPoseDue = true;
    NotePoseCheck(pass);
  }
  if (!applyTransformToOrig && !staticObject) {
    /* Blender never gets this object's pose (a fluid simulation would start over), only the
     * evaluated copy does (TagForTransformUpdateEvaluated). Its shading stamp is the cheapest
     * update the renderers notice (nothing evaluated again): EEVEE draws its shadow anew.
     * DEG_bump_update_count would not do: its own counter, far below the evaluations' stamps,
     * makes EEVEE take every object as changed and redraw every shadow at each frame. */
    DEG_id_tag_update(&ob_orig->id, ID_RECALC_SHADING);
  }

  if (!staticObject || m_forceIgnoreParentTx) {
    std::vector<KX_GameObject *> children = GetChildren();
    if (children.size() > 0) {
      std::vector<blender::Object *> childrenObjects;
      for (KX_GameObject *go : children) {
        blender::Object *child = go->GetBlenderObject();
        if (child) {
          childrenObjects.push_back(child);
        }
      }
      GetScene()->IgnoreParentTxBGE(
          pass.bmain, pass.depsgraph, pass.scene, ob_orig, childrenObjects);
    }
  }

  if (applyTransformToOrig) {
    /* NORMAL CASE */
    if (!staticObject && ob_orig->type != OB_MBALL) {
      DEG_id_tag_update(&ob_orig->id, ID_RECALC_TRANSFORM);
    }
    /* SPECIAL CASE: EXPERIMENTAL -> TEST METABALLS (incomplete) (TODO restore elems position at
     * ge exit) */
    else if (!staticObject && ob_orig->type == OB_MBALL) {
      if (!BKE_mball_is_basis(ob_orig)) {
        DEG_id_tag_update(&ob_orig->id, ID_RECALC_GEOMETRY);
      }
      else {
        DEG_id_tag_update(&ob_orig->id, ID_RECALC_TRANSFORM);
      }
    }
  }

  m_forceIgnoreParentTx = false;
}

void KX_GameObject::TagForTransformUpdateEvaluated(blender::Depsgraph *depsgraph,
                                                   const bool pass_evaluated,
                                                   const uint64_t pass_start_update)
{
  if (m_isUpbgeDupliInstance) {
    return;
  }
  blender::Object *ob_orig = GetBlenderObject();
  if (ob_orig == nullptr) {
    return;
  }

  if (ob_orig->transflag & OB_TRANSFLAG_OVERRIDE_GAME_PRIORITY) {
    SyncTransformWithDepsgraph();
    return;
  }

  if (!m_evaluatedPoseDue && !pass_evaluated) {
    return;
  }
  /* The drawn pose goes straight into the object Blender draws. Marking every object changed
   * here re-evaluated all of them on every camera pass (and woke Blender's worker threads each
   * time): TagForTransformUpdate marks the objects whose drawn pose changed. An object Blender
   * did not evaluate keeps the pose written into it before. */
  blender::Object *ob_eval = DEG_get_evaluated(depsgraph, ob_orig);
  if (ob_eval == nullptr) {
    return;
  }
  /* Blender's update counts only grow (one global counter), and its evaluation stamps the count
   * of the update it ran in (two when a Python handler asked for a second round). */
  const bool evaluated = ob_eval == ob_orig ||
                         ob_eval->runtime->last_update_transform > pass_start_update;
  if (m_posePathProbeStart != 0 && ob_eval != ob_orig) {
    /* The move marked to find the path: Blender evaluated the transform, and the geometry too if
     * it reads the position. Geometry evaluated for another reason in the same update (the
     * game's first frames) looks the same, so three such moves are needed to stay marked. */
    if (ob_eval->runtime->last_update_transform > m_posePathProbeStart) {
      if (ob_eval->runtime->last_update_geometry <= m_posePathProbeStart) {
        m_posePath = PosePath::Direct;
      }
      else if (++m_posePathProbes >= 3) {
        m_posePath = PosePath::Tagged;
      }
    }
    m_posePathProbeStart = 0;
  }
  if (!m_evaluatedPoseDue && !evaluated) {
    return;
  }
  m_evaluatedPoseDue = false;

  float object_to_world[4][4];
  NodeGetWorldTransform().getValue(&object_to_world[0][0]);
  BKE_object_apply_mat4(ob_eval, object_to_world, false, true);
  copy_m4_m4(ob_eval->runtime->object_to_world.ptr(), object_to_world);
}

void KX_GameObject::WriteDirectPose(blender::Depsgraph *depsgraph)
{
  blender::Object *ob_orig = GetBlenderObject();
  blender::Object *ob_eval = ob_orig != nullptr ? DEG_get_evaluated(depsgraph, ob_orig) : nullptr;
  /* Not in Blender's graph (hidden): Blender evaluates the written original when it comes back. */
  if (ob_eval == nullptr || ob_eval == ob_orig) {
    return;
  }
  copy_m4_m4(ob_eval->runtime->object_to_world.ptr(), m_renderSyncedMatrix);
  /* Blender's own last transform step: the inverse, the negative-scale flag and the transform
   * stamp, newer than every renderer's last draw thanks to the pass's mark. */
  BKE_object_eval_transform_final(depsgraph, ob_eval);
}

void KX_GameObject::SetIsUpbgeDupliBase()
{
  m_isUpbgeDupliBase = true;
}

static bool is_realtime_compositor_enabled(blender::bContext *C)
{
  bool is_realtime_compositor_enabled = false;
  blender::View3D *v3d = CTX_wm_view3d(C);
  blender::RegionView3D *rv3d = CTX_wm_region_view3d(C);
  blender::Scene *scene = CTX_data_scene(C);
  if (v3d && scene) {
    if (v3d->shading.use_compositor != V3D_SHADING_USE_COMPOSITOR_DISABLED &&
        v3d->shading.type >= OB_MATERIAL && scene->compositing_node_group && rv3d &&
        !(v3d->shading.use_compositor == V3D_SHADING_USE_COMPOSITOR_CAMERA &&
          rv3d->persp != RV3D_CAMOB))
    {
      is_realtime_compositor_enabled = true;
    }
  }
  return is_realtime_compositor_enabled;
}

void KX_GameObject::ReplicateBlenderObject()
{
  blender::Object *ob = GetBlenderObject();

  if (ob && ob->gameflag & OB_DUPLI_UPBGE) {
    /* Don't do a new copy of the blender::Object if the gameobj is a dupli Base or a dupli
     * instances All dupli instances will point to the same blender::Object (m_pBlenderObject) */
    return;
  }

  if (ob) {
    m_replicaSourceUid = ob->id.session_uid;
    m_replicaSourceData = static_cast<blender::ID *>(ob->data);
    m_replicaSourceModifiers = BLI_listbase_count(&ob->modifiers);
    m_blenderObjectChanged = false;
    m_blenderObjectReusable = GetScene()->CopyReuseActive();
    /* A removed copy of the same source kept in Blender's graph: no graph rebuild (2 copies a
     * tick among 10,000 objects rebuilt it at every frame, 226 ms). */
    blender::Object *parked = m_blenderObjectReusable ? GetScene()->TakeParkedCopy(ob) : nullptr;
    if (parked != nullptr) {
      BKE_game_persistence_identity_copy(ob->id, parked->id);
      if (!compare_v4v4(parked->color, ob->color, 0.0f)) {
        copy_v4_v4(parked->color, ob->color);
        DEG_id_tag_update(&parked->id, ID_RECALC_SHADING | ID_RECALC_TRANSFORM);
      }
      if (m_collectionSource == nullptr) {
        m_collectionSource = ob;
      }
      m_pBlenderObject = parked;
      m_isReplica = true;
      return;
    }
    blender::bContext *C = KX_GetActiveEngine()->GetContext();
    blender::Main *bmain = CTX_data_main(C);
    blender::Object *newob;
    BKE_id_copy_ex(bmain, &ob->id, (blender::ID **)&newob, 0);
    /* This is a temporary render copy, not a distinct authored identity. */
    BKE_game_persistence_identity_copy(ob->id, newob->id);
    id_us_min(&newob->id);
    blender::Scene *scene = GetScene()->GetBlenderScene();
    /* The copy is drawn from the scene's master collection, which is always in the view layer.
     * It used to be linked where the view layer's first camera is (not the active camera): a
     * menu camera in a collection disabled in viewports left every Add Object and Drop Item copy
     * undrawn. Its source's collections still decide its membership (CollectionsOf). */
    BKE_collection_object_add(bmain, scene->master_collection, newob);

    /* Avoid to make instance_collections "containers" visibled
     * when replicating as we want only the instances created in DupliGroupRecuse
     * to be visibled */
    if (!ob->instance_collection) {
      newob->base_flag |= (BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT |
                           BASE_ENABLED_AND_VISIBLE_IN_DEFAULT_VIEWPORT);
      newob->visibility_flag &= ~OB_HIDE_VIEWPORT;
    }

    /* This will call BKE_main_collection_sync_remap at frame end. */
    GetScene()->TagForCollectionRemap();

    /* Attempt to fix missing notifier in special cases (realtime compositor when overlay pass)
     * See: https://github.com/UPBGE/upbge/issues/1818 - Would maybe need more investigations
     * for a better fix... */
    if (GetScene()->GetOverlayCamera()) {
      if (is_realtime_compositor_enabled(C)) {
        GetScene()->AppendToIdsToUpdate(&scene->id, ID_RECALC_EDITORS, true);
      }
    }

    GetScene()->TagForRelationsUpdate();

    /* Linked to the master collection, the copy stays in its source's collections for a camera's
     * Collection setting and the other collection queries (a copy of a copy keeps the first
     * source). */
    if (m_collectionSource == nullptr) {
      m_collectionSource = ob;
    }
    m_pBlenderObject = newob;
    m_isReplica = true;
  }
}
void KX_GameObject::DiscardRenderedObject()
{
  blender::Object *ob = GetBlenderObject();
  if (ob) {
    PHY_IPhysicsController *ctrl = GetPhysicsController();
    if (ctrl) {
      ctrl->RemoveSoftBodyModifier(ob);
    }
    if (m_drawHidden && GetScene()) {
      GetScene()->RemoveDrawHidden(this);
    }
    /* 1. Rendered blender::Object is a dupli instance, just erase it from duplis list */
    if (m_isUpbgeDupliInstance) {
      if (KX_Scene *scene = GetScene()) {
        TagUpbgeDupliInstanceForTaaReset();
        scene->RemoveUpbgeDupliInstanceFromList(this);
        if (m_dupliDrawBase != nullptr) {
          scene->ForgetSharedMember(this);
          /* An Auto object's own Blender object (out of the graph, given its visibility back at
           * the end) ends where the game left it, as an object drawn by its own would; with Undo
           * at Exit the scene put the poses back before removing its objects. */
          if (scene->m_isRuntime ||
              (scene->GetBlenderScene()->gm.flag & GAME_USE_UNDO) == 0)
          {
            WriteOwnBlenderObject();
          }
        }
      }
      ReleaseDupliDrawOriginal();
      m_isUpbgeDupliInstance = false;
      m_dupliDrawBase = nullptr;
      return;
    }
    /* 2. Rendered blender::Object is a Replica (m_isReplica = true).
     * As we created a new blender::Object during KX_GameObject::ProcessReplica
     * (It is either a simple copy (BKE_id_copy) or a deep copy
     * (blender::ed::object::add_duplicate) of the original Object),
     * we need to remove it with BKE_id_delete */
    if (m_isReplica) {
      blender::bContext *C = KX_GetActiveEngine()->GetContext();
      blender::Main *bmain = CTX_data_main(C);
      RestoreOwnMaterials(ob, true);
      /* A Full Copy was converted at runtime, which noted its visibility to give back at the
       * end: the scene must not write into the deleted object then. */
      if (GetScene()) {
        GetScene()->ForgetVisibilityFlag(ob);
        /* A copy a merged scene made at its conversion (a collection instance's). */
        GetScene()->ForgetMergedLink(ob);
        /* Kept in Blender's graph for the next copy of its source: no graph rebuild now, nor
         * at that copy. */
        if (CanParkBlenderObject(ob) && GetScene()->ParkCopy(ob, m_replicaSourceUid)) {
          SetBlenderObject(nullptr);
          return;
        }
      }
      DeleteBlenderObjectWithOwnedData(bmain, ob);
      SetBlenderObject(nullptr);
      if (GetScene()) {
        GetScene()->TagForRelationsUpdate();
      }
      else {
        DEG_relations_tag_update(bmain);
      }
      return;
    }
    /* 3. The blender::Object is not a Replica nor a dupli Instance,
     * just hide the Original blender::Object. Its visibility flags will
     * be restored at KX_Scene exit */
    if (m_mergedBatch >= 0) {
      /* Its part of the batch goes for good; its own object never came back. */
      if (KX_Scene *scene = GetScene()) {
        scene->SetMergedPartShown(m_mergedBatch, m_mergedFirstVertex, m_mergedVertexCount, false);
      }
      m_mergedBatch = -1;
      return;
    }
    SetVisible(false, false);
    /* Hidden in Blender's graph: the drawing's own hide works for game objects only. */
    HideInGraph();
    /* Its color as converted: a menu opened again starts as authored, not lit by the highlight
     * action that was playing when it closed. */
    if (m_hasConvertedColor && !compare_v4v4(ob->color, m_convertedColor.getValue(), 0.0f)) {
      copy_v4_v4(ob->color, m_convertedColor.getValue());
      DEG_id_tag_update(&ob->id, ID_RECALC_SHADING);
    }
    /* Its materials as authored too (material actions changed copies). */
    RestoreOwnMaterials(ob, true);
  }
}

blender::Material *KX_GameObject::OwnMaterial(const short slot)
{
  for (const OwnMaterialSlot &own : m_ownMaterials) {
    if (own.slot == slot) {
      return own.copy;
    }
  }
  blender::Object *ob = GetBlenderObject();
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  blender::bContext *C = engine != nullptr ? engine->GetContext() : nullptr;
  blender::Main *bmain = C != nullptr ? CTX_data_main(C) : nullptr;
  /* A material of its own: a Merged object leaves its batch, a Shared one cannot (warned). */
  NoteOwnDrawingNeeded();
  if (ob == nullptr || bmain == nullptr || m_isUpbgeDupliInstance || slot < 1 ||
      slot > ob->totcol)
  {
    return nullptr;
  }
  blender::Material *source = BKE_object_material_get(ob, slot);
  if (source == nullptr) {
    return nullptr;
  }
  /* Without the source's animation data: an action assigned to the source would be evaluated on
   * the copy at the scene frame and undo what the game plays. */
  blender::ID *copy_id = nullptr;
  BKE_id_copy_ex(bmain, &source->id, &copy_id, LIB_ID_COPY_NO_ANIMDATA);
  blender::Material *copy = reinterpret_cast<blender::Material *>(copy_id);
  if (copy == nullptr) {
    return nullptr;
  }
  OwnMaterialSlot own;
  own.slot = slot;
  own.source = source;
  own.copy = copy;
  own.matbits = ob->matbits[slot - 1];
  own.object_material = ob->mat[slot - 1];
  /* Linked to the object: the mesh (shared with copies of this object) keeps its material. */
  BKE_object_material_assign(bmain, ob, copy, slot, BKE_MAT_ASSIGN_OBJECT);
  /* The slot holds the copy's only user. */
  id_us_min(&copy->id);
  m_ownMaterials.push_back(own);
  return copy;
}

blender::Material *KX_GameObject::SourceMaterial(const short slot)
{
  for (const OwnMaterialSlot &own : m_ownMaterials) {
    if (own.slot == slot) {
      return own.source;
    }
  }
  blender::Object *ob = GetBlenderObject();
  return ob != nullptr && slot >= 1 && slot <= ob->totcol ? BKE_object_material_get(ob, slot) :
                                                             nullptr;
}

void KX_GameObject::RestoreOwnMaterials(blender::Object *ob, const bool delete_copies)
{
  if (m_ownMaterials.empty()) {
    return;
  }
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  blender::bContext *C = engine != nullptr ? engine->GetContext() : nullptr;
  blender::Main *bmain = C != nullptr ? CTX_data_main(C) : nullptr;
  bool restored = false;
  for (const OwnMaterialSlot &own : m_ownMaterials) {
    if (ob != nullptr && own.slot <= ob->totcol && ob->mat[own.slot - 1] == own.copy) {
      id_us_min(&own.copy->id);
      ob->mat[own.slot - 1] = own.object_material;
      if (own.object_material != nullptr) {
        id_us_plus(&own.object_material->id);
      }
      ob->matbits[own.slot - 1] = own.matbits;
      restored = true;
    }
    if (delete_copies && bmain != nullptr && ID_REAL_USERS(&own.copy->id) <= 0) {
      BKE_id_delete(bmain, own.copy);
    }
  }
  m_ownMaterials.clear();
  if (restored) {
    DEG_id_tag_update(&ob->id, ID_RECALC_SYNC_TO_EVAL | ID_RECALC_GEOMETRY);
  }
  if (GetScene() != nullptr) {
    GetScene()->TagForRelationsUpdate();
  }
  else if (bmain != nullptr) {
    DEG_relations_tag_update(bmain);
  }
}

void KX_GameObject::CreateUpbgeDupliInstanceFromExisting()
{
  if (m_pBlenderObject && (m_pBlenderObject->gameflag & OB_DUPLI_UPBGE) == 0) {
    return;
  }
  m_isUpbgeDupliInstance = true;
  m_dupliDrawnShown = GetVisible();

  TagUpbgeDupliInstanceForTaaReset();

  GetScene()->AddUpbgeDupliInstanceToList(this);
}

void KX_GameObject::TagUpbgeDupliInstanceForTaaReset()
{
  if (!m_isUpbgeDupliInstance) {
    return;
  }

  /* Tag dupli Base object to reset taa samples +
   * update last_transform_update i guess - no noticeable
   * performances loss */
  blender::Object *base = GetDupliDrawBase();
  if (base->id.recalc == 0) {
    GetScene()->AppendToIdsToUpdate(
        &base->id, ID_RECALC_TRANSFORM, base->gameflag & OB_OVERLAY_COLLECTION);
  }
}

void KX_GameObject::MakeSharedInstance(blender::Object *base)
{
  /* Hidden at start: the provider skips it now; the drawing's own hide could take the group's
   * object out of the graph later (RetireDrawHiddenObjects). */
  if (m_drawHidden) {
    GetScene()->RemoveDrawHidden(this);
  }
  m_dupliDrawBase = base;
  m_isUpbgeDupliInstance = true;
  m_dupliDrawnShown = GetVisible();
  GetScene()->AddUpbgeDupliInstanceToList(this);
  TagUpbgeDupliInstanceForTaaReset();
}

void KX_GameObject::SetMergedPart(const int batch,
                                  const uint32_t first_vertex,
                                  const uint32_t vertex_count)
{
  m_mergedBatch = batch;
  m_mergedFirstVertex = first_vertex;
  m_mergedVertexCount = vertex_count;
  m_mergedPartHidden = false;
  /* The pose it has now is the one merged: a later change is a move. */
  NodeGetWorldTransform().getValue(&m_renderSyncedMatrix[0][0]);
  m_renderSyncedValid = true;
}

void KX_GameObject::LeaveMergedBatch()
{
  if (m_mergedBatch < 0) {
    return;
  }
  KX_Scene *scene = GetScene();
  scene->SetMergedPartShown(m_mergedBatch, m_mergedFirstVertex, m_mergedVertexCount, false);
  m_mergedBatch = -1;
  m_mergedPartHidden = false;
  /* Drawn by its own Blender object again, from the next pass's graph rebuild; the next compare
   * hands it the pose. */
  m_renderSyncedValid = false;
  if (GetVisible()) {
    SetGraphHidden(false);
  }
}

float KX_GameObject::GetOwnDrawDistance()
{
  if (!m_drawCull.distance_value_known) {
    m_drawCull.distance_value_known = true;
    m_drawCull.distance_value = GetProperty("draw_distance");
    if (m_drawCull.distance_value != nullptr) {
      m_drawCull.distance_value->AddRef();
    }
  }
  if (m_drawCull.distance_value == nullptr) {
    return 0.0f;
  }
  const double distance = m_drawCull.distance_value->GetNumber();
  return (distance > 0.0 && std::isfinite(distance)) ? float(distance) : 0.0f;
}

void KX_GameObject::PropertyMapChanged(const std::string &name)
{
  if (name.empty() || name == "draw_distance") {
    if (m_drawCull.distance_value != nullptr) {
      m_drawCull.distance_value->Release();
      m_drawCull.distance_value = nullptr;
    }
    m_drawCull.distance_value_known = false;
  }
}

void KX_GameObject::NoteOwnDrawingNeeded()
{
  if (m_mergedBatch >= 0) {
    LeaveMergedBatch();
  }
  else if (m_dupliDrawBase != nullptr) {
    LeaveSharedGroup();
  }
}

void KX_GameObject::WriteOwnBlenderObject()
{
  blender::Object *ob = GetBlenderObject();
  if (ob == nullptr) {
    return;
  }
  /* Grouped objects have no Blender parent (KX_Scene::SetupGameDrawing). */
  float object_to_world[4][4];
  NodeGetWorldTransform().getValue(&object_to_world[0][0]);
  copy_m4_m4(ob->runtime->object_to_world.ptr(), object_to_world);
  BKE_object_apply_mat4(ob, object_to_world, false, false);
  copy_v4_v4(ob->color, m_objectColor.getValue());
}

void KX_GameObject::LeaveSharedGroup()
{
  KX_Scene *scene = GetScene();
  /* The renderers draw again without the instance. */
  TagUpbgeDupliInstanceForTaaReset();
  scene->RemoveUpbgeDupliInstanceFromList(this);
  scene->ForgetSharedMember(this);
  ReleaseDupliDrawOriginal();
  m_isUpbgeDupliInstance = false;
  m_dupliDrawBase = nullptr;
  /* Its Blender object kept the pose and color it had at game start. */
  WriteOwnBlenderObject();
  /* The next sync compares from scratch: it hands Blender the pose and picks the pose path. */
  m_renderSyncedValid = false;
  m_evaluatedPoseDue = false;
  /* Back in Blender's graph (one rebuild), or left out while hidden: showing it later brings it
   * back. */
  if (GetVisible()) {
    SetGraphHidden(false);
  }
}

void KX_GameObject::SetDupliDrawIdentity(blender::Object &drawn,
                                         const float matrix[4][4],
                                         const uint64_t update_count)
{
  blender::Object *base = GetDupliDrawBase();
  if (base == nullptr) {
    return;
  }
  /* A copy of the base, so whatever reads the drawn object's original reads the base's values;
   * only its address differs (and stays the copy's for its life). Made whole once; what the
   * drawing reads through the original and the game can change is copied again when it changed
   * (2.4 KB written per copy per draw cost 1.4 ms among 10,000 copies). */
  const uint64_t base_state = blender::get_default_hash(
      blender::get_default_hash(base->data, base->mat, base->modifiers.first),
      blender::get_default_hash(base->modifiers.last, base->lodlevels.first, base->currentlod),
      blender::get_default_hash(base->id.recalc, base->type, base->mode),
      blender::get_default_hash(base->totcol, base->gameflag, base->transflag),
      blender::get_default_hash(base->visibility_flag, base->base_flag, base->matbits));
  if (m_dupliDrawOriginal == nullptr) {
    m_dupliDrawOriginal = new blender::Object(blender::dna::shallow_copy(*base));
  }
  else if (base_state != m_dupliDrawBaseState) {
    blender::Object &original = *m_dupliDrawOriginal;
    original.id.recalc = base->id.recalc;
    original.type = base->type;
    original.mode = base->mode;
    original.data = base->data;
    original.mat = base->mat;
    original.matbits = base->matbits;
    original.totcol = base->totcol;
    original.modifiers = base->modifiers;
    original.lodlevels = base->lodlevels;
    original.currentlod = base->currentlod;
    original.gameflag = base->gameflag;
    original.transflag = base->transflag;
    original.visibility_flag = base->visibility_flag;
    original.base_flag = base->base_flag;
  }
  m_dupliDrawBaseState = base_state;
  drawn.id.orig_id = &m_dupliDrawOriginal->id;
  /* An Auto object draws with its own name: shaders' Object Info Random and Workbench's random
   * color come from it, so it looks as it would drawn by its own Blender object. */
  if (m_dupliDrawBase != nullptr) {
    BLI_strncpy(drawn.id.name, m_pBlenderObject->id.name, sizeof(drawn.id.name));
    drawn.id.lib = m_pBlenderObject->id.lib;
  }
  drawn.runtime->last_update_transform = UpdateDupliDrawnMove(matrix, update_count);
}

uint64_t KX_GameObject::UpdateDupliDrawnMove(const float matrix[4][4], const uint64_t update_count)
{
  if (!m_dupliDrawnValid || !equals_m4m4(m_dupliDrawnMatrix, matrix)) {
    copy_m4_m4(m_dupliDrawnMatrix, matrix);
    m_dupliDrawnValid = true;
    m_dupliMovedUpdate = update_count;
  }
  return m_dupliMovedUpdate;
}

int KX_GameObject::GetDupliDrawId()
{
  /* Never reused while the game runs: a renderer keeps a gone copy's records under its key until
   * its next draw, and a new copy with that key would inherit its motion. */
  static int last_id = 0;
  if (m_dupliDrawId == 0) {
    last_id = (last_id >= std::numeric_limits<int>::max() - 1) ? 1 : last_id + 1;
    m_dupliDrawId = last_id;
  }
  return m_dupliDrawId;
}

uint32_t KX_GameObject::GetDupliDrawRandom()
{
  /* What a copy's own draw entry gives Object Info Random (draw::ObjectRef::random): the hash of
   * the entry's name, which is its Blender object's (an Auto object's own, a Dupli copy's base). */
  if (m_dupliDrawRandomOf != m_pBlenderObject) {
    m_dupliDrawRandomOf = m_pBlenderObject;
    m_dupliDrawRandom = m_pBlenderObject ? BLI_hash_int_2d(
                                               BLI_hash_string(m_pBlenderObject->id.name + 2), 0) :
                                           0;
  }
  return m_dupliDrawRandom;
}

void KX_GameObject::ReleaseDupliDrawOriginal()
{
  if (m_dupliDrawOriginal == nullptr) {
    return;
  }
  if (KX_Scene *scene = GetScene()) {
    /* Renderers keep the address as a key until their next draw. */
    scene->RetireDupliDrawOriginal(m_dupliDrawOriginal);
  }
  else {
    delete m_dupliDrawOriginal;
  }
  m_dupliDrawOriginal = nullptr;
  m_dupliDrawnValid = false;
}

void KX_GameObject::Dispose()
{
#ifdef WITH_PYTHON
  if (m_components) {
    for (KX_PythonComponent *comp : m_components) {
      comp->Dispose();
    }
  }
#endif
  SCA_IObject::Dispose();
}

static void suspend_physics_recursive(SG_Node *node, bool freeConstraints)
{
  const NodeList &children = node->GetSGChildren();

  for (SG_Node *childnode : children) {
    KX_GameObject *clientgameobj = static_cast<KX_GameObject *>(childnode->GetSGClientObject());
    if (clientgameobj != nullptr) {  // This is a GameObject
      clientgameobj->SuspendPhysics(freeConstraints, false);
    }

    // if the childobj is nullptr then this may be an inverse parent link
    // so a non recursive search should still look down this node.
    suspend_physics_recursive(childnode, freeConstraints);
  }
}

/* We Remove Physics controller */
void KX_GameObject::SuspendPhysics(bool freeConstraints, bool childrenRecursive)
{
  if (m_pPhysicsController) {
    GetPhysicsController()->SuspendPhysics(freeConstraints);
  }
  if (childrenRecursive) {
    suspend_physics_recursive(GetSGNode(), freeConstraints);
  }
}

static void restore_physics_recursive(SG_Node *node)
{
  const NodeList &children = node->GetSGChildren();

  for (SG_Node *childnode : children) {
    KX_GameObject *clientgameobj = static_cast<KX_GameObject *>(childnode->GetSGClientObject());
    if (clientgameobj != nullptr) {  // This is a GameObject
      clientgameobj->RestorePhysics(false);
    }

    // if the childobj is nullptr then this may be an inverse parent link
    // so a non recursive search should still look down this node.
    restore_physics_recursive(childnode);
  }
}

void KX_GameObject::RestorePhysics(bool childrenRecursive)
{
  if (m_pPhysicsController) {
    GetPhysicsController()->RestorePhysics();
  }
  if (childrenRecursive) {
    restore_physics_recursive(GetSGNode());
  }
}

static void suspend_logic_recursive(SG_Node *node)
{
  const NodeList &children = node->GetSGChildren();

  for (SG_Node *childnode : children) {
    KX_GameObject *clientgameobj = static_cast<KX_GameObject *>(childnode->GetSGClientObject());
    if (clientgameobj != nullptr) {  // This is a GameObject
      clientgameobj->SuspendLogic();
      if (clientgameobj->GetActionManagerNoCreate()) {
        clientgameobj->GetActionManagerNoCreate()->Suspend();
      }
    }

    // if the childobj is nullptr then this may be an inverse parent link
    // so a non recursive search should still look down this node.
    suspend_logic_recursive(childnode);
  }
}

/* We Disable Sensors */
void KX_GameObject::SuspendLogicAndActions(bool childrenRecursive)
{
  SuspendLogic();
  /* Even if SuspendSensors might stop actions, we call
   * m_actionManager->Suspend() to update IsActionsSuspended()
   * result */
  if (m_actionManager) {
    m_actionManager->Suspend();
  }

  if (childrenRecursive) {
    suspend_logic_recursive(GetSGNode());
  }
}

static void restore_logic_recursive(SG_Node *node)
{
  const NodeList &children = node->GetSGChildren();

  for (SG_Node *childnode : children) {
    KX_GameObject *clientgameobj = static_cast<KX_GameObject *>(childnode->GetSGClientObject());
    if (clientgameobj != nullptr) {  // This is a GameObject
      clientgameobj->ResumeLogic();
      if (clientgameobj->GetActionManagerNoCreate()) {
        clientgameobj->GetActionManagerNoCreate()->Resume();
      }
    }

    // if the childobj is nullptr then this may be an inverse parent link
    // so a non recursive search should still look down this node.
    restore_logic_recursive(childnode);
  }
}

void KX_GameObject::RestoreLogicAndActions(bool childrenRecursive)
{
  ResumeLogic();
  if (m_actionManager) {
    m_actionManager->Resume();
  }

  if (childrenRecursive) {
    restore_logic_recursive(GetSGNode());
  }
}

KX_GameObject::ActivityCullingInfo &KX_GameObject::GetActivityCullingInfo()
{
  return m_activityCullingInfo;
}

void KX_GameObject::SetActivityCullingInfo(const ActivityCullingInfo &cullingInfo)
{
  m_activityCullingInfo = cullingInfo;
}

void KX_GameObject::SetActivityCulling(ActivityCullingInfo::Flag flag, bool enable)
{
  if (enable) {
    m_activityCullingInfo.m_flags = (ActivityCullingInfo::Flag)(m_activityCullingInfo.m_flags |
                                                                flag);
  }
  else {
    m_activityCullingInfo.m_flags = (ActivityCullingInfo::Flag)(m_activityCullingInfo.m_flags &
                                                                ~flag);

    // Restore physics or logic when disabling activity culling.
    if (flag & ActivityCullingInfo::ACTIVITY_PHYSICS) {
      RestorePhysics(false);
    }
    if (flag & ActivityCullingInfo::ACTIVITY_LOGIC) {
      RestoreLogicAndActions(false);
    }
  }
}

void KX_GameObject::AddDummyLodManager(RAS_MeshObject *meshObj, blender::Object *ob)
{
  m_lodManager = new KX_LodManager(meshObj, ob);
  m_lodManager->AddRef();
  GetScene()->AddObjToLodObjList(this);
}

bool KX_GameObject::IsReplica()
{
  return m_isReplica;
}

void KX_GameObject::SetIsReplicaObject()
{
  m_isReplica = true;
  ClearPersistenceInstanceUuid();
}

void KX_GameObject::SetOwnedData(std::vector<blender::ID *> &&ids)
{
  m_ownedData = std::move(ids);
}

bool KX_GameObject::GiveOwnMesh()
{
  blender::Object *ob = GetBlenderObject();
  KX_Scene *scene = GetScene();
  if (ob == nullptr || ob->type != OB_MESH || ob->data == nullptr || !m_isReplica ||
      m_isUpbgeDupliInstance || scene == nullptr)
  {
    return false;
  }
  if (m_hasOwnMesh) {
    return true;
  }
  blender::Main *bmain = CTX_data_main(KX_GetActiveEngine()->GetContext());
  blender::ID *mesh = static_cast<blender::ID *>(ob->data);
  /* With its shape keys (Blender copies them with their mesh). */
  blender::ID *own = BKE_id_copy(bmain, mesh);
  if (own == nullptr) {
    return false;
  }
  /* The copy's one user is the object now. */
  ob->data = own;
  id_us_min(mesh);
  m_ownedData.push_back(own);
  m_hasOwnMesh = true;
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  scene->TagForRelationsUpdate();
  return true;
}

bool KX_GameObject::CanParkBlenderObject(const blender::Object *ob) const
{
  /* The next copy must get what a fresh copy of the source gets: only an ordinary copy the game
   * did not change for itself, outside the camera passes' hiding (Use Viewport Render), and
   * nothing Blender relates to other objects (a parent, constraints, a simulation, LOD levels,
   * text data the game writes, a menu's own copies). A level of detail manager (Replace Mesh
   * makes one) swaps the mesh Blender's evaluated copy draws. */
  return m_replicaSourceUid != 0 && m_blenderObjectReusable && !m_blenderObjectChanged &&
         m_ownedData.empty() && m_lodManager == nullptr &&
         !KX_GetActiveEngine()->UseViewportRender() && ob->data == m_replicaSourceData &&
         BLI_listbase_count(&ob->modifiers) == m_replicaSourceModifiers && ob->parent == nullptr &&
         BLI_listbase_is_empty(&ob->constraints) && BLI_listbase_is_empty(&ob->lodlevels) &&
         ob->type != OB_FONT && !KX_BlenderSimulatesObject(ob) &&
         (ob->gameflag & (OB_OVERLAY_COLLECTION | OB_DUPLI_UPBGE)) == 0;
}

void KX_GameObject::DeleteBlenderObjectWithOwnedData(blender::Main *bmain, blender::Object *ob)
{
  if (m_ownedData.empty()) {
    BKE_id_delete(bmain, ob);
    return;
  }
  /* The object goes with every owned ID whose users are all the object and the others going: one
   * the game gave another user stays, with what it uses. A mesh deformed on the GPU stays too:
   * the drawing keeps it registered by address after the object is gone. */
  blender::Set<blender::ID *> doomed = {&ob->id};
  for (blender::ID *id : m_ownedData) {
    const bool gpu_deformed = GS(id->name) == blender::ID_ME &&
                              reinterpret_cast<blender::Mesh *>(id)->is_running_gpu_animation_playback;
    if (!gpu_deformed) {
      doomed.add(id);
    }
  }
  for (bool changed = true; changed;) {
    changed = false;
    blender::Map<blender::ID *, int> users_inside;
    for (blender::ID *id : doomed) {
      blender::BKE_library_foreach_ID_link(
          bmain,
          id,
          [&](blender::LibraryIDLinkCallbackData *cb_data) {
            blender::ID *used = *cb_data->id_pointer;
            if (used != nullptr && (cb_data->cb_flag & blender::IDWALK_CB_USER) &&
                doomed.contains(used))
            {
              users_inside.lookup_or_add(used, 0)++;
            }
            return int(blender::IDWALK_RET_NOP);
          },
          nullptr,
          blender::IDWALK_READONLY);
    }
    for (blender::ID *id : m_ownedData) {
      if (!doomed.contains(id)) {
        continue;
      }
      /* Shape keys belong to their mesh (Blender deletes them with it), whatever their count. */
      const bool kept = GS(id->name) == blender::ID_KE ?
                            !doomed.contains(reinterpret_cast<blender::Key *>(id)->from) :
                            ID_REAL_USERS(id) > users_inside.lookup_default(id, 0);
      if (kept) {
        doomed.remove(id);
        changed = true;
      }
    }
  }
  m_ownedData.clear();
  BKE_id_multi_delete(bmain, doomed);
}

const blender::bUUID &KX_GameObject::GetPersistenceSourceUuid() const
{
  return m_persistenceSourceUuid;
}

void KX_GameObject::SetPersistenceSourceUuid(const blender::bUUID &uuid)
{
  m_persistenceSourceUuid = uuid;
}

const blender::bUUID &KX_GameObject::GetPersistenceInstanceUuid() const
{
  return m_persistenceInstanceUuid;
}

void KX_GameObject::SetPersistenceInstanceUuid(const blender::bUUID &uuid)
{
  m_persistenceInstanceUuid = uuid;
}

void KX_GameObject::ClearPersistenceInstanceUuid()
{
  m_persistenceInstanceUuid = blender::BLI_uuid_nil();
}

void KX_GameObject::SetSaveGameObjectState(const bool enabled)
{
  m_saveGameObjectState = enabled;
}

bool KX_GameObject::GetSaveGameObjectState() const
{
  return m_saveGameObjectState;
}

void KX_GameObject::AddSaveGameProperty(std::string name,
                                        const SaveGamePropertyMetadata::Type type,
                                        const bool timer)
{
  if (!name.empty()) {
    m_saveGameProperties.insert_or_assign(std::move(name), SaveGamePropertyMetadata{type, timer});
  }
}

bool KX_GameObject::IsSaveGameProperty(const std::string &name) const
{
  return GetSaveGamePropertyMetadata(name) != nullptr;
}

const KX_GameObject::SaveGamePropertyMetadata *KX_GameObject::GetSaveGamePropertyMetadata(
    const std::string &name) const
{
  const auto found = m_saveGameProperties.find(name);
  return found == m_saveGameProperties.end() ? nullptr : &found->second;
}

void KX_GameObject::AddReferenceValue(KX_ObjectReferenceValue *value)
{
  m_referenceValues.push_back(value);
}

void KX_GameObject::RemoveReferenceValue(KX_ObjectReferenceValue *value)
{
  std::erase(m_referenceValues, value);
}

void KX_GameObject::ClearReferenceValues()
{
  for (KX_ObjectReferenceValue *value : std::exchange(m_referenceValues, {})) {
    value->m_object = nullptr;
  }
}

bool KX_GameObject::HasSaveGameProperties() const
{
  return !m_saveGameProperties.empty();
}

const std::unordered_map<std::string, KX_GameObject::SaveGamePropertyMetadata> &KX_GameObject::
    GetSaveGameProperties() const
{
  return m_saveGameProperties;
}

void KX_GameObject::SetInventoryRuleSet(const std::string &property, const std::string &rules)
{
  if (rules.empty()) {
    m_inventoryRuleSets.erase(property);
  }
  else {
    m_inventoryRuleSets.insert_or_assign(property, rules);
  }
}

const std::string *KX_GameObject::GetInventoryRuleSet(const std::string &property) const
{
  const auto found = m_inventoryRuleSets.find(property);
  return found == m_inventoryRuleSets.end() ? nullptr : &found->second;
}

/********************End of EEVEE INTEGRATION*********************/

KX_GameObject *KX_GameObject::GetClientObject(KX_ClientObjectInfo *info)
{
  if (!info)
    return nullptr;
  return info->m_gameobject;
}

std::string KX_GameObject::GetName()
{
  return m_name;
}

/* Set the name of the value */
void KX_GameObject::SetName(const std::string &name)
{
  m_name = name;
}

PHY_IPhysicsController *KX_GameObject::GetPhysicsController()
{
  return m_pPhysicsController;
}

KX_GameObject *KX_GameObject::GetDupliGroupObject()
{
  return m_pDupliGroupObject;
}

EXP_ListValue<KX_GameObject> *KX_GameObject::GetInstanceObjects()
{
  return m_pInstanceObjects;
}

void KX_GameObject::AddInstanceObjects(KX_GameObject *obj)
{
  if (!m_pInstanceObjects)
    m_pInstanceObjects = new EXP_ListValue<KX_GameObject>();

  obj->AddRef();
  m_pInstanceObjects->Add(obj);
}

void KX_GameObject::RemoveInstanceObject(KX_GameObject *obj)
{
  BLI_assert(m_pInstanceObjects);
  m_pInstanceObjects->RemoveValue(obj);
  obj->Release();
}

void KX_GameObject::RemoveDupliGroupObject()
{
  if (m_pDupliGroupObject) {
    m_pDupliGroupObject->Release();
    m_pDupliGroupObject = nullptr;
    /* No longer in the collections of the Empty that instanced it. */
    InvalidateCollectionMembership();
  }
}

void KX_GameObject::SetDupliGroupObject(KX_GameObject *obj)
{
  obj->AddRef();
  m_pDupliGroupObject = obj;
  /* It now also counts as in the collections of the Empty instancing it. */
  InvalidateCollectionMembership();
}

void KX_GameObject::SetCollectionMembership(const blender::Collection *collection,
                                            const bool member)
{
  /* The latest call for a collection wins and counts as the newest, as the calls apply in
   * order (taking an object out of a parent collection, then putting it in a child of it). */
  std::erase_if(m_collectionMemberships,
                [collection](const auto &entry) { return entry.first == collection; });
  m_collectionMemberships.emplace_back(collection, member);
  InvalidateCollectionMembership();
}

void KX_GameObject::AddConstraint(blender::bRigidBodyJointConstraint *cons)
{
  m_constraints.push_back(cons);
}

std::vector<blender::bRigidBodyJointConstraint *> KX_GameObject::GetConstraints()
{
  return m_constraints;
}

void KX_GameObject::ClearConstraints()
{
  m_constraints.clear();
}

KX_GameObject::RigidBodyConstraintData *KX_GameObject::AddRigidBodyConstraint(
    blender::RigidBodyCon *cons,
    const std::string &name,
    blender::Object *ob1,
    blender::Object *ob2,
    const MT_Vector3 &pivotLocal,
    const MT_Matrix3x3 &basisLocal)
{
  if (!cons) {
    return nullptr;
  }

  for (RigidBodyConstraintData &data : m_rigidbodyConstraints) {
    if (data.m_constraintKey == cons) {
      data.m_settings = RigidBodyConstraintSettingsFromBlender(*cons);
      data.m_name = name;
      data.m_object1Name = ob1 ? ob1->id.name + 2 : "";
      data.m_object2Name = ob2 ? ob2->id.name + 2 : "";
      data.m_hasObject2 = (ob2 != nullptr);
      data.m_pivotLocal = pivotLocal;
      data.m_basisLocal = basisLocal;
      return &data;
    }
  }
  RigidBodyConstraintData data;
  data.m_constraintKey = cons;
  data.m_settings = RigidBodyConstraintSettingsFromBlender(*cons);
  data.m_name = name;
  data.m_object1Name = ob1 ? ob1->id.name + 2 : "";
  data.m_object2Name = ob2 ? ob2->id.name + 2 : "";
  data.m_hasObject2 = (ob2 != nullptr);
  data.m_pivotLocal = pivotLocal;
  data.m_basisLocal = basisLocal;
  data.m_constraintId = -1;  // Will be set after constraint is created
  m_rigidbodyConstraints.push_back(data);
  return &m_rigidbodyConstraints.back();
}

void KX_GameObject::AddRuntimeRigidBodyConstraint(const std::string &name,
                                                  KX_GameObject *object1,
                                                  KX_GameObject *object2,
                                                  const PHY_RigidBodyConstraintSettings &settings,
                                                  const MT_Vector3 &pivotLocal,
                                                  const MT_Matrix3x3 &basisLocal,
                                                  int constraintId)
{
  KX_Scene *scene = GetScene();
  PHY_IPhysicsEnvironment *physEnv = scene ? scene->GetPhysicsEnvironment() : nullptr;

  RigidBodyConstraintData data;
  data.m_constraintKey = nullptr;
  data.m_settings = settings;
  data.m_name = name;
  data.m_object1Name = object1 ? object1->GetName() : "";
  data.m_object2Name = object2 ? object2->GetName() : "";
  data.m_hasObject2 = (object2 != nullptr);
  data.m_pivotLocal = pivotLocal;
  data.m_basisLocal = basisLocal;
  data.m_constraintId = constraintId;

  const auto existing = std::find_if(
      m_rigidbodyConstraints.begin(),
      m_rigidbodyConstraints.end(),
      [&](const RigidBodyConstraintData &item) { return item.m_name == name; });
  if (existing == m_rigidbodyConstraints.end()) {
    m_rigidbodyConstraints.push_back(std::move(data));
    return;
  }

  const int existingConstraintId = existing->m_constraintId;
  if (physEnv && existingConstraintId != -1) {
    if (!physEnv->RemoveConstraintById(existingConstraintId, true)) {
      if (constraintId != -1) {
        physEnv->RemoveConstraintById(constraintId, true);
      }
      return;
    }
    /* Jolt synchronizes the registry during physical removal. Other backends
     * are handled by the explicit removal below. */
    RemoveRigidBodyConstraintId(existingConstraintId);
  }
  else {
    m_rigidbodyConstraints.erase(existing);
  }
  m_rigidbodyConstraints.push_back(std::move(data));
}

void KX_GameObject::SetRigidBodyConstraintId(blender::RigidBodyCon *cons, int constraintId)
{
  for (RigidBodyConstraintData &data : m_rigidbodyConstraints) {
    if (data.m_constraintKey == cons) {
      data.m_constraintId = constraintId;
      return;
    }
  }
}

const KX_GameObject::RigidBodyConstraintData *KX_GameObject::FindRigidBodyConstraint(
    const std::string &name) const
{
  const auto constraint = std::find_if(
      m_rigidbodyConstraints.begin(),
      m_rigidbodyConstraints.end(),
      [&](const RigidBodyConstraintData &data) { return data.m_name == name; });
  return constraint != m_rigidbodyConstraints.end() ? &*constraint : nullptr;
}

bool KX_GameObject::RemoveRigidBodyConstraint(const std::string &name)
{
  const auto constraint = std::find_if(
      m_rigidbodyConstraints.begin(),
      m_rigidbodyConstraints.end(),
      [&](const RigidBodyConstraintData &data) { return data.m_name == name; });
  if (constraint == m_rigidbodyConstraints.end()) {
    return false;
  }

  KX_Scene *scene = GetScene();
  PHY_IPhysicsEnvironment *physEnv = scene ? scene->GetPhysicsEnvironment() : nullptr;
  const int constraintId = constraint->m_constraintId;
  if (physEnv && constraintId != -1) {
    if (!physEnv->RemoveConstraintById(constraintId, true)) {
      return false;
    }
    RemoveRigidBodyConstraintId(constraintId);
    return true;
  }
  m_rigidbodyConstraints.erase(constraint);
  return true;
}

void KX_GameObject::RemoveRigidBodyConstraintId(int constraintId)
{
  if (constraintId == -1) {
    return;
  }

  m_rigidbodyConstraints.erase(std::remove_if(m_rigidbodyConstraints.begin(),
                                              m_rigidbodyConstraints.end(),
                                              [constraintId](const RigidBodyConstraintData &data) {
                                                return data.m_constraintId == constraintId;
                                              }),
                               m_rigidbodyConstraints.end());
}

void KX_GameObject::RemoveRigidBodyConstraintIds(const std::vector<int> &constraintIds)
{
  if (constraintIds.empty() || m_rigidbodyConstraints.empty()) {
    return;
  }
  if (constraintIds.size() == 1) {
    RemoveRigidBodyConstraintId(constraintIds.front());
    return;
  }

  std::unordered_set<int> constraintIdSet;
  constraintIdSet.reserve(constraintIds.size());
  for (const int constraintId : constraintIds) {
    if (constraintId != -1) {
      constraintIdSet.insert(constraintId);
    }
  }
  if (constraintIdSet.empty()) {
    return;
  }

  m_rigidbodyConstraints.erase(std::remove_if(m_rigidbodyConstraints.begin(),
                                              m_rigidbodyConstraints.end(),
                                              [&](const RigidBodyConstraintData &data) {
                                                return constraintIdSet.contains(
                                                    data.m_constraintId);
                                              }),
                               m_rigidbodyConstraints.end());
}

const std::vector<KX_GameObject::RigidBodyConstraintData> &KX_GameObject::GetRigidBodyConstraints()
    const
{
  return m_rigidbodyConstraints;
}

void KX_GameObject::ClearRigidBodyConstraints()
{
  m_rigidbodyConstraints.clear();
}

void KX_GameObject::RemoveRigidBodyConstraints()
{
  if (m_rigidbodyConstraints.empty()) {
    return;
  }

  KX_Scene *scene = GetScene();
  PHY_IPhysicsEnvironment *physEnv = scene ? scene->GetPhysicsEnvironment() : nullptr;
  if (!physEnv) {
    m_rigidbodyConstraints.clear();
    return;
  }

  std::vector<int> constraintIds;
  constraintIds.reserve(m_rigidbodyConstraints.size());
  for (const RigidBodyConstraintData &data : m_rigidbodyConstraints) {
    if (data.m_constraintId != -1) {
      constraintIds.push_back(data.m_constraintId);
    }
  }

  std::vector<int> removedConstraintIds;
  physEnv->RemoveConstraintsByIds(constraintIds, true, removedConstraintIds);
  RemoveRigidBodyConstraintIds(removedConstraintIds);

  m_rigidbodyConstraints.erase(std::remove_if(m_rigidbodyConstraints.begin(),
                                              m_rigidbodyConstraints.end(),
                                              [](const RigidBodyConstraintData &data) {
                                                return data.m_constraintId == -1;
                                              }),
                               m_rigidbodyConstraints.end());
}

bool KX_GameObject::HasRigidBodyConstraints() const
{
  return !m_rigidbodyConstraints.empty();
}

bool KX_GameObject::SetRigidBodyConstraintsEnabled(bool enabled,
                                                   const std::string &filterObjectName)
{
  if (m_rigidbodyConstraints.empty()) {
    return false;
  }

  KX_Scene *scene = GetScene();
  if (!scene) {
    return false;
  }

  PHY_IPhysicsEnvironment *physEnv = scene->GetPhysicsEnvironment();
  if (!physEnv) {
    return false;
  }

  bool changed = false;
  for (const RigidBodyConstraintData &data : m_rigidbodyConstraints) {
    if (data.m_constraintId == -1) {
      continue;
    }

    if (!filterObjectName.empty() && data.m_object1Name != filterObjectName) {
      continue;
    }

    physEnv->SetRigidBodyConstraintEnabled(data.m_constraintId, enabled);
    changed = true;
  }
  return changed;
}

void KX_GameObject::ReplicateRigidBodyConstraints(
    const std::unordered_map<std::string, KX_GameObject *> &objectLookup)
{
  if (!HasRigidBodyConstraints()) {
    return;
  }

  KX_Scene *scene = GetScene();
  if (!scene) {
    return;
  }

  PHY_IPhysicsEnvironment *physEnv = scene->GetPhysicsEnvironment();
  if (!physEnv) {
    return;
  }

  for (RigidBodyConstraintData &data : m_rigidbodyConstraints) {
    if (data.m_object1Name.empty()) {
      continue;
    }

    // O(1) hash map lookup instead of O(n) linear search
    auto it1 = objectLookup.find(data.m_object1Name);
    if (it1 == objectLookup.end()) {
      CM_Debug("ReplicateRigidBodyConstraints: target1 '" << data.m_object1Name
                                                          << "' not found for " << GetName());
      continue;
    }
    KX_GameObject *target1 = it1->second;

    if (!data.m_hasObject2 || data.m_object2Name.empty()) {
      continue;
    }

    KX_GameObject *target2 = nullptr;
    auto it2 = objectLookup.find(data.m_object2Name);
    if (it2 != objectLookup.end()) {
      target2 = it2->second;
    }
    else {
      CM_Debug("ReplicateRigidBodyConstraints: target2 '" << data.m_object2Name
                                                          << "' not found for " << GetName());
      continue;
    }

    int constraintId = physEnv->CreateRigidBodyConstraint(
        target1, target2, data.m_pivotLocal, data.m_basisLocal, data.m_settings, data.m_name);
    // Store any valid ID (only -1 means failure, other negative values are valid due to int
    // overflow)
    if (constraintId != -1) {
      data.m_constraintId = constraintId;
    }
  }
}

KX_GameObject *KX_GameObject::GetParent()
{
  KX_GameObject *result = nullptr;
  SG_Node *node = m_pSGNode;

  while (node && !result) {
    node = node->GetSGParent();
    if (node)
      result = (KX_GameObject *)node->GetSGClientObject();
  }

  return result;
}

void KX_GameObject::ClearParentIntent()
{
  m_parentCompoundIntent = false;
  m_parentGhostIntent = false;
  m_parentPhysicsManaged = false;
}

bool KX_GameObject::GetParentCompoundIntent() const
{
  return m_parentCompoundIntent;
}

bool KX_GameObject::GetParentGhostIntent() const
{
  return m_parentGhostIntent;
}

bool KX_GameObject::GetParentPhysicsManaged() const
{
  return m_parentPhysicsManaged;
}

bool KX_GameObject::BeginAuthoredTransformParenting()
{
  if (m_pSGNode == nullptr || m_pSGNode->GetSGParent() == nullptr ||
      m_pPhysicsController == nullptr)
  {
    return false;
  }
  if (m_parentPhysicsManaged) {
    if (m_pPhysicsController->IsTransformParentingActive()) {
      return true;
    }
    /* A failed or externally ended transition must not leave authored conversion
     * permanently unable to retry its engine-only parenting setup. */
    m_parentPhysicsManaged = false;
  }
  if (!m_pPhysicsController->BeginTransformParenting()) {
    return false;
  }

  m_parentCompoundIntent = false;
  m_parentGhostIntent = false;
  m_parentPhysicsManaged = true;
  return true;
}

const char *KX_ParentResultMessage(const KX_ParentResult result)
{
  switch (result) {
    case KX_ParentResult::Success:
      return "success";
    case KX_ParentResult::InvalidRequest:
      return "invalid child, parent, or parent type";
    case KX_ParentResult::DifferentScene:
      return "child and parent must belong to the same active or inactive scene list";
    case KX_ParentResult::ParentingLoop:
      return "parenting would create a hierarchy loop";
    case KX_ParentResult::InvalidTransform:
      return "parenting requires finite, non-singular transforms and non-zero scale";
    case KX_ParentResult::ParentIsNotArmature:
      return "Bone parenting requires an armature parent object";
    case KX_ParentResult::BoneNotFound:
      return "the requested pose bone was not found";
    case KX_ParentResult::JoltRequired:
      return "modern Set Parent requires a Jolt scene";
    case KX_ParentResult::UnsupportedPhysicsBody:
      return "the child physics body does not support transform parenting";
    case KX_ParentResult::CompoundUnavailable:
      return "Compound parenting requires a supported child collider and a compound-enabled "
             "Jolt root collider";
    case KX_ParentResult::PhysicsTransitionFailed:
      return "the Jolt body could not enter its parented state or update the compound shape";
  }
  return "unknown parenting failure";
}

namespace {

bool parent_transform_is_finite(const MT_Vector3 &position,
                                const MT_Matrix3x3 &orientation,
                                const MT_Vector3 &scale)
{
  for (int axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(position[axis]) || !std::isfinite(scale[axis]) ||
        std::abs(scale[axis]) <= MT_Scalar(FLT_EPSILON))
    {
      return false;
    }
    for (int column = 0; column < 3; ++column) {
      if (!std::isfinite(orientation[axis][column])) {
        return false;
      }
    }
  }
  const MT_Scalar determinant = orientation.determinant();
  return std::isfinite(determinant) && std::abs(determinant) > MT_Scalar(FLT_EPSILON);
}

bool vector_is_finite(const MT_Vector3 &value)
{
  return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

bool decompose_parent_transform(const MT_Transform &transform,
                                MT_Vector3 &r_position,
                                MT_Matrix3x3 &r_orientation,
                                MT_Vector3 &r_scale)
{
  r_position = transform.getOrigin();
  const MT_Matrix3x3 &basis = transform.getBasis();
  r_scale = MT_Vector3(
      basis.getColumn(0).length(), basis.getColumn(1).length(), basis.getColumn(2).length());
  if (!parent_transform_is_finite(r_position, basis, r_scale)) {
    return false;
  }
  r_orientation = basis.scaled(1.0f / r_scale.x(), 1.0f / r_scale.y(), 1.0f / r_scale.z());
  return parent_transform_is_finite(r_position, r_orientation, r_scale);
}

bool object_has_same_scene_membership(KX_Scene &scene,
                                      const KX_GameObject &child,
                                      const KX_GameObject &parent)
{
  EXP_ListValue<KX_GameObject> *active = scene.GetObjectList();
  EXP_ListValue<KX_GameObject> *inactive = scene.GetInactiveList();
  if (active == nullptr || inactive == nullptr) {
    return false;
  }
  const bool child_active = active->SearchValue(const_cast<KX_GameObject *>(&child));
  const bool child_inactive = inactive->SearchValue(const_cast<KX_GameObject *>(&child));
  const bool parent_active = active->SearchValue(const_cast<KX_GameObject *>(&parent));
  const bool parent_inactive = inactive->SearchValue(const_cast<KX_GameObject *>(&parent));
  return child_active != child_inactive && parent_active != parent_inactive &&
         child_active == parent_active;
}

struct ParentStateSnapshot {
  SG_Node *parent = nullptr;
  std::unique_ptr<SG_ParentRelation> relation;
  MT_Vector3 local_position;
  MT_Matrix3x3 local_orientation;
  MT_Vector3 local_scale;
  MT_Vector3 linear_velocity;
  MT_Vector3 angular_velocity;
  bool dynamic = false;
  bool compound = false;
  bool ghost = false;
  bool physics_managed = false;
};

ParentStateSnapshot capture_parent_state(KX_GameObject &object)
{
  ParentStateSnapshot snapshot;
  SG_Node *node = object.GetSGNode();
  snapshot.parent = node != nullptr ? node->GetSGParent() : nullptr;
  if (node != nullptr && node->GetParentRelation() != nullptr) {
    snapshot.relation.reset(node->GetParentRelation()->NewCopy());
  }
  snapshot.local_position = object.NodeGetLocalPosition();
  snapshot.local_orientation = object.NodeGetLocalOrientation();
  snapshot.local_scale = object.NodeGetLocalScaling();
  snapshot.compound = object.GetParentCompoundIntent();
  snapshot.ghost = object.GetParentGhostIntent();
  snapshot.physics_managed = object.GetParentPhysicsManaged();
  PHY_IPhysicsController *controller = object.GetPhysicsController();
  if (controller != nullptr && controller->IsDynamic()) {
    snapshot.dynamic = true;
    snapshot.linear_velocity = controller->GetLinearVelocity();
    snapshot.angular_velocity = controller->GetAngularVelocity();
  }
  return snapshot;
}

PHY_IPhysicsController *root_physics_controller(const SG_Node *node)
{
  const SG_Node *root_node = node != nullptr ? node->GetRootSGParent() : nullptr;
  KX_GameObject *root_object = root_node != nullptr ?
                                   static_cast<KX_GameObject *>(root_node->GetSGClientObject()) :
                                   nullptr;
  return root_object != nullptr ? root_object->GetPhysicsController() : nullptr;
}

}  // namespace

KX_ParentResult KX_GameObject::SetParent(KX_GameObject *obj, const KX_ParentOptions &options)
{
  if (obj == nullptr || obj == this || m_pSGNode == nullptr || obj->GetSGNode() == nullptr ||
      options.type > KX_ParentType::Bone ||
      (options.type == KX_ParentType::Bone && options.compound))
  {
    return KX_ParentResult::InvalidRequest;
  }
  KX_Scene *scene = GetScene();
  if (scene == nullptr || obj->GetScene() != scene ||
      !object_has_same_scene_membership(*scene, *this, *obj))
  {
    return KX_ParentResult::DifferentScene;
  }
  if (m_pSGNode->IsAncessor(obj->GetSGNode())) {
    return KX_ParentResult::ParentingLoop;
  }
  if (scene->GetBlenderScene() == nullptr || scene->GetBlenderScene()->gm.physicsEngine != UseJolt)
  {
    return KX_ParentResult::JoltRequired;
  }

  MT_Transform parent_transform;
  std::unique_ptr<SG_ParentRelation> relation;
  if (options.type == KX_ParentType::Bone) {
    if (dynamic_cast<BL_ArmatureObject *>(obj) == nullptr) {
      return KX_ParentResult::ParentIsNotArmature;
    }
    auto bone_relation = std::make_unique<KX_BoneParentRelation>(options.bone_name);
    if (!bone_relation->GetParentTransform(obj->GetSGNode(), parent_transform)) {
      return KX_ParentResult::BoneNotFound;
    }
    relation = std::move(bone_relation);
  }
  else {
    parent_transform = obj->NodeGetWorldTransform();
    relation = std::make_unique<KX_NormalParentRelation>();
  }

  MT_Vector3 local_position;
  MT_Matrix3x3 local_orientation;
  MT_Vector3 local_scale;
  const MT_Scalar parent_determinant = parent_transform.getBasis().determinant();
  if (!std::isfinite(parent_determinant) || std::abs(parent_determinant) <= MT_Scalar(FLT_EPSILON))
  {
    return KX_ParentResult::InvalidTransform;
  }
  MT_Transform local_transform;
  local_transform.multInverseLeft(parent_transform, NodeGetWorldTransform());
  if (!decompose_parent_transform(local_transform, local_position, local_orientation, local_scale))
  {
    return KX_ParentResult::InvalidTransform;
  }

  PHY_IPhysicsController *compound_root_controller = nullptr;
  if (options.compound) {
    compound_root_controller = root_physics_controller(obj->GetSGNode());
    if (m_pPhysicsController == nullptr || compound_root_controller == nullptr ||
        compound_root_controller == m_pPhysicsController ||
        !compound_root_controller->IsCompound())
    {
      return KX_ParentResult::CompoundUnavailable;
    }
  }

  /* Set Parent may be evaluated on consecutive ticks. Reapplying an identical
   * request would otherwise release and recreate the temporary Jolt parenting
   * state every time, despite there being no scene-graph work to do. Require
   * the actual controller state to be active as well: stale bookkeeping must
   * still take the normal recovery path below. */
  const bool same_direct_parent = m_pSGNode->GetSGParent() == obj->GetSGNode();
  bool same_relation = false;
  if (options.type == KX_ParentType::Object) {
    same_relation = dynamic_cast<KX_NormalParentRelation *>(m_pSGNode->GetParentRelation()) !=
                    nullptr;
  }
  else {
    const auto *bone_relation = dynamic_cast<KX_BoneParentRelation *>(
        m_pSGNode->GetParentRelation());
    same_relation = bone_relation != nullptr && bone_relation->GetBoneName() == options.bone_name;
  }
  const bool physics_state_matches = m_pPhysicsController == nullptr ||
                                     m_pPhysicsController->IsTransformParentingActive();
  if (same_direct_parent && same_relation && m_parentCompoundIntent == options.compound &&
      physics_state_matches)
  {
    return KX_ParentResult::Success;
  }

  /* An already managed controller is currently in a temporary Jolt state. RemoveParent() below
   * restores its authored state. Legacy compound children are temporary as well, so defer their
   * capability validation until the old compound shape has been removed. */
  if (m_pPhysicsController != nullptr && !m_pPhysicsController->IsTransformParentingActive() &&
      !m_parentCompoundIntent &&
      !(options.compound ? m_pPhysicsController->SupportsCompoundParenting() :
                           m_pPhysicsController->SupportsTransformParenting()))
  {
    return options.compound ? KX_ParentResult::CompoundUnavailable :
                              KX_ParentResult::UnsupportedPhysicsBody;
  }

  ParentStateSnapshot previous = capture_parent_state(*this);

  auto restore_previous_parent = [&]() {
    bool physics_managed_restored = false;
    bool compound_restored = previous.compound;
    if (m_pSGNode->GetSGParent() != nullptr) {
      RemoveParent();
    }
    m_pSGNode->SetParentRelation(previous.relation ? previous.relation.release() :
                                                     new KX_NormalParentRelation());

    if (previous.parent != nullptr) {
      previous.parent->AddChild(m_pSGNode);
      NodeSetLocalPosition(previous.local_position);
      NodeSetLocalOrientation(previous.local_orientation);
      NodeSetLocalScale(previous.local_scale);
      NodeUpdateGS(0.0f);
      EXP_ListValue<KX_GameObject> *previous_root_list = scene->GetRootParentList();
      if (previous_root_list != nullptr && previous_root_list->RemoveValue(this)) {
        Release();
      }

      if (m_pPhysicsController != nullptr) {
        if (previous.physics_managed) {
          physics_managed_restored = previous.compound ?
                                         m_pPhysicsController->BeginCompoundParenting() :
                                         m_pPhysicsController->BeginTransformParenting();
          if (!physics_managed_restored) {
            CM_Warning("Failed to restore the previous Jolt transform-parenting state for object '"
                       << GetName() << "'");
          }
        }
        else if (!m_pPhysicsController->IsKinematic() || previous.compound) {
          m_pPhysicsController->SuspendDynamics(previous.ghost && previous.compound);
        }

        if (previous.compound) {
          PHY_IPhysicsController *previous_root_controller = root_physics_controller(
              previous.parent);
          if ((previous.physics_managed && !physics_managed_restored) ||
              previous_root_controller == nullptr || !previous_root_controller->IsCompound() ||
              !previous_root_controller->TryAddCompoundChild(m_pPhysicsController))
          {
            compound_restored = false;
            if (physics_managed_restored) {
              m_pPhysicsController->EndTransformParenting();
              physics_managed_restored = false;
            }
            else {
              m_pPhysicsController->RestoreDynamics();
            }
            CM_Warning("Failed to restore the previous compound-parenting state for object '"
                       << GetName() << "'");
          }
        }
      }
      else {
        compound_restored = false;
      }
    }
    else {
      compound_restored = false;
      NodeSetLocalPosition(previous.local_position);
      NodeSetLocalOrientation(previous.local_orientation);
      NodeSetLocalScale(previous.local_scale);
      NodeUpdateGS(0.0f);
      EXP_ListValue<KX_GameObject> *previous_root_list = scene->GetRootParentList();
      if (previous_root_list != nullptr && !previous_root_list->SearchValue(this)) {
        previous_root_list->Add(CM_AddRef(this));
      }
    }

    m_parentCompoundIntent = compound_restored;
    m_parentGhostIntent = compound_restored && previous.ghost;
    m_parentPhysicsManaged = physics_managed_restored;
    if (previous.dynamic && m_pPhysicsController != nullptr) {
      m_pPhysicsController->SetLinearVelocity(previous.linear_velocity, false);
      m_pPhysicsController->SetAngularVelocity(previous.angular_velocity, false);
    }
  };

  RemoveParent();
  m_pSGNode->SetParentRelation(relation.release());
  obj->GetSGNode()->AddChild(m_pSGNode);
  NodeSetLocalPosition(local_position);
  NodeSetLocalOrientation(local_orientation);
  NodeSetLocalScale(local_scale);
  NodeUpdateGS(0.0f);

  EXP_ListValue<KX_GameObject> *root_list = scene->GetRootParentList();
  if (root_list != nullptr && root_list->RemoveValue(this)) {
    Release();
  }

  bool physics_started = false;
  if (m_pPhysicsController != nullptr) {
    /* Claim the transition before entering Jolt so a partial failure is rolled
     * back through EndTransformParenting(), never through the legacy dynamics
     * restore path. */
    m_parentPhysicsManaged = true;
    physics_started = options.compound ? m_pPhysicsController->BeginCompoundParenting() :
                                         m_pPhysicsController->BeginTransformParenting();
    if (!physics_started) {
      RemoveParent();
      restore_previous_parent();
      return KX_ParentResult::PhysicsTransitionFailed;
    }
  }
  m_parentPhysicsManaged = physics_started;
  m_parentCompoundIntent = false;
  m_parentGhostIntent = false;

  if (options.compound && !compound_root_controller->TryAddCompoundChild(m_pPhysicsController)) {
    RemoveParent();
    restore_previous_parent();
    return KX_ParentResult::PhysicsTransitionFailed;
  }
  m_parentCompoundIntent = options.compound;

  return KX_ParentResult::Success;
}

void KX_GameObject::SetParent(KX_GameObject *obj, bool addToCompound, bool ghost)
{
  // check on valid node in case a python controller holds a reference to a deleted object
  if (obj &&
      GetSGNode()->GetSGParent() != obj->GetSGNode() &&  // not already parented to same object
      !GetSGNode()->IsAncessor(obj->GetSGNode()) &&      // no parenting loop
      this != obj)                                       // not the object itself
  {
    if (!(GetScene()->GetInactiveList()->SearchValue(obj) !=
          GetScene()->GetObjectList()->SearchValue(this)))
    {
      CM_FunctionWarning(
          "child and parent are not in the same game objects list (active or inactive). This "
          "operation is forbidden.");
      return;
    }
    // Make sure the objects have some scale
    MT_Vector3 scale1 = NodeGetWorldScaling();
    MT_Vector3 scale2 = obj->NodeGetWorldScaling();
    if (fabs(scale2[0]) < (MT_Scalar)FLT_EPSILON || fabs(scale2[1]) < (MT_Scalar)FLT_EPSILON ||
        fabs(scale2[2]) < (MT_Scalar)FLT_EPSILON || fabs(scale1[0]) < (MT_Scalar)FLT_EPSILON ||
        fabs(scale1[1]) < (MT_Scalar)FLT_EPSILON || fabs(scale1[2]) < (MT_Scalar)FLT_EPSILON)
    {
      return;
    }

    KX_Scene *scene = GetScene();
    // Remove us from our old parent and set our new parent
    RemoveParent();
    GetSGNode()->SetParentRelation(new KX_NormalParentRelation());
    obj->GetSGNode()->AddChild(GetSGNode());

    if (m_pPhysicsController && (!m_pPhysicsController->IsKinematic() || addToCompound)) {
      /* Parent-actuator ghost mode is only meaningful when adding the child
       * to the parent's compound setup. If compound mode is disabled, ignore
       * any stale ghost flag value (UI can keep a disabled checkbox state).
       * Authored Kinematic bodies remain active when normally parented: their
       * world transform is already an explicit physics-step target, so there
       * is no dynamic/scene-graph ownership conflict. */
      m_pPhysicsController->SuspendDynamics(ghost && addToCompound);
    }
    // Set us to our new scale, position, and orientation
    scale2[0] = 1.0f / scale2[0];
    scale2[1] = 1.0f / scale2[1];
    scale2[2] = 1.0f / scale2[2];
    scale1 = scale1 * scale2;
    MT_Matrix3x3 invori = obj->NodeGetWorldOrientation().inverse();
    MT_Vector3 newpos = invori * (NodeGetWorldPosition() - obj->NodeGetWorldPosition()) * scale2;

    NodeSetLocalScale(scale1);
    NodeSetLocalPosition(MT_Vector3(newpos[0], newpos[1], newpos[2]));
    NodeSetLocalOrientation(invori * NodeGetWorldOrientation());
    NodeUpdateGS(0.f);
    // object will now be a child, it must be removed from the parent list
    EXP_ListValue<KX_GameObject> *rootlist = scene->GetRootParentList();
    if (rootlist->RemoveValue(this))
      // the object was in parent list, decrement ref count as it's now removed
      Release();
    // if the new parent is a compound object, add this object shape to the compound shape.
    // step 0: verify this object has physical controller
    if (m_pPhysicsController && addToCompound) {
      // step 1: find the top parent (not necessarily obj)
      KX_GameObject *rootobj =
          (KX_GameObject *)obj->GetSGNode()->GetRootSGParent()->GetSGClientObject();
      // step 2: verify it has a physical controller and compound shape
      if (rootobj != nullptr && rootobj->m_pPhysicsController != nullptr &&
          rootobj->m_pPhysicsController->IsCompound())
      {
        rootobj->m_pPhysicsController->AddCompoundChild(m_pPhysicsController);
      }
    }

    /* Record only a completed hierarchy change. Ghost has no runtime meaning when compound mode
     * is disabled, matching the normalized argument passed to SuspendDynamics above. */
    m_parentCompoundIntent = addToCompound;
    m_parentGhostIntent = addToCompound && ghost;
  }
}

void KX_GameObject::RemoveParent()
{
  // check on valid node in case a python controller holds a reference to a deleted object
  if (GetSGNode()->GetSGParent()) {
    const bool physics_managed =
        m_parentPhysicsManaged ||
        (m_pPhysicsController != nullptr && m_pPhysicsController->IsTransformParentingActive());
    const bool compound_parent = m_parentCompoundIntent;
    // get the root object to remove us from compound object if needed
    KX_GameObject *rootobj = (KX_GameObject *)GetSGNode()->GetRootSGParent()->GetSGClientObject();
    // Set us to the right spot
    GetSGNode()->SetLocalScale(GetSGNode()->GetWorldScaling());
    GetSGNode()->SetLocalOrientation(GetSGNode()->GetWorldOrientation());
    GetSGNode()->SetLocalPosition(GetSGNode()->GetWorldPosition());

    // Remove us from our parent
    GetSGNode()->DisconnectFromParent();
    GetSGNode()->SetParentRelation(new KX_NormalParentRelation());
    NodeUpdateGS(0.f);

    KX_Scene *scene = GetScene();
    // the object is now a root object, add it to the parentlist
    EXP_ListValue<KX_GameObject> *rootlist = scene->GetRootParentList();
    if (!rootlist->SearchValue(this))
      // object was not in root list, add it now and increment ref count
      rootlist->Add(CM_AddRef(this));
    if (m_pPhysicsController) {
      // in case this controller was added as a child shape to the parent
      if (compound_parent && rootobj != nullptr && rootobj->m_pPhysicsController != nullptr &&
          rootobj->m_pPhysicsController->IsCompound())
      {
        rootobj->m_pPhysicsController->RemoveCompoundChild(m_pPhysicsController);
      }
      if (physics_managed) {
        m_pPhysicsController->EndTransformParenting();
      }
      else {
        m_pPhysicsController->RestoreDynamics();
      }
      if (m_pPhysicsController->IsDynamic() && !m_pPhysicsController->IsDynamicsSuspended() &&
          !m_pPhysicsController->IsPhysicsSuspended() && rootobj != nullptr &&
          rootobj->GetSGNode() != nullptr && rootobj->m_pPhysicsController != nullptr)
      {
        /* A released Dynamic child inherits the velocity of its attachment point,
         * including the parent's angular contribution at the child's world position. */
        const MT_Vector3 relPoint = GetSGNode()->GetWorldPosition() -
                                    rootobj->GetSGNode()->GetWorldPosition();
        const MT_Vector3 linearVelocity = rootobj->m_pPhysicsController->GetVelocity(relPoint);
        const MT_Vector3 angularVelocity = rootobj->m_pPhysicsController->GetAngularVelocity();
        if (vector_is_finite(linearVelocity) && vector_is_finite(angularVelocity)) {
          m_pPhysicsController->SetLinearVelocity(linearVelocity, false);
          m_pPhysicsController->SetAngularVelocity(angularVelocity, false);
        }
      }
    }
  }
  ClearParentIntent();
}

namespace {

enum class SaveObjectStateListMembership : uint8_t {
  Invalid = 0,
  Active,
  Inactive,
};

SaveObjectStateListMembership save_object_state_list_membership(KX_Scene &scene,
                                                                KX_GameObject &object)
{
  EXP_ListValue<KX_GameObject> *active = scene.GetObjectList();
  EXP_ListValue<KX_GameObject> *inactive = scene.GetInactiveList();
  if (active == nullptr || inactive == nullptr) {
    return SaveObjectStateListMembership::Invalid;
  }
  const bool is_active = active->SearchValue(&object);
  const bool is_inactive = inactive->SearchValue(&object);
  if (is_active == is_inactive) {
    return SaveObjectStateListMembership::Invalid;
  }
  return is_active ? SaveObjectStateListMembership::Active :
                     SaveObjectStateListMembership::Inactive;
}

bool save_object_state_parent_chain_has_cycle(SG_Node *node)
{
  SG_Node *slow = node;
  SG_Node *fast = node;
  while (fast != nullptr && fast->GetSGParent() != nullptr) {
    slow = slow->GetSGParent();
    fast = fast->GetSGParent()->GetSGParent();
    if (slow != nullptr && slow == fast) {
      return true;
    }
  }
  return false;
}

bool save_object_state_normal_parent_chain(SG_Node *node,
                                           KX_Scene &scene,
                                           const SaveObjectStateListMembership membership,
                                           const SG_Node *forbidden)
{
  if (node == nullptr || save_object_state_parent_chain_has_cycle(node)) {
    return false;
  }
  for (SG_Node *cursor = node; cursor != nullptr; cursor = cursor->GetSGParent()) {
    if (cursor == forbidden || cursor->GetSGClientInfo() != &scene) {
      return false;
    }
    if (void *client = cursor->GetSGClientObject()) {
      KX_GameObject *object = static_cast<KX_GameObject *>(client);
      if (save_object_state_list_membership(scene, *object) != membership ||
          object->GetParentCompoundIntent() || object->GetParentGhostIntent() ||
          object->GetParentPhysicsManaged())
      {
        return false;
      }
    }
    if (cursor->GetSGParent() != nullptr &&
        dynamic_cast<KX_NormalParentRelation *>(cursor->GetParentRelation()) == nullptr)
    {
      return false;
    }
  }
  return true;
}

bool save_object_state_nonsingular_transform(const KX_GameObject &object)
{
  const MT_Vector3 &position = object.NodeGetWorldPosition();
  const MT_Matrix3x3 &orientation = object.NodeGetWorldOrientation();
  const MT_Vector3 &scale = object.NodeGetWorldScaling();
  for (int axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(position[axis]) || !std::isfinite(scale[axis]) ||
        std::abs(scale[axis]) < MT_Scalar(FLT_EPSILON))
    {
      return false;
    }
    for (int column = 0; column < 3; ++column) {
      if (!std::isfinite(orientation[axis][column])) {
        return false;
      }
    }
  }
  const MT_Scalar determinant = orientation.determinant();
  return std::isfinite(determinant) && std::abs(determinant) >= MT_Scalar(FLT_EPSILON);
}

}  // namespace

bool KX_GameObject::RestoreSaveObjectStateParent(KX_GameObject *parent)
{
  if (m_pSGNode == nullptr || m_parentCompoundIntent || m_parentGhostIntent ||
      !save_object_state_nonsingular_transform(*this))
  {
    return false;
  }
  /* Unchanged Jolt-managed parenting is already restored. */
  if (m_parentPhysicsManaged && parent != nullptr && GetParent() == parent) {
    return true;
  }

  KX_Scene *scene = GetScene();
  const SaveObjectStateListMembership membership = scene != nullptr ?
                                                       save_object_state_list_membership(*scene,
                                                                                         *this) :
                                                       SaveObjectStateListMembership::Invalid;
  /* A Jolt-managed child leaves its current parent below, so only the new chain matters. */
  if (scene == nullptr || membership == SaveObjectStateListMembership::Invalid ||
      (!m_parentPhysicsManaged &&
       !save_object_state_normal_parent_chain(m_pSGNode, *scene, membership, nullptr)))
  {
    return false;
  }
  if (parent != nullptr &&
      (parent == this || parent->GetSGNode() == nullptr || parent->GetScene() != scene ||
       save_object_state_list_membership(*scene, *parent) != membership ||
       !save_object_state_nonsingular_transform(*parent) ||
       !save_object_state_normal_parent_chain(parent->GetSGNode(), *scene, membership, m_pSGNode)))
  {
    return false;
  }

  if (m_parentPhysicsManaged) {
    /* Leaving the parent ends Jolt-managed parenting and restores the body's own state. */
    RemoveParent();
  }
  if (parent == nullptr) {
    RemoveParent();
    return m_pSGNode->GetSGParent() == nullptr && !m_parentCompoundIntent && !m_parentGhostIntent;
  }

  SetParent(parent, false, false);
  return m_pSGNode->GetSGParent() == parent->GetSGNode() &&
         dynamic_cast<KX_NormalParentRelation *>(m_pSGNode->GetParentRelation()) != nullptr &&
         !m_parentCompoundIntent && !m_parentGhostIntent;
}

BL_ActionManager *KX_GameObject::GetActionManager()
{
  // We only want to create an action manager if we need it
  if (!m_actionManager) {
    GetScene()->AddAnimatedObject(this);
    m_actionManager = new BL_ActionManager(this);
  }
  return m_actionManager;
}

BL_ActionManager *KX_GameObject::GetActionManagerNoCreate()
{
  return m_actionManager;
}

bool KX_GameObject::PlayAction(const std::string &name,
                               float start,
                               float end,
                               short layer,
                               short priority,
                               float blendin,
                               short play_mode,
                               float layer_weight,
                               short ipo_flags,
                               float playback_speed,
                               short blend_mode)
{
  return GetActionManager()->PlayAction(name,
                                        start,
                                        end,
                                        layer,
                                        priority,
                                        blendin,
                                        play_mode,
                                        layer_weight,
                                        ipo_flags,
                                        playback_speed,
                                        blend_mode);
}

BL_ActionPlayResult KX_GameObject::PlayActionTracked(const std::string &name,
                                                     const float start,
                                                     const float end,
                                                     const short layer,
                                                     const short priority,
                                                     const float blendin,
                                                     const short play_mode,
                                                     const float layer_weight,
                                                     const short ipo_flags,
                                                     const float playback_speed,
                                                     const short blend_mode)
{
  return GetActionManager()->PlayActionTracked(name,
                                               start,
                                               end,
                                               layer,
                                               priority,
                                               blendin,
                                               play_mode,
                                               layer_weight,
                                               ipo_flags,
                                               playback_speed,
                                               blend_mode);
}

void KX_GameObject::StopAction(short layer)
{
  GetActionManager()->StopAction(layer);
}

void KX_GameObject::RemoveTaggedActions()
{
  GetActionManager()->RemoveTaggedActions();
}

bool KX_GameObject::IsActionDone(short layer)
{
  return GetActionManager()->IsActionDone(layer);
}

bool KX_GameObject::IsActionsSuspended()
{
  return GetActionManager()->IsSuspended();
}

void KX_GameObject::UpdateActionManager(float curtime, bool applyToObject)
{
  GetActionManager()->Update(curtime, applyToObject);
}

void KX_GameObject::RebaseActionTime(const float previous_time,
                                     const float current_time,
                                     const bool use_pause_clock)
{
  if (m_actionManager != nullptr) {
    m_actionManager->RebaseTime(previous_time, current_time, use_pause_clock);
  }
}

float KX_GameObject::GetActionFrame(short layer)
{
  return GetActionManager()->GetActionFrame(layer);
}

const std::string KX_GameObject::GetActionName(short layer)
{
  return GetActionManager()->GetActionName(layer);
}

void KX_GameObject::SetActionFrame(short layer, float frame)
{
  GetActionManager()->SetActionFrame(layer, frame);
}

blender::bAction *KX_GameObject::GetCurrentAction(short layer)
{
  return GetActionManager()->GetCurrentAction(layer);
}

void KX_GameObject::SetPlayMode(short layer, short mode)
{
  GetActionManager()->SetPlayMode(layer, mode);
}

void KX_GameObject::ProcessReplica()
{
  /* The source's draw distance value is the source's to release; the copy finds its own. */
  m_drawCull.distance_value = nullptr;
  m_drawCull.distance_value_known = false;
  m_drawCull.culled_cameras = 0;
  m_drawCull.pending_cameras = 0;
  m_drawCull.pass = 0;
  KX_PythonProxy::ProcessReplica();

  /* Runtime interpolation bookkeeping belongs to the original scene object and must never be
   * inherited by a replica made while the original is being rendered between fixed poses. */
  m_cachedRenderPosition = MT_Vector3(0.0f, 0.0f, 0.0f);
  m_cachedRenderOrientation = MT_Matrix3x3::Identity();
  m_cachedRenderScale = MT_Vector3(1.0f, 1.0f, 1.0f);
  m_cachedInterpolatedTransform = MT_Transform::Identity();
  m_useRenderInterpolation = false;
  m_interpolationListRegistered = false;
  /* The copy's own Blender object has had nothing handed to it yet. */
  m_renderSyncedValid = false;
  m_renderSyncedFrame = 0;
  m_evaluatedPoseDue = false;
  /* Its own Blender object (or the instances' one): its own pose path. */
  m_posePath = PosePath::Unknown;
  m_posePathRelations = 0;
  m_posePathProbeStart = 0;
  m_posePathProbes = 0;
  m_poseCheckPass = 0;
  /* The copy keeps the Smooth Motion choice, not the source's jump, fade or drawn frame. */
  m_smoothResetPending = false;
  m_smoothCorrectionLeft = 0.0f;
  m_smoothDrawnFrame = 0;
  ClearParentIntent();

  /* Replicas retain their fixed source identity and receive an instance identity only if the
   * checkpoint registry promotes them. */
  ClearPersistenceInstanceUuid();

  /* A Scroll or Grid View clipped the source; no view owns the copy (yet). */
  m_viewClipVisible = true;
  /* References to the source stay with the source. */
  m_referenceValues.clear();
  /* The source gives its Blender object's color back; a copy draws its own (or the shared
   * instance's) Blender object. */
  m_hasConvertedColor = false;

  blender::Object *source_object = m_pBlenderObject;
  ReplicateBlenderObject();
  /* The source's own material copies stay its own (they go with it): a copy of its Blender
   * object gets back what those slots held before. */
  RestoreOwnMaterials(m_pBlenderObject != source_object ? m_pBlenderObject : nullptr, false);
  /* A copy of a Full Copy shares its source's data; the source deletes it (or keeps it while the
   * copy uses it). */
  m_ownedData.clear();
  m_hasOwnMesh = false;

  m_isUpbgeDupliInstance = false;
  /* The scene lists the source; the copy's own Blender object is shown (ReplicateBlenderObject). */
  m_drawHidden = false;
  /* A copy of an Auto or Merged object is drawn by its own Blender object. */
  m_dupliDrawBase = nullptr;
  m_dupliListIndex = -1;
  m_mergedBatch = -1;
  m_mergedPartHidden = false;
  /* The source's draw identity stays the source's; the copy gets its own at its first draw. */
  m_dupliDrawOriginal = nullptr;
  m_dupliDrawnValid = false;
  m_dupliDrawId = 0;
  m_dupliGroupedPass = 0;

  GetScene()->GetBlenderSceneConverter()->RegisterGameObject(this, m_pBlenderObject);
  CreateUpbgeDupliInstanceFromExisting();

  if (m_lodManager) {
    m_lodManager->AddRef();
    GetScene()->AddObjToLodObjList(this);
  }

  m_pPhysicsController = nullptr;
  m_pSGNode = nullptr;

  /* Dupli group and instance list are set later in replication.
   * See KX_Scene::DupliGroupRecurse. */
  m_pDupliGroupObject = nullptr;
  m_pInstanceObjects = nullptr;
  /* The copy's collections are found again (KX_Scene::CollectionsOf). It keeps Set Collection
   * Member's changes of its source. */
  InvalidateCollectionMembership();
  m_pClient_info = new KX_ClientObjectInfo(*m_pClient_info);
  m_pClient_info->m_gameobject = this;
  m_actionManager = nullptr;
  m_state = 0;

#ifdef WITH_PYTHON

  if (m_attr_dict)
    m_attr_dict = PyDict_Copy(m_attr_dict);

  if (m_components) {
    m_components = (EXP_ListValue<KX_PythonComponent> *)m_components->GetReplica();
    for (KX_PythonComponent *component : m_components) {
      component->SetGameObject(this);
    }
  }

#endif
}

void KX_GameObject::SetCollisionGroup(unsigned short group)
{
  if (m_pPhysicsController) {
    m_pPhysicsController->SetCollisionGroup(group);
    m_pPhysicsController->RefreshCollisions();
  }
}
void KX_GameObject::SetCollisionMask(unsigned short mask)
{
  if (m_pPhysicsController) {
    m_pPhysicsController->SetCollisionMask(mask);
    m_pPhysicsController->RefreshCollisions();
  }
}

unsigned short KX_GameObject::GetCollisionGroup() const
{
  return m_pPhysicsController ? m_pPhysicsController->GetCollisionGroup() : 0;
}

unsigned short KX_GameObject::GetCollisionMask() const
{
  return m_pPhysicsController ? m_pPhysicsController->GetCollisionMask() : 0;
}

KX_PythonProxy *KX_GameObject::NewInstance()
{
  return new KX_GameObject(*this);
}

bool KX_GameObject::IsDynamic() const
{
  if (m_pPhysicsController) {
    return m_pPhysicsController->IsDynamic();
  }
  return false;
}

bool KX_GameObject::IsDynamicsSuspended() const
{
  if (m_pPhysicsController)
    return m_pPhysicsController->IsDynamicsSuspended();
  return false;
}

float KX_GameObject::getLinearDamping() const
{
  if (m_pPhysicsController)
    return m_pPhysicsController->GetLinearDamping();
  return 0;
}

float KX_GameObject::getAngularDamping() const
{
  if (m_pPhysicsController)
    return m_pPhysicsController->GetAngularDamping();
  return 0;
}

void KX_GameObject::setLinearDamping(float damping)
{
  if (m_pPhysicsController)
    m_pPhysicsController->SetLinearDamping(damping);
}

void KX_GameObject::setAngularDamping(float damping)
{
  if (m_pPhysicsController)
    m_pPhysicsController->SetAngularDamping(damping);
}

void KX_GameObject::setDamping(float linear, float angular)
{
  if (m_pPhysicsController)
    m_pPhysicsController->SetDamping(linear, angular);
}

void KX_GameObject::setCcdMotionThreshold(float motion_threshold)
{
  if (m_pPhysicsController)
    m_pPhysicsController->SetCcdMotionThreshold(motion_threshold);
}

void KX_GameObject::setCcdSweptSphereRadius(float swept_sphere_radius)
{
  if (m_pPhysicsController)
    m_pPhysicsController->SetCcdSweptSphereRadius(swept_sphere_radius);
}

void KX_GameObject::ApplyForce(const MT_Vector3 &force, bool local)
{
  if (m_pPhysicsController)
    m_pPhysicsController->ApplyForce(force, local);
}

void KX_GameObject::ApplyTorque(const MT_Vector3 &torque, bool local)
{
  if (m_pPhysicsController)
    m_pPhysicsController->ApplyTorque(torque, local);
}

void KX_GameObject::ApplyMovement(const MT_Vector3 &dloc, bool local)
{
  if (m_pPhysicsController)  // (IsDynamic())
  {
    m_pPhysicsController->RelativeTranslate(dloc, local);
  }
  GetSGNode()->RelativeTranslate(dloc, GetSGNode()->GetSGParent(), local);
  NodeUpdateGS(0.0f);
}

void KX_GameObject::ApplyRotation(const MT_Vector3 &drot, bool local)
{
  ApplyRotation(MT_Matrix3x3(drot), local);
}

void KX_GameObject::ApplyRotation(const MT_Matrix3x3 &rotation, bool local)
{
  GetSGNode()->RelativeRotate(rotation, local);

  if (m_pPhysicsController) {  // (IsDynamic())
    m_pPhysicsController->RelativeRotate(rotation, local);
  }
  NodeUpdateGS(0.0f);
}

void KX_GameObject::UpdateBlenderObjectMatrix(blender::Object *blendobj)
{
  if (m_isUpbgeDupliInstance) {
    return;
  }
  if (!blendobj)
    blendobj = m_pBlenderObject;
  if (blendobj) {
    float object_to_world[4][4];
    NodeGetWorldTransform().getValue(&object_to_world[0][0]);
    copy_m4_m4(blendobj->runtime->object_to_world.ptr(), object_to_world);
    /* Making sure it's updated. (To move volumes) */
    invert_m4_m4(blendobj->runtime->world_to_object.ptr(), blendobj->object_to_world().ptr());
  }
}

void KX_GameObject::UpdateBuckets() {}

void KX_GameObject::RemoveMeshes()
{
  // note: meshes can be shared, and are deleted by BL_SceneConverter
  m_meshes.clear();
}

bool KX_GameObject::UseCulling() const
{
  return false;
}

void KX_GameObject::SetLodManager(KX_LodManager *lodManager)
{
  // Reset lod level to avoid overflow index in KX_LodManager::GetLevel.
  m_currentLodLevel = 0;

  // Restore object original mesh.
  if (!lodManager && m_lodManager && m_lodManager->GetLevelCount() > 0) {
    KX_Scene *scene = GetScene();
    RAS_MeshObject *origmesh = m_lodManager->GetLevel(0)->GetMesh();
    scene->ReplaceMesh(this, origmesh, true, false);
  }

  if (m_lodManager) {
    m_lodManager->Release();
  }

  m_lodManager = lodManager;

  if (m_lodManager) {
    m_lodManager->AddRef();
  }
}

KX_LodManager *KX_GameObject::GetLodManager() const
{
  return m_lodManager;
}

void KX_GameObject::UpdateLod(const float distance2)
{
  if (!m_lodManager) {
    return;
  }

  KX_Scene *scene = GetScene();
  KX_LodLevel *lodLevel = m_lodManager->GetLevel(scene, m_currentLodLevel, distance2);

  bool updatePhysicsShape = false;
  bool back_to_level0 = false;
  if (m_currentLodLevel != m_previousLodLevel) {
    if (GetBlenderObject()->gameflag & OB_LOD_UPDATE_PHYSICS) {
      if (GetPhysicsController()) {
        /* As m_previousLodLevel is initialized to -1,
         * the physics shape will be ensured on first frame
         * to match the lodLevel or the absence of lodLevel
         */
        updatePhysicsShape = true;
      }
    }
    if (m_currentLodLevel == 0) {
      back_to_level0 = true;
    }
    m_previousLodLevel = m_currentLodLevel;
  }

  if (lodLevel) {
    RAS_MeshObject *mesh = lodLevel->GetMesh();
    if (mesh != m_meshes[0]) {
      scene->ReplaceMesh(this, mesh, true, false);
    }
    m_currentLodLevel = lodLevel->GetLevel();
  }

  KX_LodLevel *currentLodLevel = m_lodManager->GetLevel(m_currentLodLevel);

  if (currentLodLevel) {
    blender::bContext *C = KX_GetActiveEngine()->GetContext();
    blender::Depsgraph *depsgraph = CTX_data_expect_evaluated_depsgraph(C);

    /* Here we want to change the object which will be rendered, then the evaluated object by the
     * depsgraph */
    blender::Object *ob_eval = DEG_get_evaluated(depsgraph, GetBlenderObject());

    blender::Object *lod_ob = currentLodLevel->GetObject();
    blender::Object *eval_lod_ob = lod_ob ? DEG_get_evaluated(depsgraph, lod_ob) : nullptr;
    /* Try to get the object with all modifiers applied */
    blender::Mesh *lod_mesh = eval_lod_ob ? (Mesh *)eval_lod_ob->runtime->data_eval : nullptr;
    blender::Mesh *copied_mesh = nullptr;
    if (lod_mesh == nullptr && eval_lod_ob != ob_eval) {
      /* The depsgraph skips objects that are never drawn (Disable in Renders, a hidden
       * collection), so a mesh borrowed from one has no evaluated version: draw a copy of it. */
      copied_mesh = lod_mesh = scene->GetReplaceMeshEvalMesh(currentLodLevel->GetMesh());
    }
    /* An object outside the depsgraph (a copy made this frame) is assigned on a later pass. */
    const bool can_assign = ob_eval != GetBlenderObject() &&
                            (back_to_level0 || lod_mesh != nullptr);
    if (can_assign && (back_to_level0 || (Mesh *)ob_eval->runtime->data_eval != lod_mesh)) {
      BKE_object_free_derived_caches(ob_eval);
      m_replaceMeshEval = nullptr;
      if (back_to_level0) {
        DEG_bump_update_count(depsgraph);
        ob_eval->runtime->last_update_geometry = DEG_get_update_count(depsgraph) + 1;
      }
      else {
        /* We assign lod_mesh data to ob_eval */
        BKE_object_eval_assign_data(ob_eval, &lod_mesh->id, false);
        m_replaceMeshEval = copied_mesh;
        /* Bump depsgraph count to avoid issues with TAA + shadows problems */
        DEG_bump_update_count(depsgraph);
        /* Never do that before UpdateDepsgraph, else last_update_geometry can still be < depsgraph
         * counter */
        ob_eval->runtime->last_update_geometry = DEG_get_update_count(depsgraph) + 1;
      }
    }
  }

  if (updatePhysicsShape) {
    /* If we come back to level 0, physics shape will be reinstanced from RAS_MeshObject
     * as ob_eval->data has been cleared (free derived caches), else, physics shape will
     * be reinstanced from assigned lodmesh. */
    GetPhysicsController()->ReinstancePhysicsShape(this, nullptr, false, true);
  }
}

void KX_GameObject::UpdateActivity(float distance)
{
  // Manage physics culling.
  if (m_activityCullingInfo.m_flags & ActivityCullingInfo::ACTIVITY_PHYSICS) {
    if (distance > m_activityCullingInfo.m_physicsRadius) {
      SuspendPhysics(false, false);
    }
    else {
      RestorePhysics(false);
    }
  }

  // Manage logic culling.
  if (m_activityCullingInfo.m_flags & ActivityCullingInfo::ACTIVITY_LOGIC) {
    if (distance > m_activityCullingInfo.m_logicRadius) {
      SuspendLogicAndActions(false);
    }
    else {
      RestoreLogicAndActions(false);
    }
  }
}

void KX_GameObject::UpdateTransform()
{
  // HACK: saves function call for dynamic object, they are handled differently
  if (m_pPhysicsController && !m_pPhysicsController->IsDynamic())
    m_pPhysicsController->SetTransform();
}

void KX_GameObject::UpdateTransformFunc(SG_Node * /*node*/, void *gameobj, void * /*scene*/)
{
  KX_GameObject *object = static_cast<KX_GameObject *>(gameobj);
  object->UpdateTransform();
}

void KX_GameObject::SynchronizeTransform()
{
  // only used for sensor object, do full synchronization as bullet doesn't do it
  if (m_pPhysicsController)
    m_pPhysicsController->SetTransform();
}

void KX_GameObject::SynchronizeTransformFunc(SG_Node *node, void *gameobj, void *scene)
{
  ((KX_GameObject *)gameobj)->SynchronizeTransform();
}

void KX_GameObject::InitIPO(bool ipo_as_force, bool ipo_add, bool ipo_local)
{
  SGControllerList::iterator it = GetSGNode()->GetSGControllerList().begin();

  while (it != GetSGNode()->GetSGControllerList().end()) {
    (*it)->SetOption(SG_Controller::SG_CONTR_IPO_RESET, true);
    (*it)->SetOption(SG_Controller::SG_CONTR_IPO_IPO_AS_FORCE, ipo_as_force);
    (*it)->SetOption(SG_Controller::SG_CONTR_IPO_IPO_ADD, ipo_add);
    (*it)->SetOption(SG_Controller::SG_CONTR_IPO_LOCAL, ipo_local);
    it++;
  }
}

void KX_GameObject::UpdateIPO(float curframetime, bool recurse)
{
  // just the 'normal' update procedure.
  GetSGNode()->SetSimulatedTimeThread(curframetime, recurse);
  GetSGNode()->UpdateWorldDataThread(curframetime);
}

bool KX_GameObject::GetVisible(void)
{
  return GetVisibleIgnoringViewClip() && m_viewClipVisible;
}

bool KX_GameObject::GetVisibleIgnoringViewClip() const
{
  return (m_bVisible || m_overlayVisible) && m_collectionVisible && m_presentationVisible;
}

static void setVisible_recursive(SG_Node *node, bool v)
{
  const NodeList &children = node->GetSGChildren();

  for (SG_Node *childnode : children) {
    KX_GameObject *clientgameobj = static_cast<KX_GameObject *>(childnode->GetSGClientObject());
    if (clientgameobj != nullptr)  // This is a GameObject
      clientgameobj->SetVisible(v, 0);

    // if the childobj is nullptr then this may be an inverse parent link
    // so a non recursive search should still look down this node.
    setVisible_recursive(childnode, v);
  }
}

void KX_GameObject::ApplyRuntimeVisibility()
{
  if (m_isUpbgeDupliInstance) {
    /* bge_dupli_provider skips a hidden instance: the renderers draw again when it changes (a
     * still frame kept showing it). */
    if (GetVisible() != m_dupliDrawnShown) {
      m_dupliDrawnShown = GetVisible();
      if (GetSGNode() != nullptr && GetScene() != nullptr) {
        TagUpbgeDupliInstanceForTaaReset();
      }
    }
    return;
  }

  blender::Object *ob = GetBlenderObject();
  if (ob) {
    const bool hidden = !GetVisible();
    KX_Scene *scene = GetScene();
    /* Merged: its part of the batch collapses or comes back; its own object stays hidden. */
    if (m_mergedBatch >= 0) {
      if (hidden != m_mergedPartHidden) {
        m_mergedPartHidden = hidden;
        scene->SetMergedPartShown(m_mergedBatch, m_mergedFirstVertex, m_mergedVertexCount, !hidden);
      }
      return;
    }
    if (m_drawHidden) {
      if (!hidden) {
        /* Shown again while still in Blender's graph: drawn at the next pass, nothing rebuilt. */
        scene->RemoveDrawHidden(this);
        scene->NoteDrawChanged(this);
      }
      return;
    }
    /* Already drawn or hidden so: nothing to sync. Logic that sets the same visibility every
     * frame (a Set Visible polling a property) made the scene resync all collections each frame
     * before 2026-10-15, 1.6 ms a frame in the split screen demo. */
    if (((ob->visibility_flag & OB_HIDE_VIEWPORT) != 0) == hidden) {
      return;
    }
    /* Hidden from the drawing only (each camera pass hides it, KX_Scene::HideCameraHiddenObjects):
     * leaving Blender's graph rebuilds all of it at the next pass (32 ms among 3,000 objects,
     * 205 ms among 10,000). Use Viewport Render draws without the camera passes' hiding. */
    if (hidden && !KX_GetActiveEngine()->UseViewportRender()) {
      scene->AddDrawHidden(this);
      scene->NoteDrawChanged(this);
      return;
    }
    SetGraphHidden(hidden);
  }
}

void KX_GameObject::SetGraphHidden(const bool hidden)
{
  blender::Object *ob = GetBlenderObject();
  GetScene()->TagForCollectionRemap();
  if (hidden) {
    ob->visibility_flag |= OB_HIDE_VIEWPORT;
  }
  else {
    ob->visibility_flag &= ~OB_HIDE_VIEWPORT;
  }
  GetScene()->AppendToIdsToUpdate(
      &ob->id, ID_RECALC_SYNC_TO_EVAL, ob->gameflag & OB_OVERLAY_COLLECTION);
}

void KX_GameObject::HideInGraph()
{
  KX_Scene *scene = GetScene();
  if (m_drawHidden && scene != nullptr) {
    scene->RemoveDrawHidden(this);
  }
  blender::Object *ob = GetBlenderObject();
  if (ob != nullptr && scene != nullptr && (ob->visibility_flag & OB_HIDE_VIEWPORT) == 0) {
    SetGraphHidden(true);
  }
}

void KX_GameObject::SetVisible(bool v, bool recursive)
{
  m_bVisible = v;
  /* An explicit hide must still win while the object is exposed by an overlay collection. The
   * scene can establish the overlay layer again on a later Set Overlay Collection request. */
  if (!v) {
    m_overlayVisible = false;
  }
  ApplyRuntimeVisibility();

  if (recursive) {
    setVisible_recursive(GetSGNode(), v);
  }
}

void KX_GameObject::SetCollectionVisible(const bool visible)
{
  if (m_collectionVisible == visible) {
    return;
  }
  m_collectionVisible = visible;
  ApplyRuntimeVisibility();
}

void KX_GameObject::SetOverlayVisible(const bool visible)
{
  if (m_overlayVisible == visible) {
    return;
  }
  m_overlayVisible = visible;
  ApplyRuntimeVisibility();
}

void KX_GameObject::SetPresentationVisible(const bool visible)
{
  if (m_presentationVisible == visible) {
    return;
  }
  m_presentationVisible = visible;
  ApplyRuntimeVisibility();
}

void KX_GameObject::SetViewClipVisible(const bool visible)
{
  if (m_viewClipVisible == visible) {
    return;
  }
  m_viewClipVisible = visible;
  ApplyRuntimeVisibility();
}

bool KX_GameObject::GetUIInteractable()
{
  if (m_uiInteractableOverride >= 0) {
    return m_uiInteractableOverride != 0;
  }
  const blender::Object *ob = GetBlenderObject();
  return ob == nullptr || (ob->game_ui.flag & OB_UI_DISABLED) == 0;
}

void KX_GameObject::SetUIInteractable(const bool interactable)
{
  m_uiInteractableOverride = interactable ? 1 : 0;
}

static void setOccluder_recursive(SG_Node *node, bool v)
{
  const NodeList &children = node->GetSGChildren();

  for (SG_Node *childnode : children) {
    KX_GameObject *clientgameobj = static_cast<KX_GameObject *>(childnode->GetSGClientObject());
    if (clientgameobj != nullptr)  // This is a GameObject
      clientgameobj->SetOccluder(v, false);

    // if the childobj is nullptr then this may be an inverse parent link
    // so a non recursive search should still look down this node.
    setOccluder_recursive(childnode, v);
  }
}

void KX_GameObject::SetOccluder(bool v, bool recursive)
{
  m_bOccluder = v;
  if (recursive)
    setOccluder_recursive(GetSGNode(), v);
}

static void setDebug_recursive(KX_Scene *scene, SG_Node *node, bool debug)
{
  const NodeList &children = node->GetSGChildren();

  for (SG_Node *childnode : children) {
    KX_GameObject *clientgameobj = static_cast<KX_GameObject *>(childnode->GetSGClientObject());
    if (clientgameobj != nullptr) {
      if (debug) {
        if (!scene->ObjectInDebugList(clientgameobj))
          scene->AddObjectDebugProperties(clientgameobj);
      }
      else
        scene->RemoveObjectDebugProperties(clientgameobj);
    }

    /* if the childobj is nullptr then this may be an inverse parent link
     * so a non recursive search should still look down this node. */
    setDebug_recursive(scene, childnode, debug);
  }
}

void KX_GameObject::SetUseDebugProperties(bool debug, bool recursive)
{
  KX_Scene *scene = GetScene();

  if (debug) {
    if (!scene->ObjectInDebugList(this))
      scene->AddObjectDebugProperties(this);
  }
  else
    scene->RemoveObjectDebugProperties(this);

  if (recursive)
    setDebug_recursive(scene, GetSGNode(), debug);
}

void KX_GameObject::SetLayer(int l)
{
  m_layer = l;
}

int KX_GameObject::GetLayer(void)
{
  return m_layer;
}

void KX_GameObject::addLinearVelocity(const MT_Vector3 &lin_vel, bool local)
{
  if (m_pPhysicsController) {
    MT_Vector3 lv = local ? NodeGetWorldOrientation() * lin_vel : lin_vel;
    m_pPhysicsController->SetLinearVelocity(lv + m_pPhysicsController->GetLinearVelocity(), 0);
  }
}

void KX_GameObject::setLinearVelocity(const MT_Vector3 &lin_vel, bool local)
{
  if (m_pPhysicsController)
    m_pPhysicsController->SetLinearVelocity(lin_vel, local);
}

void KX_GameObject::setAngularVelocity(const MT_Vector3 &ang_vel, bool local)
{
  if (m_pPhysicsController)
    m_pPhysicsController->SetAngularVelocity(ang_vel, local);
}

void KX_GameObject::SetObjectColor(const MT_Vector4 &rgbavec)
{
  if (MT_fuzzyEqual(m_objectColor, rgbavec)) {
    return;
  }

  m_objectColor = rgbavec;
  if (m_isUpbgeDupliInstance) {
    /* bge_dupli_provider draws the new color: the renderers draw again. */
    if (GetSGNode() != nullptr && GetScene() != nullptr) {
      TagUpbgeDupliInstanceForTaaReset();
    }
    return;
  }
  /* A batch draws one color: its own object shows the new one. */
  if (m_mergedBatch >= 0) {
    LeaveMergedBatch();
  }
  blender::Object *ob_orig = GetBlenderObject();
  if (ob_orig && GetScene()->OrigObCanBeTransformedInRealtime(ob_orig) &&
      ELEM(ob_orig->type, OB_MESH, OB_CURVES_LEGACY, OB_SURF, OB_FONT, OB_MBALL))
  {
    copy_v4_v4(ob_orig->color, m_objectColor.getValue());
    DEG_id_tag_update(&ob_orig->id, ID_RECALC_SHADING | ID_RECALC_TRANSFORM);
    WM_main_add_notifier(NC_OBJECT | ND_DRAW, &ob_orig->id);
  }
}

const MT_Vector4 &KX_GameObject::GetObjectColor()
{
  return m_objectColor;
}

void KX_GameObject::SetConvertedColor(const MT_Vector4 &rgbavec)
{
  m_convertedColor = rgbavec;
  m_hasConvertedColor = true;
}

void KX_GameObject::AlignAxisToVect(const MT_Vector3 &dir, int axis, float fac)
{
  const MT_Scalar eps = 3.0f * MT_EPSILON;
  MT_Matrix3x3 orimat;
  MT_Vector3 vect, ori, z, x, y;
  MT_Scalar len;

  vect = dir;
  len = vect.length();
  if (MT_fuzzyZero(len)) {
    CM_FunctionError("null vector!");
    return;
  }

  if (fac <= 0.0f) {
    return;
  }

  // normalize
  vect /= len;
  orimat = GetSGNode()->GetWorldOrientation();
  switch (axis) {
    case 0:  // align x axis of new coord system to vect
      ori.setValue(orimat[0][2], orimat[1][2], orimat[2][2]);    // pivot axis
      if (1.0f - MT_abs(vect.dot(ori)) < eps) {                  // vect parallel to pivot?
        ori.setValue(orimat[0][1], orimat[1][1], orimat[2][1]);  // change the pivot!
      }

      if (fac == 1.0f) {
        x = vect;
      }
      else {
        x = (vect * fac) + ((orimat * MT_Vector3(1.0f, 0.0f, 0.0f)) * (1.0f - fac));
        len = x.length();
        if (MT_fuzzyZero(len))
          x = vect;
        else
          x /= len;
      }
      y = ori.cross(x);
      z = x.cross(y);
      break;
    case 1:  // y axis
      ori.setValue(orimat[0][0], orimat[1][0], orimat[2][0]);
      if (1.0f - MT_abs(vect.dot(ori)) < eps) {
        ori.setValue(orimat[0][2], orimat[1][2], orimat[2][2]);
      }

      if (fac == 1.0f) {
        y = vect;
      }
      else {
        y = (vect * fac) + ((orimat * MT_Vector3(0.0f, 1.0f, 0.0f)) * (1.0f - fac));
        len = y.length();
        if (MT_fuzzyZero(len))
          y = vect;
        else
          y /= len;
      }
      z = ori.cross(y);
      x = y.cross(z);
      break;
    case 2:  // z axis
      ori.setValue(orimat[0][1], orimat[1][1], orimat[2][1]);
      if (1.0f - MT_abs(vect.dot(ori)) < eps) {
        ori.setValue(orimat[0][0], orimat[1][0], orimat[2][0]);
      }

      if (fac == 1.0f) {
        z = vect;
      }
      else {
        z = (vect * fac) + ((orimat * MT_Vector3(0.0f, 0.0f, 1.0f)) * (1.0f - fac));
        len = z.length();
        if (MT_fuzzyZero(len))
          z = vect;
        else
          z /= len;
      }
      x = ori.cross(z);
      y = z.cross(x);
      break;
    default:  // invalid axis specified
      CM_FunctionWarning("invalid axis '" << axis << "'");
      return;
  }
  x.normalize();  // normalize the new base vectors
  y.normalize();
  z.normalize();
  orimat.setValue(x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2]);

  if (GetSGNode()->GetSGParent() != nullptr) {
    // the object is a child, adapt its local orientation so that
    // the global orientation is aligned as we want (cancelling out the parent orientation)
    MT_Matrix3x3 invori = GetSGNode()->GetSGParent()->GetWorldOrientation().inverse();
    NodeSetLocalOrientation(invori * orimat);
  }
  else {
    NodeSetLocalOrientation(orimat);
  }
}

MT_Scalar KX_GameObject::GetMass()
{
  if (m_pPhysicsController) {
    return m_pPhysicsController->GetMass();
  }
  return 0.0f;
}

MT_Vector3 KX_GameObject::GetLocalInertia()
{
  MT_Vector3 local_inertia(0.0f, 0.0f, 0.0f);
  if (m_pPhysicsController) {
    local_inertia = m_pPhysicsController->GetLocalInertia();
  }
  return local_inertia;
}

MT_Vector3 KX_GameObject::GetLinearVelocity(bool local)
{
  MT_Vector3 velocity(0.0f, 0.0f, 0.0f), locvel;
  MT_Matrix3x3 ori;
  if (m_pPhysicsController) {
    velocity = m_pPhysicsController->GetLinearVelocity();

    if (local) {
      ori = GetSGNode()->GetWorldOrientation();

      locvel = velocity * ori;
      return locvel;
    }
  }
  return velocity;
}

MT_Vector3 KX_GameObject::GetAngularVelocity(bool local)
{
  MT_Vector3 velocity(0.0f, 0.0f, 0.0f), locvel;
  MT_Matrix3x3 ori;
  if (m_pPhysicsController) {
    velocity = m_pPhysicsController->GetAngularVelocity();

    if (local) {
      ori = GetSGNode()->GetWorldOrientation();

      locvel = velocity * ori;
      return locvel;
    }
  }
  return velocity;
}

MT_Vector3 KX_GameObject::GetGravity()
{
  MT_Vector3 gravity(0.0f, 0.0f, 0.0f);
  if (m_pPhysicsController) {
    gravity = m_pPhysicsController->GetGravity();
    return gravity;
  }
  return gravity;
}

void KX_GameObject::SetGravity(const MT_Vector3 &gravity)
{
  if (m_pPhysicsController) {
    m_pPhysicsController->SetGravity(gravity);
  }
}

MT_Vector3 KX_GameObject::GetVelocity(const MT_Vector3 &point)
{
  if (m_pPhysicsController) {
    return m_pPhysicsController->GetVelocity(point);
  }
  return MT_Vector3(0.0f, 0.0f, 0.0f);
}

// scenegraph node stuff

void KX_GameObject::NodeSetLocalPosition(const MT_Vector3 &trans)
{
  if (m_pPhysicsController && !GetSGNode()->GetSGParent()) {
    // don't update physic controller if the object is a child:
    // 1) the transformation will not be right
    // 2) in this case, the physic controller is necessarily a static object
    //    that is updated from the normal kinematic synchronization
    m_pPhysicsController->SetPosition(trans);
  }

  GetSGNode()->SetLocalPosition(trans);
}

void KX_GameObject::NodeSetLocalOrientation(const MT_Matrix3x3 &rot)
{
  if (m_pPhysicsController && !GetSGNode()->GetSGParent()) {
    // see note above
    m_pPhysicsController->SetOrientation(rot);
  }
  GetSGNode()->SetLocalOrientation(rot);
}

void KX_GameObject::NodeSetGlobalOrientation(const MT_Matrix3x3 &rot)
{
  if (GetSGNode()->GetSGParent())
    GetSGNode()->SetLocalOrientation(GetSGNode()->GetSGParent()->GetWorldOrientation().inverse() *
                                     rot);
  else
    NodeSetLocalOrientation(rot);
}

void KX_GameObject::NodeSetLocalScale(const MT_Vector3 &scale)
{
  if (m_pPhysicsController && !GetSGNode()->GetSGParent()) {
    // see note above
    m_pPhysicsController->SetScaling(scale);
  }
  GetSGNode()->SetLocalScale(scale);
}

void KX_GameObject::NodeSetRelativeScale(const MT_Vector3 &scale)
{
  GetSGNode()->RelativeScale(scale);
  if (m_pPhysicsController && (!GetSGNode()->GetSGParent())) {
    // see note above
    // we can use the local scale: it's the same thing for a root object
    // and the world scale is not yet updated
    MT_Vector3 newscale = GetSGNode()->GetLocalScale();
    m_pPhysicsController->SetScaling(newscale);
  }
}

void KX_GameObject::NodeSetWorldScale(const MT_Vector3 &scale)
{
  SG_Node *parent = GetSGNode()->GetSGParent();
  if (parent != nullptr) {
    // Make sure the objects have some scale
    MT_Vector3 p_scale = parent->GetWorldScaling();
    if (fabs(p_scale[0]) < (MT_Scalar)FLT_EPSILON || fabs(p_scale[1]) < (MT_Scalar)FLT_EPSILON ||
        fabs(p_scale[2]) < (MT_Scalar)FLT_EPSILON)
    {
      return;
    }

    p_scale[0] = 1 / p_scale[0];
    p_scale[1] = 1 / p_scale[1];
    p_scale[2] = 1 / p_scale[2];

    NodeSetLocalScale(scale * p_scale);
  }
  else {
    NodeSetLocalScale(scale);
  }
}

void KX_GameObject::NodeSetWorldPosition(const MT_Vector3 &trans)
{
  SG_Node *parent = m_pSGNode->GetSGParent();
  if (parent != nullptr) {
    // Make sure the objects have some scale
    MT_Vector3 scale = parent->GetWorldScaling();
    if (fabs(scale[0]) < (MT_Scalar)FLT_EPSILON || fabs(scale[1]) < (MT_Scalar)FLT_EPSILON ||
        fabs(scale[2]) < (MT_Scalar)FLT_EPSILON)
    {
      return;
    }
    scale[0] = 1.0f / scale[0];
    scale[1] = 1.0f / scale[1];
    scale[2] = 1.0f / scale[2];
    MT_Matrix3x3 invori = parent->GetWorldOrientation().inverse();
    MT_Vector3 newpos = invori * (trans - parent->GetWorldPosition()) * scale;
    NodeSetLocalPosition(MT_Vector3(newpos[0], newpos[1], newpos[2]));
  }
  else {
    NodeSetLocalPosition(trans);
  }
}

void KX_GameObject::NodeUpdateGS(double time)
{
  m_pSGNode->UpdateWorldData(time);
}

const MT_Matrix3x3 &KX_GameObject::NodeGetWorldOrientation() const
{
  return m_pSGNode->GetWorldOrientation();
}

const MT_Matrix3x3 &KX_GameObject::NodeGetLocalOrientation() const
{
  return m_pSGNode->GetLocalOrientation();
}

const MT_Vector3 &KX_GameObject::NodeGetWorldScaling() const
{
  return m_pSGNode->GetWorldScaling();
}

const MT_Vector3 &KX_GameObject::NodeGetLocalScaling() const
{
  return m_pSGNode->GetLocalScale();
}

const MT_Vector3 &KX_GameObject::NodeGetWorldPosition() const
{
  return m_pSGNode->GetWorldPosition();
}

const MT_Vector3 &KX_GameObject::NodeGetLocalPosition() const
{
  return m_pSGNode->GetLocalPosition();
}

MT_Transform KX_GameObject::NodeGetWorldTransform() const
{
  return m_pSGNode->GetWorldTransform();
}

MT_Transform KX_GameObject::NodeGetInterpolatedTransform(double alpha) const
{
  MT_Vector3 position;
  MT_Matrix3x3 orientation;
  MT_Vector3 scale;
  GetPhysicsInterpolatedPose(alpha, position, orientation, scale);
  return MT_Transform(position, orientation.scaled(scale[0], scale[1], scale[2]));
}

MT_Transform KX_GameObject::NodeGetLocalTransform() const
{
  return m_pSGNode->GetLocalTransform();
}

void KX_GameObject::StorePhysicsInterpolationState()
{
  if (m_pSGNode != nullptr) {
    /* Cache the current physics pose so render interpolation can blend from it later. */
    m_pSGNode->StorePreviousWorldTransform();
    m_useRenderInterpolation = false;
  }
}

void KX_GameObject::RebasePhysicsInterpolationState()
{
  if (m_pSGNode != nullptr) {
    /* A render-frame transform is authoritative immediately. Interpolating it against the
     * preceding fixed pose would introduce a one-frame rollback or partial movement. */
    m_pSGNode->StorePreviousWorldTransform();
  }
}

void KX_GameObject::GetPhysicsInterpolatedPose(double alpha,
                                               MT_Vector3 &r_position,
                                               MT_Matrix3x3 &r_orientation,
                                               MT_Vector3 &r_scale) const
{
  if (!m_pSGNode->HasPreviousWorldTransform()) {
    r_position = m_pSGNode->GetWorldPosition();
    r_orientation = m_pSGNode->GetWorldOrientation();
    r_scale = m_pSGNode->GetWorldScaling();
    return;
  }

  /* Never allow an invalid public alpha value to poison the authoritative scene graph. */
  const double finiteAlpha = std::isfinite(alpha) ? alpha : 1.0;
  const MT_Scalar t = MT_Scalar(std::clamp(finiteAlpha, 0.0, 1.0));
  const MT_Scalar oneMinusT = MT_Scalar(1.0f) - t;

  r_position = (m_pSGNode->GetPrevWorldPosition() * oneMinusT) +
               (m_pSGNode->GetWorldPosition() * t);
  r_scale = (m_pSGNode->GetPrevWorldScaling() * oneMinusT) + (m_pSGNode->GetWorldScaling() * t);

  const MT_Quaternion previous = m_pSGNode->GetPrevWorldOrientation().getRotation();
  const MT_Quaternion current = m_pSGNode->GetWorldOrientation().getRotation();
  r_orientation = MT_Matrix3x3(previous.slerp(current, t));
}

void KX_GameObject::GetPhysicsExtrapolatedPose(double alpha,
                                               MT_Vector3 &r_position,
                                               MT_Matrix3x3 &r_orientation) const
{
  r_position = m_pSGNode->GetWorldPosition();
  r_orientation = m_pSGNode->GetWorldOrientation();
  if (!m_pSGNode->HasPreviousWorldTransform()) {
    return;
  }

  const double finiteAlpha = std::isfinite(alpha) ? alpha : 0.0;
  const MT_Scalar t = MT_Scalar(std::clamp(finiteAlpha, 0.0, 1.0));
  r_position += (r_position - m_pSGNode->GetPrevWorldPosition()) * t;

  const MT_Quaternion previous = m_pSGNode->GetPrevWorldOrientation().getRotation();
  const MT_Quaternion current = r_orientation.getRotation();
  r_orientation = MT_Matrix3x3(previous.slerp(current, MT_Scalar(1.0f) + t));
}

void KX_GameObject::ApplyPhysicsInterpolation(double alpha)
{
  SmoothMotionFrame frame;
  frame.interpolation = alpha;
  ApplySmoothMotion(
      frame, OB_SMOOTH_MOTION_INTERPOLATE, SmoothMotionFollow(), SmoothMotionReset(), nullptr);
}

static bool same_vector(const MT_Vector3 &a, const MT_Vector3 &b)
{
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

static bool same_matrix(const MT_Matrix3x3 &a, const MT_Matrix3x3 &b)
{
  return same_vector(a[0], b[0]) && same_vector(a[1], b[1]) && same_vector(a[2], b[2]);
}

void KX_GameObject::SetSmoothMotion(const char mode)
{
  m_smoothMotion = std::clamp<char>(mode, OB_SMOOTH_MOTION_SCENE, OB_SMOOTH_MOTION_EXTRAPOLATE);
  if (ELEM(m_smoothMotion, OB_SMOOTH_MOTION_INTERPOLATE, OB_SMOOTH_MOTION_EXTRAPOLATE)) {
    if (KX_Scene *scene = m_pSGNode ? GetScene() : nullptr) {
      scene->NoteSmoothMotion();
    }
  }
}

void KX_GameObject::ResetSmoothing(const float blend_seconds)
{
  m_smoothResetPending = true;
  m_smoothResetBlend = std::isfinite(blend_seconds) ? std::max(0.0f, blend_seconds) : 0.0f;
}

KX_GameObject::SmoothMotionReset KX_GameObject::TakeSmoothMotionReset()
{
  SmoothMotionReset reset;
  reset.active = m_smoothResetPending;
  reset.blend = m_smoothResetBlend;
  m_smoothResetPending = false;
  return reset;
}

void KX_GameObject::StoreSmoothMotionHistory(const bool smoothed)
{
  if (m_pSGNode == nullptr) {
    return;
  }
  if (smoothed) {
    m_pSGNode->StorePreviousWorldTransform();
  }
  else if (m_pSGNode->HasPreviousWorldTransform()) {
    m_pSGNode->ClearPreviousWorldTransform();
  }
  /* The step starts from where a snapped jump ended. A blended one waits for the next drawn
   * frame, which fades it from where the last frame drew the object. */
  if (m_smoothResetPending && m_smoothResetBlend <= 0.0f) {
    m_smoothResetPending = false;
  }
}

KX_GameObject::SmoothMotionFollow KX_GameObject::ApplySmoothMotion(
    const SmoothMotionFrame &frame,
    const char mode,
    const SmoothMotionFollow &parent,
    const SmoothMotionReset &reset,
    KX_Scene *scene)
{
  SmoothMotionFollow follow;
  if (m_pSGNode == nullptr) {
    return follow;
  }
  if (m_useRenderInterpolation) {
    /* Drawn twice in one frame: start again from the real pose. */
    ClearPhysicsInterpolationState();
  }

  const MT_Vector3 position = m_pSGNode->GetWorldPosition();
  const MT_Matrix3x3 orientation = m_pSGNode->GetWorldOrientation();
  const MT_Vector3 scale = m_pSGNode->GetWorldScaling();
  const bool smoothed = ELEM(mode, OB_SMOOTH_MOTION_INTERPOLATE, OB_SMOOTH_MOTION_EXTRAPOLATE);

  if (reset.active && m_pSGNode->HasPreviousWorldTransform()) {
    /* A jump: no step history across it, for the frames until the next step too. */
    m_pSGNode->StorePreviousWorldTransform();
  }

  MT_Vector3 drawn_position = position;
  MT_Matrix3x3 drawn_orientation = orientation;
  MT_Vector3 drawn_scale = scale;
  if (smoothed && !reset.active && m_pSGNode->HasPreviousWorldTransform()) {
    /* Still since the last step: drawn where it is, exactly (no work, no write). */
    const bool moved = !same_vector(m_pSGNode->GetPrevWorldPosition(), position) ||
                       !same_matrix(m_pSGNode->GetPrevWorldOrientation(), orientation) ||
                       !same_vector(m_pSGNode->GetPrevWorldScaling(), scale);
    if (moved && mode == OB_SMOOTH_MOTION_INTERPOLATE) {
      GetPhysicsInterpolatedPose(
          frame.interpolation, drawn_position, drawn_orientation, drawn_scale);
    }
    else if (moved) {
      GetPhysicsExtrapolatedPose(frame.interpolation, drawn_position, drawn_orientation);
    }
  }
  else if (parent.moved) {
    /* No smoothing of its own (None, no step history yet, a jump): drawn with its parent, so
     * what is attached to a smoothed object never shakes against it. */
    drawn_position = parent.rotation * position + parent.translation;
    drawn_orientation = parent.rotation * orientation;
  }

  if (reset.active) {
    m_smoothCorrectionLeft = 0.0f;
    if (reset.blend > 0.0f && smoothed && m_smoothDrawnFrame != 0 &&
        m_smoothDrawnFrame + 1 == frame.number)
    {
      /* A blended jump: drawn where the last frame showed it, then slides to its pose. */
      m_smoothCorrectionOffset = m_smoothDrawnPosition - drawn_position;
      m_smoothCorrectionTurn =
          (m_smoothDrawnOrientation * drawn_orientation.transposed()).getRotation();
      m_smoothCorrectionDuration = reset.blend;
      m_smoothCorrectionLeft = reset.blend;
    }
  }
  else if (m_smoothCorrectionLeft > 0.0f) {
    m_smoothCorrectionLeft -= float(std::max(0.0, frame.seconds));
  }
  if (m_smoothCorrectionLeft > 0.0f && m_smoothCorrectionDuration > 0.0f) {
    const float t = std::min(1.0f, m_smoothCorrectionLeft / m_smoothCorrectionDuration);
    const MT_Scalar weight = t * t * (3.0f - 2.0f * t);
    drawn_position += m_smoothCorrectionOffset * weight;
    const MT_Quaternion still(0.0f, 0.0f, 0.0f, 1.0f);
    drawn_orientation = MT_Matrix3x3(still.slerp(m_smoothCorrectionTurn, weight)) *
                        drawn_orientation;
  }
  else {
    m_smoothCorrectionLeft = 0.0f;
  }

  if (smoothed) {
    m_smoothDrawnPosition = drawn_position;
    m_smoothDrawnOrientation = drawn_orientation;
    m_smoothDrawnFrame = frame.number;
  }

  if (same_vector(drawn_position, position) && same_matrix(drawn_orientation, orientation) &&
      same_vector(drawn_scale, scale))
  {
    return follow;
  }

  /* Register before changing the node: a failed registration can't leave a drawn pose behind
   * that nothing restores. */
  if (scene != nullptr) {
    scene->RegisterInterpolatedObject(this);
  }
  /* Preserve the exact TRS components. Decomposing an MT_Transform later would lose negative
   * scale signs and can change reflected objects when interpolation is merely toggled. */
  m_cachedRenderPosition = position;
  m_cachedRenderOrientation = orientation;
  m_cachedRenderScale = scale;
  m_cachedInterpolatedTransform = MT_Transform(
      drawn_position, drawn_orientation.scaled(drawn_scale[0], drawn_scale[1], drawn_scale[2]));
  m_pSGNode->SetWorldPosition(drawn_position);
  m_pSGNode->SetWorldOrientation(drawn_orientation);
  m_pSGNode->SetWorldScale(drawn_scale);
  m_useRenderInterpolation = true;

  if (!m_pSGNode->GetSGChildren().empty()) {
    follow.moved = true;
    follow.rotation = drawn_orientation * orientation.transposed();
    follow.translation = drawn_position - follow.rotation * position;
  }
  return follow;
}

void KX_GameObject::ClearPhysicsInterpolationState()
{
  if (m_pSGNode != nullptr) {
    if (m_useRenderInterpolation) {
      /* Restore the physics pose captured before interpolation modified the SG node. */
      m_pSGNode->SetWorldPosition(m_cachedRenderPosition);
      m_pSGNode->SetWorldOrientation(m_cachedRenderOrientation);
      m_pSGNode->SetWorldScale(m_cachedRenderScale);
    }
    m_useRenderInterpolation = false;
    m_cachedInterpolatedTransform = MT_Transform::Identity();
  }
}

void KX_GameObject::UnregisterCollisionCallbacks()
{
  if (!GetPhysicsController()) {
    CM_Warning(
        "trying to unregister collision callbacks for object without collisions: " << GetName());
    return;
  }

  // Unregister from callbacks
  KX_Scene *scene = GetScene();
  PHY_IPhysicsEnvironment *pe = scene->GetPhysicsEnvironment();
  PHY_IPhysicsController *spc = GetPhysicsController();
  // If we are the last to unregister on this physics controller
  if (pe->RemoveCollisionCallback(spc)) {
    // If we are a sensor object
    if (m_pClient_info->isSensor())
      // Remove sensor body from physics world
      pe->RemoveSensor(spc);
  }
}

void KX_GameObject::RegisterCollisionCallbacks()
{
  if (!GetPhysicsController()) {
    CM_Warning(
        "trying to register collision callbacks for object without collisions: " << GetName());
    return;
  }

  // Register from callbacks
  KX_Scene *scene = GetScene();
  PHY_IPhysicsEnvironment *pe = scene->GetPhysicsEnvironment();
  PHY_IPhysicsController *spc = GetPhysicsController();
  // If we are the first to register on this physics controller
  if (pe->RequestCollisionCallback(spc)) {
    // If we are a sensor object
    if (m_pClient_info->isSensor())
      // Add sensor body to physics world
      pe->AddSensor(spc);
  }
}
void KX_GameObject::RunCollisionCallbacks(KX_GameObject *collider,
                                          KX_CollisionContactPointList &contactPointList)
{
#ifdef WITH_PYTHON
  if (!m_collisionCallbacks || PyList_GET_SIZE(m_collisionCallbacks) == 0) {
    return;
  }

  const PHY_ICollData *collData = contactPointList.GetCollData();
  const bool isFirstObject = contactPointList.GetFirstObject();

  PyObject *args[] = {collider->GetProxy(),
                      PyObjectFrom(collData->GetWorldPoint(0, isFirstObject)),
                      PyObjectFrom(collData->GetNormal(0, isFirstObject)),
                      contactPointList.GetProxy()};
  EXP_RunPythonCallBackList(m_collisionCallbacks, args, 1, ARRAY_SIZE(args));

  for (unsigned int i = 0; i < ARRAY_SIZE(args); ++i) {
    Py_DECREF(args[i]);
  }

  // Invalidate the collison contact point to avoid acces to it in next frame
  contactPointList.InvalidateProxy();
#endif
}

void KX_GameObject::RunOnRemoveCallbacks()
{
#ifdef WITH_PYTHON
  PyObject *list = m_removeCallbacks;
  if (!list || PyList_GET_SIZE(list) == 0) {
    return;
  }

  PyObject *args[1] = {GetProxy()};
  EXP_RunPythonCallBackList(list, args, 0, 1);
#endif  // WITH PYTHON
}

template<bool recursive>
static void walk_children(const SG_Node *node, std::vector<KX_GameObject *> &list)
{
  if (!node) {
    return;
  }
  const NodeList &children = node->GetSGChildren();

  for (SG_Node *childnode : children) {
    KX_GameObject *childobj = static_cast<KX_GameObject *>(childnode->GetSGClientObject());
    if (childobj) {
      list.push_back(childobj);
    }

    /* if the childobj is nullptr then this may be an inverse parent link
     * so a non recursive search should still look down this node. */
    if (recursive || !childobj) {
      walk_children<recursive>(childnode, list);
    }
  }
}

std::vector<KX_GameObject *> KX_GameObject::GetChildren() const
{
  std::vector<KX_GameObject *> list;
  // GetSGNode() is always valid or it would have raised an exception before this.
  walk_children<false>(GetSGNode(), list);
  return list;
}

std::vector<KX_GameObject *> KX_GameObject::GetChildrenRecursive() const
{
  std::vector<KX_GameObject *> list;
  walk_children<true>(GetSGNode(), list);
  return list;
}

EXP_ListValue<KX_PythonComponent> *KX_GameObject::GetComponents() const
{
#ifdef WITH_PYTHON
  return m_components;
#else
  return nullptr;
#endif
}

void KX_GameObject::SetComponents(EXP_ListValue<KX_PythonComponent> *components)
{
#ifdef WITH_PYTHON
  m_components = components;
#endif
}

void KX_GameObject::Update()
{
#ifdef WITH_PYTHON
  if (!m_logicSuspended) {
    if (m_components) {
      for (KX_PythonComponent *comp : m_components) {
        comp->Update();
      }
    }

    KX_PythonProxy::Update();
  }
#endif  // WITH_PYTHON
}

KX_Scene *KX_GameObject::GetScene()
{
  BLI_assert(m_pSGNode);
  return static_cast<KX_Scene *>(m_pSGNode->GetSGClientInfo());
}

void KX_GameObject::SetScene(KX_Scene *scene)
{
  BLI_assert(!m_pSGNode);

  m_pSGNode = new SG_Node(this, scene, KX_Scene::m_callbacks);

  // define the relationship between this node and it's parent.

  KX_NormalParentRelation *parent_relation = new KX_NormalParentRelation();
  m_pSGNode->SetParentRelation(parent_relation);
}

/* ---------------------------------------------------------------------
 * Some stuff taken from the header
 * --------------------------------------------------------------------- */
void KX_GameObject::Relink(std::map<SCA_IObject *, SCA_IObject *> &map_parameter)
{
  // we will relink the sensors and actuators that use object references
  // if the object is part of the replicated hierarchy, use the new
  // object reference instead
  SCA_SensorList &sensorlist = GetSensors();
  SCA_SensorList::iterator sit;
  for (sit = sensorlist.begin(); sit != sensorlist.end(); sit++) {
    (*sit)->Relink(map_parameter);
  }
  SCA_ActuatorList &actuatorlist = GetActuators();
  SCA_ActuatorList::iterator ait;
  for (ait = actuatorlist.begin(); ait != actuatorlist.end(); ait++) {
    (*ait)->Relink(map_parameter);
  }
  RelinkObjectProperties(map_parameter);
}

void KX_GameObject::RelinkObjectProperties(const std::map<SCA_IObject *, SCA_IObject *> &copies)
{
  for (int index = 0; index < GetPropertyCount(); index++) {
    KX_RelinkObjectReferences(GetProperty(index), copies);
  }
}

#ifdef WITH_PYTHON

#  define PYTHON_CHECK_PHYSICS_CONTROLLER(obj, attr, ret) \
    if (!(obj)->GetPhysicsController()) { \
      PyErr_Format( \
          PyExc_AttributeError, "KX_GameObject.%s, is missing a physics controller", (attr)); \
      return (ret); \
    }

#endif

#ifdef USE_MATHUTILS

/* These require an SGNode */
#  define MATHUTILS_VEC_CB_POS_LOCAL 1
#  define MATHUTILS_VEC_CB_POS_GLOBAL 2
#  define MATHUTILS_VEC_CB_SCALE_LOCAL 3
#  define MATHUTILS_VEC_CB_SCALE_GLOBAL 4
#  define MATHUTILS_VEC_CB_INERTIA_LOCAL 5
#  define MATHUTILS_VEC_CB_OBJECT_COLOR 6
#  define MATHUTILS_VEC_CB_LINVEL_LOCAL 7
#  define MATHUTILS_VEC_CB_LINVEL_GLOBAL 8
#  define MATHUTILS_VEC_CB_ANGVEL_LOCAL 9
#  define MATHUTILS_VEC_CB_ANGVEL_GLOBAL 10
#  define MATHUTILS_VEC_CB_GRAVITY 11

static unsigned char mathutils_kxgameob_vector_cb_index = -1; /* index for our callbacks */

static int mathutils_kxgameob_generic_check(blender::BaseMathObject *bmo)
{
  KX_GameObject *self = static_cast<KX_GameObject *> EXP_PROXY_REF(bmo->cb_user);
  if (self == nullptr)
    return -1;

  return 0;
}

static int mathutils_kxgameob_vector_get(blender::BaseMathObject *bmo, int subtype)
{
  KX_GameObject *self = static_cast<KX_GameObject *> EXP_PROXY_REF(bmo->cb_user);
  if (self == nullptr)
    return -1;

  switch (subtype) {
    case MATHUTILS_VEC_CB_POS_LOCAL:
      self->NodeGetLocalPosition().getValue(bmo->data);
      break;
    case MATHUTILS_VEC_CB_POS_GLOBAL:
      self->NodeGetWorldPosition().getValue(bmo->data);
      break;
    case MATHUTILS_VEC_CB_SCALE_LOCAL:
      self->NodeGetLocalScaling().getValue(bmo->data);
      break;
    case MATHUTILS_VEC_CB_SCALE_GLOBAL:
      self->NodeGetWorldScaling().getValue(bmo->data);
      break;
    case MATHUTILS_VEC_CB_INERTIA_LOCAL:
      PYTHON_CHECK_PHYSICS_CONTROLLER(self, "localInertia", -1);
      self->GetPhysicsController()->GetLocalInertia().getValue(bmo->data);
      break;
    case MATHUTILS_VEC_CB_OBJECT_COLOR:
      self->GetObjectColor().getValue(bmo->data);
      break;
    case MATHUTILS_VEC_CB_LINVEL_LOCAL:
      PYTHON_CHECK_PHYSICS_CONTROLLER(self, "localLinearVelocity", -1);
      self->GetLinearVelocity(true).getValue(bmo->data);
      break;
    case MATHUTILS_VEC_CB_LINVEL_GLOBAL:
      PYTHON_CHECK_PHYSICS_CONTROLLER(self, "worldLinearVelocity", -1);
      self->GetLinearVelocity(false).getValue(bmo->data);
      break;
    case MATHUTILS_VEC_CB_ANGVEL_LOCAL:
      PYTHON_CHECK_PHYSICS_CONTROLLER(self, "localLinearVelocity", -1);
      self->GetAngularVelocity(true).getValue(bmo->data);
      break;
    case MATHUTILS_VEC_CB_ANGVEL_GLOBAL:
      PYTHON_CHECK_PHYSICS_CONTROLLER(self, "worldLinearVelocity", -1);
      self->GetAngularVelocity(false).getValue(bmo->data);
      break;
    case MATHUTILS_VEC_CB_GRAVITY:
      PYTHON_CHECK_PHYSICS_CONTROLLER(self, "gravity", -1);
      self->GetGravity().getValue(bmo->data);
      break;
  }

#  undef PHYS_ERR

  return 0;
}

static int mathutils_kxgameob_vector_set(blender::BaseMathObject *bmo, int subtype)
{
  KX_GameObject *self = static_cast<KX_GameObject *> EXP_PROXY_REF(bmo->cb_user);
  if (self == nullptr)
    return -1;

  switch (subtype) {
    case MATHUTILS_VEC_CB_POS_LOCAL:
      self->NodeSetLocalPosition(MT_Vector3(bmo->data));
      self->NodeUpdateGS(0.f);
      break;
    case MATHUTILS_VEC_CB_POS_GLOBAL:
      self->NodeSetWorldPosition(MT_Vector3(bmo->data));
      self->NodeUpdateGS(0.f);
      break;
    case MATHUTILS_VEC_CB_SCALE_LOCAL:
      self->NodeSetLocalScale(MT_Vector3(bmo->data));
      self->NodeUpdateGS(0.f);
      break;
    case MATHUTILS_VEC_CB_SCALE_GLOBAL:
      self->NodeSetWorldScale(MT_Vector3(bmo->data));
      self->NodeUpdateGS(0.0f);
      break;
    case MATHUTILS_VEC_CB_INERTIA_LOCAL:
      /* read only */
      break;
    case MATHUTILS_VEC_CB_OBJECT_COLOR:
      self->SetObjectColor(MT_Vector4(bmo->data));
      break;
    case MATHUTILS_VEC_CB_LINVEL_LOCAL:
      self->setLinearVelocity(MT_Vector3(bmo->data), true);
      break;
    case MATHUTILS_VEC_CB_LINVEL_GLOBAL:
      self->setLinearVelocity(MT_Vector3(bmo->data), false);
      break;
    case MATHUTILS_VEC_CB_ANGVEL_LOCAL:
      self->setAngularVelocity(MT_Vector3(bmo->data), true);
      break;
    case MATHUTILS_VEC_CB_ANGVEL_GLOBAL:
      self->setAngularVelocity(MT_Vector3(bmo->data), false);
      break;
    case MATHUTILS_VEC_CB_GRAVITY:
      self->SetGravity(MT_Vector3(bmo->data));
      break;
  }

  return 0;
}

static int mathutils_kxgameob_vector_get_index(blender::BaseMathObject *bmo,
                                               int subtype,
                                               int index)
{
  /* lazy, avoid repeteing the case statement */
  if (mathutils_kxgameob_vector_get(bmo, subtype) == -1)
    return -1;
  return 0;
}

static int mathutils_kxgameob_vector_set_index(blender::BaseMathObject *bmo,
                                               int subtype,
                                               int index)
{
  float f = bmo->data[index];

  /* lazy, avoid repeteing the case statement */
  if (mathutils_kxgameob_vector_get(bmo, subtype) == -1)
    return -1;

  bmo->data[index] = f;
  return mathutils_kxgameob_vector_set(bmo, subtype);
}

static Mathutils_Callback mathutils_kxgameob_vector_cb = {mathutils_kxgameob_generic_check,
                                                          mathutils_kxgameob_vector_get,
                                                          mathutils_kxgameob_vector_set,
                                                          mathutils_kxgameob_vector_get_index,
                                                          mathutils_kxgameob_vector_set_index};

/* Matrix */
#  define MATHUTILS_MAT_CB_ORI_LOCAL 1
#  define MATHUTILS_MAT_CB_ORI_GLOBAL 2

static unsigned char mathutils_kxgameob_matrix_cb_index = -1; /* index for our callbacks */

static int mathutils_kxgameob_matrix_get(blender::BaseMathObject *bmo, int subtype)
{
  KX_GameObject *self = static_cast<KX_GameObject *> EXP_PROXY_REF(bmo->cb_user);
  if (self == nullptr)
    return -1;

  switch (subtype) {
    case MATHUTILS_MAT_CB_ORI_LOCAL:
      self->NodeGetLocalOrientation().getValue3x3(bmo->data);
      break;
    case MATHUTILS_MAT_CB_ORI_GLOBAL:
      self->NodeGetWorldOrientation().getValue3x3(bmo->data);
      break;
  }

  return 0;
}

static int mathutils_kxgameob_matrix_set(blender::BaseMathObject *bmo, int subtype)
{
  KX_GameObject *self = static_cast<KX_GameObject *> EXP_PROXY_REF(bmo->cb_user);
  if (self == nullptr)
    return -1;

  MT_Matrix3x3 mat3x3;
  switch (subtype) {
    case MATHUTILS_MAT_CB_ORI_LOCAL:
      mat3x3.setValue3x3(bmo->data);
      self->NodeSetLocalOrientation(mat3x3);
      self->NodeUpdateGS(0.f);
      break;
    case MATHUTILS_MAT_CB_ORI_GLOBAL:
      mat3x3.setValue3x3(bmo->data);
      self->NodeSetLocalOrientation(mat3x3);
      self->NodeUpdateGS(0.f);
      break;
  }

  return 0;
}

static Mathutils_Callback mathutils_kxgameob_matrix_cb = {mathutils_kxgameob_generic_check,
                                                          mathutils_kxgameob_matrix_get,
                                                          mathutils_kxgameob_matrix_set,
                                                          nullptr,
                                                          nullptr};

void KX_GameObject_Mathutils_Callback_Init(void)
{
  // register mathutils callbacks, ok to run more than once.
  mathutils_kxgameob_vector_cb_index = Mathutils_RegisterCallback(&mathutils_kxgameob_vector_cb);
  mathutils_kxgameob_matrix_cb_index = Mathutils_RegisterCallback(&mathutils_kxgameob_matrix_cb);
}

#endif  // USE_MATHUTILS

#ifdef WITH_PYTHON
/* ------- python stuff ---------------------------------------------------*/
PyMethodDef KX_GameObject::Methods[] = {
    {"applyForce", (PyCFunction)KX_GameObject::sPyApplyForce, METH_VARARGS},
    {"applyTorque", (PyCFunction)KX_GameObject::sPyApplyTorque, METH_VARARGS},
    {"applyRotation", (PyCFunction)KX_GameObject::sPyApplyRotation, METH_VARARGS},
    {"applyMovement", (PyCFunction)KX_GameObject::sPyApplyMovement, METH_VARARGS},
    {"getLinearVelocity", (PyCFunction)KX_GameObject::sPyGetLinearVelocity, METH_VARARGS},
    {"setLinearVelocity", (PyCFunction)KX_GameObject::sPySetLinearVelocity, METH_VARARGS},
    {"getAngularVelocity", (PyCFunction)KX_GameObject::sPyGetAngularVelocity, METH_VARARGS},
    {"setAngularVelocity", (PyCFunction)KX_GameObject::sPySetAngularVelocity, METH_VARARGS},
    {"getVelocity", (PyCFunction)KX_GameObject::sPyGetVelocity, METH_VARARGS},
    {"setDamping", (PyCFunction)KX_GameObject::sPySetDamping, METH_VARARGS},
    {"getReactionForce", (PyCFunction)KX_GameObject::sPyGetReactionForce, METH_NOARGS},
    {"alignAxisToVect",
     (PyCFunction)KX_GameObject::sPyAlignAxisToVect,
     METH_VARARGS | METH_KEYWORDS},
    {"getAxisVect", (PyCFunction)KX_GameObject::sPyGetAxisVect, METH_O},
    {"suspendPhysics", (PyCFunction)KX_GameObject::sPySuspendPhysics, METH_VARARGS},
    {"restorePhysics", (PyCFunction)KX_GameObject::sPyRestorePhysics, METH_NOARGS},
    {"suspendDynamics", (PyCFunction)KX_GameObject::sPySuspendDynamics, METH_VARARGS},
    {"restoreDynamics", (PyCFunction)KX_GameObject::sPyRestoreDynamics, METH_NOARGS},
    {"enableRigidBody", (PyCFunction)KX_GameObject::sPyEnableRigidBody, METH_NOARGS},
    {"disableRigidBody", (PyCFunction)KX_GameObject::sPyDisableRigidBody, METH_NOARGS},
    {"applyImpulse", (PyCFunction)KX_GameObject::sPyApplyImpulse, METH_VARARGS},
    {"setCollisionMargin", (PyCFunction)KX_GameObject::sPySetCollisionMargin, METH_O},
    {"collide", (PyCFunction)KX_GameObject::sPyCollide, METH_O},
    {"setParent", (PyCFunction)KX_GameObject::sPySetParent, METH_VARARGS | METH_KEYWORDS},
    {"setVisible", (PyCFunction)KX_GameObject::sPySetVisible, METH_VARARGS},
    {"setOcclusion", (PyCFunction)KX_GameObject::sPySetOcclusion, METH_VARARGS},
    {"removeParent", (PyCFunction)KX_GameObject::sPyRemoveParent, METH_NOARGS},

    {"getPhysicsId", (PyCFunction)KX_GameObject::sPyGetPhysicsId, METH_NOARGS},
    {"getPropertyNames", (PyCFunction)KX_GameObject::sPyGetPropertyNames, METH_NOARGS},
    {"replaceMesh", (PyCFunction)KX_GameObject::sPyReplaceMesh, METH_VARARGS | METH_KEYWORDS},
    {"endObject", (PyCFunction)KX_GameObject::sPyEndObject, METH_NOARGS},
    {"reinstancePhysicsMesh",
     (PyCFunction)KX_GameObject::sPyReinstancePhysicsMesh,
     METH_VARARGS | METH_KEYWORDS},
    {"replacePhysicsShape", (PyCFunction)KX_GameObject::sPyReplacePhysicsShape, METH_O},

    {"setCcdMotionThreshold", (PyCFunction)KX_GameObject::sPySetCcdMotionThreshold, METH_VARARGS},
    {"setCcdSweptSphereRadius",
     (PyCFunction)KX_GameObject::sPySetCcdSweptSphereRadius,
     METH_VARARGS},

    EXP_PYMETHODTABLE_KEYWORDS(KX_GameObject, rayCastTo),
    EXP_PYMETHODTABLE_KEYWORDS(KX_GameObject, rayCast),
    EXP_PYMETHODTABLE_O(KX_GameObject, getDistanceTo),
    EXP_PYMETHODTABLE_O(KX_GameObject, getVectTo),
    EXP_PYMETHODTABLE_KEYWORDS(KX_GameObject, sendMessage),
    EXP_PYMETHODTABLE(KX_GameObject, addDebugProperty),

    EXP_PYMETHODTABLE_KEYWORDS(KX_GameObject, playAction),
    EXP_PYMETHODTABLE(KX_GameObject, stopAction),
    EXP_PYMETHODTABLE(KX_GameObject, getActionFrame),
    EXP_PYMETHODTABLE(KX_GameObject, getActionName),
    EXP_PYMETHODTABLE(KX_GameObject, setActionFrame),
    EXP_PYMETHODTABLE(KX_GameObject, isPlayingAction),
    EXP_PYMETHODTABLE_KEYWORDS(KX_GameObject, resetSmoothing),

    // dict style access for props
    {"get", (PyCFunction)KX_GameObject::sPyget, METH_VARARGS},

    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef KX_GameObject::Attributes[] = {
    EXP_PYATTRIBUTE_SHORT_RO("currentLodLevel", KX_GameObject, m_currentLodLevel),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "lodManager", KX_GameObject, pyattr_get_lodManager, pyattr_set_lodManager),
    EXP_PYATTRIBUTE_RW_FUNCTION("name", KX_GameObject, pyattr_get_name, pyattr_set_name),
    EXP_PYATTRIBUTE_RO_FUNCTION("parent", KX_GameObject, pyattr_get_parent),
    EXP_PYATTRIBUTE_RO_FUNCTION("groupMembers", KX_GameObject, pyattr_get_group_members),
    EXP_PYATTRIBUTE_RO_FUNCTION("groupObject", KX_GameObject, pyattr_get_group_object),
    EXP_PYATTRIBUTE_RO_FUNCTION("scene", KX_GameObject, pyattr_get_scene),
    EXP_PYATTRIBUTE_RO_FUNCTION("life", KX_GameObject, pyattr_get_life),
    EXP_PYATTRIBUTE_RW_FUNCTION("mass", KX_GameObject, pyattr_get_mass, pyattr_set_mass),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "friction", KX_GameObject, pyattr_get_friction, pyattr_set_friction),
    EXP_PYATTRIBUTE_RO_FUNCTION(
        "isSuspendDynamics", KX_GameObject, pyattr_get_is_suspend_dynamics),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "linVelocityMin", KX_GameObject, pyattr_get_lin_vel_min, pyattr_set_lin_vel_min),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "linVelocityMax", KX_GameObject, pyattr_get_lin_vel_max, pyattr_set_lin_vel_max),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "angularVelocityMin", KX_GameObject, pyattr_get_ang_vel_min, pyattr_set_ang_vel_min),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "angularVelocityMax", KX_GameObject, pyattr_get_ang_vel_max, pyattr_set_ang_vel_max),
    EXP_PYATTRIBUTE_RW_FUNCTION("layer", KX_GameObject, pyattr_get_layer, pyattr_set_layer),
    EXP_PYATTRIBUTE_RW_FUNCTION("visible", KX_GameObject, pyattr_get_visible, pyattr_set_visible),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "smoothMotion", KX_GameObject, pyattr_get_smooth_motion, pyattr_set_smooth_motion),
    EXP_PYATTRIBUTE_BOOL_RW("occlusion", KX_GameObject, m_bOccluder),

    EXP_PYATTRIBUTE_RW_FUNCTION("physicsCullingRadius",
                                KX_GameObject,
                                pyattr_get_physicsCullingRadius,
                                pyattr_set_physicsCullingRadius),
    EXP_PYATTRIBUTE_RW_FUNCTION("logicCullingRadius",
                                KX_GameObject,
                                pyattr_get_logicCullingRadius,
                                pyattr_set_logicCullingRadius),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "physicsCulling", KX_GameObject, pyattr_get_physicsCulling, pyattr_set_physicsCulling),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "logicCulling", KX_GameObject, pyattr_get_logicCulling, pyattr_set_logicCulling),

    EXP_PYATTRIBUTE_RW_FUNCTION(
        "position", KX_GameObject, pyattr_get_worldPosition, pyattr_set_localPosition),
    EXP_PYATTRIBUTE_RO_FUNCTION("localInertia", KX_GameObject, pyattr_get_localInertia),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "orientation", KX_GameObject, pyattr_get_worldOrientation, pyattr_set_localOrientation),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "scaling", KX_GameObject, pyattr_get_worldScaling, pyattr_set_localScaling),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "timeOffset", KX_GameObject, pyattr_get_timeOffset, pyattr_set_timeOffset),
    EXP_PYATTRIBUTE_RW_FUNCTION("collisionCallbacks",
                                KX_GameObject,
                                pyattr_get_collisionCallbacks,
                                pyattr_set_collisionCallbacks),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "onRemove", KX_GameObject, pyattr_get_remove_callback, pyattr_set_remove_callback),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "collisionGroup", KX_GameObject, pyattr_get_collisionGroup, pyattr_set_collisionGroup),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "collisionMask", KX_GameObject, pyattr_get_collisionMask, pyattr_set_collisionMask),
    EXP_PYATTRIBUTE_RW_FUNCTION("state", KX_GameObject, pyattr_get_state, pyattr_set_state),
    EXP_PYATTRIBUTE_RO_FUNCTION("meshes", KX_GameObject, pyattr_get_meshes),
    EXP_PYATTRIBUTE_RW_FUNCTION("localOrientation",
                                KX_GameObject,
                                pyattr_get_localOrientation,
                                pyattr_set_localOrientation),
    EXP_PYATTRIBUTE_RW_FUNCTION("worldOrientation",
                                KX_GameObject,
                                pyattr_get_worldOrientation,
                                pyattr_set_worldOrientation),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "localPosition", KX_GameObject, pyattr_get_localPosition, pyattr_set_localPosition),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "worldPosition", KX_GameObject, pyattr_get_worldPosition, pyattr_set_worldPosition),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "localScale", KX_GameObject, pyattr_get_localScaling, pyattr_set_localScaling),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "worldScale", KX_GameObject, pyattr_get_worldScaling, pyattr_set_worldScaling),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "localTransform", KX_GameObject, pyattr_get_localTransform, pyattr_set_localTransform),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "worldTransform", KX_GameObject, pyattr_get_worldTransform, pyattr_set_worldTransform),
    EXP_PYATTRIBUTE_RW_FUNCTION("linearVelocity",
                                KX_GameObject,
                                pyattr_get_localLinearVelocity,
                                pyattr_set_worldLinearVelocity),
    EXP_PYATTRIBUTE_RW_FUNCTION("localLinearVelocity",
                                KX_GameObject,
                                pyattr_get_localLinearVelocity,
                                pyattr_set_localLinearVelocity),
    EXP_PYATTRIBUTE_RW_FUNCTION("worldLinearVelocity",
                                KX_GameObject,
                                pyattr_get_worldLinearVelocity,
                                pyattr_set_worldLinearVelocity),
    EXP_PYATTRIBUTE_RW_FUNCTION("angularVelocity",
                                KX_GameObject,
                                pyattr_get_localAngularVelocity,
                                pyattr_set_worldAngularVelocity),
    EXP_PYATTRIBUTE_RW_FUNCTION("localAngularVelocity",
                                KX_GameObject,
                                pyattr_get_localAngularVelocity,
                                pyattr_set_localAngularVelocity),
    EXP_PYATTRIBUTE_RW_FUNCTION("worldAngularVelocity",
                                KX_GameObject,
                                pyattr_get_worldAngularVelocity,
                                pyattr_set_worldAngularVelocity),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "linearDamping", KX_GameObject, pyattr_get_linearDamping, pyattr_set_linearDamping),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "angularDamping", KX_GameObject, pyattr_get_angularDamping, pyattr_set_angularDamping),
    EXP_PYATTRIBUTE_RO_FUNCTION("children", KX_GameObject, pyattr_get_children),
    EXP_PYATTRIBUTE_RO_FUNCTION("childrenRecursive", KX_GameObject, pyattr_get_children_recursive),
    EXP_PYATTRIBUTE_RO_FUNCTION("attrDict", KX_GameObject, pyattr_get_attrDict),
    EXP_PYATTRIBUTE_RW_FUNCTION("color", KX_GameObject, pyattr_get_obcolor, pyattr_set_obcolor),
    EXP_PYATTRIBUTE_RW_FUNCTION("debug", KX_GameObject, pyattr_get_debug, pyattr_set_debug),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "debugRecursive", KX_GameObject, pyattr_get_debugRecursive, pyattr_set_debugRecursive),
    EXP_PYATTRIBUTE_RW_FUNCTION("gravity", KX_GameObject, pyattr_get_gravity, pyattr_set_gravity),

    EXP_PYATTRIBUTE_RO_FUNCTION("blenderObject", KX_GameObject, pyattr_get_blender_object),

    /* experimental, don't rely on these yet */
    EXP_PYATTRIBUTE_RO_FUNCTION("sensors", KX_GameObject, pyattr_get_sensors),
    EXP_PYATTRIBUTE_RO_FUNCTION("controllers", KX_GameObject, pyattr_get_controllers),
    EXP_PYATTRIBUTE_RO_FUNCTION("actuators", KX_GameObject, pyattr_get_actuators),
    EXP_PYATTRIBUTE_RO_FUNCTION("components", KX_GameObject, pyattr_get_components),
    EXP_PYATTRIBUTE_RO_FUNCTION("logger", KX_GameObject, KX_PythonProxy::pyattr_get_logger),
    EXP_PYATTRIBUTE_RO_FUNCTION(
        "loggerName", KX_GameObject, KX_PythonProxy::pyattr_get_logger_name),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

PyObject *KX_GameObject::PyReplaceMesh(PyObject *args, PyObject *kwds)
{
  SCA_LogicManager *logicmgr = GetScene()->GetLogicManager();

  PyObject *value;
  int use_gfx = 1, use_phys = 0;
  RAS_MeshObject *new_mesh;

  static const char *kwlist[] = {"mesh", "useDisplayMesh", "usePhysicsMesh", nullptr};
  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwds,
                                   "O|ii:replaceMesh",
                                   const_cast<char **>(kwlist),
                                   &value,
                                   &use_gfx,
                                   &use_phys))
  {
    return nullptr;
  }

  if (!ConvertPythonToMesh(
          logicmgr, value, &new_mesh, false, "gameOb.replaceMesh(value): KX_GameObject"))
    return nullptr;

  GetScene()->ReplaceMesh(this, new_mesh, (bool)use_gfx, (bool)use_phys);
  Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyEndObject()
{
  GetScene()->DelayedRemoveObject(this);

  Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyReinstancePhysicsMesh(PyObject *args, PyObject *kwds)
{
  KX_GameObject *gameobj = nullptr;
  RAS_MeshObject *mesh = nullptr;
  SCA_LogicManager *logicmgr = GetScene()->GetLogicManager();
  int dupli = 0;
  int evaluated;

  PyObject *gameobj_py = nullptr;
  PyObject *mesh_py = nullptr;

  static const char *kwlist[] = {"gameObject", "meshObject", "dupli", "evaluated", nullptr};
  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwds,
                                   "|OOii:reinstancePhysicsMesh",
                                   const_cast<char **>(kwlist),
                                   &gameobj_py,
                                   &mesh_py,
                                   &dupli,
                                   &evaluated) ||
      (gameobj_py && !ConvertPythonToGameObject(
                         logicmgr,
                         gameobj_py,
                         &gameobj,
                         true,
                         "gameOb.reinstancePhysicsMesh(obj, mesh, dupli): KX_GameObject")) ||
      (mesh_py &&
       !ConvertPythonToMesh(logicmgr,
                            mesh_py,
                            &mesh,
                            true,
                            "gameOb.reinstancePhysicsMesh(obj, mesh, dupli): KX_GameObject")))
  {
    return nullptr;
  }

  /* gameobj and mesh can be nullptr */
  if (GetPhysicsController() &&
      GetPhysicsController()->ReinstancePhysicsShape(gameobj, mesh, dupli, evaluated))
    Py_RETURN_TRUE;

  Py_RETURN_FALSE;
}

PyObject *KX_GameObject::PyReplacePhysicsShape(PyObject *value)
{
  KX_GameObject *gameobj;
  SCA_LogicManager *logicmgr = GetScene()->GetLogicManager();

  if (!ConvertPythonToGameObject(
          logicmgr, value, &gameobj, false, "gameOb.replacePhysicsShape(obj): KX_GameObject"))
  {
    return nullptr;
  }

  if (!GetPhysicsController() || !gameobj->GetPhysicsController()) {
    PyErr_SetString(PyExc_AttributeError,
                    "gameOb.replacePhysicsShape(obj): function only available for objects with "
                    "collisions enabled");
    return nullptr;
  }

  if (GetPhysicsController()->ReplacePhysicsShape(gameobj->GetPhysicsController())) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

static PyObject *Map_GetItem(PyObject *self_v, PyObject *item)
{
  KX_GameObject *self = static_cast<KX_GameObject *> EXP_PROXY_REF(self_v);
  const char *attr_str = _PyUnicode_AsString(item);
  EXP_Value *resultattr;
  PyObject *pyconvert;

  if (self == nullptr) {
    PyErr_SetString(PyExc_SystemError, "val = gameOb[key]: KX_GameObject, " EXP_PROXY_ERROR_MSG);
    return nullptr;
  }

  /* first see if the attributes a string and try get the cvalue attribute */
  if (attr_str && (resultattr = self->GetProperty(attr_str))) {
    pyconvert = resultattr->ConvertValueToPython();
    return pyconvert ? pyconvert : resultattr->GetProxy();
  }
  /* no EXP_Value attribute, try get the python only m_attr_dict attribute */
  else if (self->m_attr_dict && (pyconvert = PyDict_GetItem(self->m_attr_dict, item))) {

    if (attr_str)
      PyErr_Clear();
    Py_INCREF(pyconvert);
    return pyconvert;
  }
  else {
    if (attr_str)
      PyErr_Format(PyExc_KeyError,
                   "value = gameOb[key]: KX_GameObject, key \"%s\" does not exist",
                   attr_str);
    else
      PyErr_SetString(PyExc_KeyError, "value = gameOb[key]: KX_GameObject, key does not exist");
    return nullptr;
  }
}

/** Reads exactly \a size numbers from a Python sequence such as a mathutils Vector. */
static bool python_numbers(PyObject *value, float *r_values, const int size)
{
  if (PyUnicode_Check(value)) {
    return false;
  }
  PyObject *sequence = PySequence_Fast(value, "");
  if (sequence == nullptr) {
    PyErr_Clear();
    return false;
  }
  bool valid = PySequence_Fast_GET_SIZE(sequence) == size;
  for (int index = 0; valid && index < size; index++) {
    r_values[index] = float(PyFloat_AsDouble(PySequence_Fast_GET_ITEM(sequence, index)));
    if (PyErr_Occurred()) {
      PyErr_Clear();
      valid = false;
    }
  }
  Py_DECREF(sequence);
  return valid;
}

/** Python value for a game property of the \a kind of an existing Vector, List, Dictionary or
 * Object property, or of any kind for items of a List or Dictionary (\a kind 0). */
static EXP_Value *python_to_property_value(PyObject *value, const int kind)
{
  float numbers[4];
  switch (kind) {
    case VALUE_VECTOR2_TYPE:
      return python_numbers(value, numbers, 2) ?
                 new EXP_Vector2Value(MT_Vector2(numbers[0], numbers[1])) :
                 nullptr;
    case VALUE_VECTOR3_TYPE:
      return python_numbers(value, numbers, 3) ? new EXP_Vector3Value(MT_Vector3(numbers)) :
                                                 nullptr;
    case VALUE_VECTOR4_TYPE:
      return python_numbers(value, numbers, 4) ? new EXP_Vector4Value(MT_Vector4(numbers)) :
                                                 nullptr;
    case VALUE_OBJECT_TYPE: {
      KX_GameObject *object = nullptr;
      if (value != Py_None) {
        if (!PyObject_TypeCheck(value, &KX_GameObject::Type) ||
            (object = static_cast<KX_GameObject *>(EXP_PROXY_REF(value))) == nullptr)
        {
          return nullptr;
        }
      }
      return new KX_ObjectReferenceValue(object);
    }
    case VALUE_ARRAY_TYPE: {
      if (!PyList_Check(value) && !PyTuple_Check(value)) {
        return nullptr;
      }
      EXP_ArrayValue *list = new EXP_ArrayValue();
      for (Py_ssize_t index = 0; index < PySequence_Size(value); index++) {
        PyObject *item = PySequence_GetItem(value, index);
        EXP_Value *converted = python_to_property_value(item, 0);
        Py_DECREF(item);
        if (converted == nullptr) {
          list->Release();
          return nullptr;
        }
        list->Append(converted);
      }
      return list;
    }
    case VALUE_DICT_TYPE: {
      if (!PyDict_Check(value)) {
        return nullptr;
      }
      EXP_DictValue *dict = new EXP_DictValue();
      PyObject *key, *item;
      Py_ssize_t position = 0;
      while (PyDict_Next(value, &position, &key, &item)) {
        EXP_Value *converted = PyUnicode_Check(key) ? python_to_property_value(item, 0) :
                                                      nullptr;
        if (converted == nullptr) {
          dict->Release();
          return nullptr;
        }
        dict->Set(_PyUnicode_AsString(key), converted);
      }
      return dict;
    }
    default:
      break;
  }
  /* Items: plain values, then the kinds above by the Python type. */
  if (PyBool_Check(value)) {
    return new EXP_BoolValue(value == Py_True);
  }
  if (PyLong_Check(value)) {
    const long long number = PyLong_AsLongLong(value);
    if (PyErr_Occurred()) {
      PyErr_Clear();
      return nullptr;
    }
    return new EXP_IntValue(cInt(number));
  }
  if (PyFloat_Check(value)) {
    const double number = PyFloat_AsDouble(value);
    return std::abs(number) <= double(FLT_MAX) ? new EXP_FloatValue(float(number)) : nullptr;
  }
  if (PyUnicode_Check(value)) {
    return new EXP_StringValue(_PyUnicode_AsString(value), "");
  }
  if (value == Py_None || PyObject_TypeCheck(value, &KX_GameObject::Type)) {
    return python_to_property_value(value, VALUE_OBJECT_TYPE);
  }
  if (PyList_Check(value) || PyTuple_Check(value)) {
    return python_to_property_value(value, VALUE_ARRAY_TYPE);
  }
  if (PyDict_Check(value)) {
    return python_to_property_value(value, VALUE_DICT_TYPE);
  }
  /* Vectors such as mathutils.Vector, by their size. */
  const Py_ssize_t size = PySequence_Check(value) ? PySequence_Size(value) : -1;
  if (size == -1) {
    PyErr_Clear();
  }
  switch (size) {
    case 2:
      return python_to_property_value(value, VALUE_VECTOR2_TYPE);
    case 3:
      return python_to_property_value(value, VALUE_VECTOR3_TYPE);
    case 4:
      return python_to_property_value(value, VALUE_VECTOR4_TYPE);
    default:
      return nullptr;
  }
}

static bool is_structured_property(EXP_Value *property)
{
  return property != nullptr && ELEM(property->GetValueType(),
                                     VALUE_VECTOR2_TYPE,
                                     VALUE_VECTOR3_TYPE,
                                     VALUE_VECTOR4_TYPE,
                                     VALUE_ARRAY_TYPE,
                                     VALUE_DICT_TYPE,
                                     VALUE_OBJECT_TYPE);
}

static int Map_SetItem(PyObject *self_v, PyObject *key, PyObject *val)
{
  KX_GameObject *self = static_cast<KX_GameObject *> EXP_PROXY_REF(self_v);
  const char *attr_str = _PyUnicode_AsString(key);
  if (attr_str == nullptr)
    PyErr_Clear();

  if (self == nullptr) {
    PyErr_SetString(PyExc_SystemError, "gameOb[key] = value: KX_GameObject, " EXP_PROXY_ERROR_MSG);
    return -1;
  }

  if (val == nullptr) { /* del ob["key"] */
    int del = 0;

    /* try remove both just in case */
    if (attr_str)
      del |= (self->RemoveProperty(attr_str) == true) ? 1 : 0;

    if (self->m_attr_dict)
      del |= (PyDict_DelItem(self->m_attr_dict, key) == 0) ? 1 : 0;

    if (del == 0) {
      if (attr_str)
        PyErr_Format(PyExc_KeyError,
                     "gameOb[key] = value: KX_GameObject, key \"%s\" could not be set",
                     attr_str);
      else
        PyErr_SetString(PyExc_KeyError,
                        "del gameOb[key]: KX_GameObject, key could not be deleted");
      return -1;
    }
    else if (self->m_attr_dict) {
      PyErr_Clear(); /* PyDict_DelItem sets an error when it fails */
    }
  }
  else { /* ob["key"] = value */
    bool set = false;

    /* A Vector, List, Dictionary or Object property keeps its kind, so Logic Nodes, Save Game
     * and the debug display still see it. New keys keep Python values as they are. */
    EXP_Value *structured = attr_str ? self->GetProperty(attr_str) : nullptr;
    if (is_structured_property(structured)) {
      EXP_Value *converted = python_to_property_value(val, structured->GetValueType());
      if (converted == nullptr) {
        PyErr_Format(PyExc_TypeError,
                     "gameOb[key] = value: KX_GameObject, \"%s\" can't hold this value",
                     attr_str);
        return -1;
      }
      self->SetProperty(attr_str, converted);
      converted->Release();
      if (self->m_attr_dict && PyDict_DelItem(self->m_attr_dict, key) != 0) {
        PyErr_Clear();
      }
      return 0;
    }

    /* as EXP_Value */
    if (attr_str && PyObject_TypeCheck(val, &EXP_PyObjectPlus::Type) ==
                        0) /* don't allow GameObjects for eg to be assigned to EXP_Value props */
    {
      EXP_Value *vallie = self->ConvertPythonToValue(val, false, "gameOb[key] = value: ");

      if (vallie) {
        EXP_Value *oldprop = self->GetProperty(attr_str);

        if (oldprop)
          oldprop->SetValue(vallie);
        else
          self->SetProperty(attr_str, vallie);

        vallie->Release();
        set = true;

        /* try remove dict value to avoid double ups */
        if (self->m_attr_dict) {
          if (PyDict_DelItem(self->m_attr_dict, key) != 0)
            PyErr_Clear();
        }
      }
      else if (PyErr_Occurred()) {
        return -1;
      }
    }

    if (set == false) {
      if (self->m_attr_dict == nullptr) /* lazy init */
        self->m_attr_dict = PyDict_New();

      if (PyDict_SetItem(self->m_attr_dict, key, val) == 0) {
        if (attr_str)
          self->RemoveProperty(attr_str); /* overwrite the EXP_Value if it exists */
        set = true;
      }
      else {
        if (attr_str)
          PyErr_Format(
              PyExc_KeyError,
              "gameOb[key] = value: KX_GameObject, key \"%s\" not be added to internal dictionary",
              attr_str);
        else
          PyErr_SetString(
              PyExc_KeyError,
              "gameOb[key] = value: KX_GameObject, key not be added to internal dictionary");
      }
    }

    if (set == false) {
      return -1; /* pythons error value */
    }
  }

  return 0; /* success */
}

static int Seq_Contains(PyObject *self_v, PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *> EXP_PROXY_REF(self_v);

  if (self == nullptr) {
    PyErr_SetString(PyExc_SystemError, "val in gameOb: KX_GameObject, " EXP_PROXY_ERROR_MSG);
    return -1;
  }

  if (PyUnicode_Check(value) && self->GetProperty(_PyUnicode_AsString(value)))
    return 1;

  if (self->m_attr_dict && PyDict_GetItem(self->m_attr_dict, value))
    return 1;

  return 0;
}

PyMappingMethods KX_GameObject::Mapping = {
    (lenfunc) nullptr,          /*inquiry mp_length */
    (binaryfunc)Map_GetItem,    /*binaryfunc mp_subscript */
    (objobjargproc)Map_SetItem, /*objobjargproc mp_ass_subscript */
};

PySequenceMethods KX_GameObject::Sequence = {
    nullptr,                  /* Cant set the len otherwise it can evaluate as false */
    nullptr,                  /* sq_concat */
    nullptr,                  /* sq_repeat */
    nullptr,                  /* sq_item */
    nullptr,                  /* sq_slice */
    nullptr,                  /* sq_ass_item */
    nullptr,                  /* sq_ass_slice */
    (objobjproc)Seq_Contains, /* sq_contains */
    (binaryfunc) nullptr,     /* sq_inplace_concat */
    (ssizeargfunc) nullptr,   /* sq_inplace_repeat */
};

PyObject *KX_GameObject::game_object_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  KX_GameObject *obj = new KX_GameObject();

  PyObject *proxy = py_base_new(type, PyTuple_Pack(1, obj->GetProxy()), kwds);
  if (!proxy) {
    delete obj;
    return nullptr;
  }

  return proxy;
}

PyTypeObject KX_GameObject::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "KX_GameObject",
                                    sizeof(EXP_PyObjectPlus_Proxy),
                                    0,
                                    py_base_dealloc,
                                    0,
                                    0,
                                    0,
                                    0,
                                    py_base_repr,
                                    0,
                                    &Sequence,
                                    &Mapping,
                                    0,
                                    0,
                                    0,
                                    nullptr,
                                    nullptr,
                                    0,
                                    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
                                    0,
                                    0,
                                    0,
                                    0,
                                    0,
                                    0,
                                    0,
                                    Methods,
                                    0,
                                    0,
                                    &SCA_IObject::Type,
                                    0,
                                    0,
                                    0,
                                    0,
                                    0,
                                    0,
                                    game_object_new};

PyObject *KX_GameObject::pyattr_get_name(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyUnicode_FromStdString(self->GetName());
}

int KX_GameObject::pyattr_set_name(EXP_PyObjectPlus *self_v,
                                   const EXP_PYATTRIBUTE_DEF *attrdef,
                                   PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

  if (!PyUnicode_Check(value)) {
    PyErr_SetString(PyExc_TypeError, "gameOb.name = str: KX_GameObject, expected a string");
    return PY_SET_ATTR_FAIL;
  }

  std::string newname = std::string(_PyUnicode_AsString(value));
  std::string oldname = self->GetName();

  SCA_LogicManager *manager = self->GetScene()->GetLogicManager();

  // If true, it mean that's this game object is not a replica and was added at conversion time.
  if (manager->GetGameObjectByName(oldname) == self) {
    /* Two non-replica objects can have the same name bacause these objects are register in the
     * logic manager and that the result of GetGameObjectByName will be undefined. */
    if (manager->GetGameObjectByName(newname)) {
      PyErr_Format(
          PyExc_TypeError,
          "gameOb.name = str: name %s is already used by an other non-replica game object",
          oldname.c_str());
      return PY_SET_ATTR_FAIL;
    }
    // Unregister the old name.
    manager->UnregisterGameObjectName(oldname);
    // Register the object under the new name.
    manager->RegisterGameObjectName(newname, self);
  }

  // Change the name
  self->SetName(newname);
  /* Lookups by name (text values such as {Player.gold}) find it under its new name. */
  if (KX_Scene *scene = self->GetScene()) {
    scene->NoteObjectListChanged();
  }

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_parent(EXP_PyObjectPlus *self_v,
                                           const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  KX_GameObject *parent = self->GetParent();
  if (parent) {
    return parent->GetProxy();
  }
  Py_RETURN_NONE;
}

PyObject *KX_GameObject::pyattr_get_group_members(EXP_PyObjectPlus *self_v,
                                                  const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  EXP_ListValue<KX_GameObject> *instances = self->GetInstanceObjects();
  if (instances) {
    return instances->GetProxy();
  }
  Py_RETURN_NONE;
}

PyObject *KX_GameObject::pyattr_get_collisionCallbacks(EXP_PyObjectPlus *self_v,
                                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

  // Only objects with a physics controller should have collision callbacks
  PYTHON_CHECK_PHYSICS_CONTROLLER(self, "collisionCallbacks", nullptr);

  // Return the existing callbacks
  if (self->m_collisionCallbacks == nullptr) {
    self->m_collisionCallbacks = PyList_New(0);
    // Subscribe to collision update from KX_CollisionEventManager
    self->RegisterCollisionCallbacks();
  }
  Py_INCREF(self->m_collisionCallbacks);
  return self->m_collisionCallbacks;
}

int KX_GameObject::pyattr_set_collisionCallbacks(EXP_PyObjectPlus *self_v,
                                                 const EXP_PYATTRIBUTE_DEF *attrdef,
                                                 PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

  // Only objects with a physics controller should have collision callbacks
  PYTHON_CHECK_PHYSICS_CONTROLLER(self, "collisionCallbacks", PY_SET_ATTR_FAIL);

  if (!PyList_CheckExact(value)) {
    PyErr_SetString(PyExc_ValueError, "Expected a list");
    return PY_SET_ATTR_FAIL;
  }

  if (self->m_collisionCallbacks == nullptr) {
    self->RegisterCollisionCallbacks();
  }
  else {
    Py_DECREF(self->m_collisionCallbacks);
  }

  Py_INCREF(value);

  self->m_collisionCallbacks = value;

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_remove_callback(EXP_PyObjectPlus *self_v,
                                                    const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

  if (!self->m_removeCallbacks) {
    self->m_removeCallbacks = PyList_New(0);
  }

  Py_INCREF(self->m_removeCallbacks);

  return self->m_removeCallbacks;
}

int KX_GameObject::pyattr_set_remove_callback(EXP_PyObjectPlus *self_v,
                                              const EXP_PYATTRIBUTE_DEF *attrdef,
                                              PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

  if (!PyList_CheckExact(value)) {
    PyErr_SetString(PyExc_ValueError, "Expected a list");
    return PY_SET_ATTR_FAIL;
  }

  Py_XDECREF(self->m_removeCallbacks);

  Py_INCREF(value);
  self->m_removeCallbacks = value;

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_collisionGroup(EXP_PyObjectPlus *self_v,
                                                   const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyLong_FromLong(self->GetCollisionGroup());
}

int KX_GameObject::pyattr_set_collisionGroup(EXP_PyObjectPlus *self_v,
                                             const EXP_PYATTRIBUTE_DEF *attrdef,
                                             PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  int val = PyLong_AsLong(value);

  if (val == -1 && PyErr_Occurred()) {
    PyErr_SetString(PyExc_TypeError,
                    "gameOb.collisionGroup = int: KX_GameObject, expected an int bit field");
    return PY_SET_ATTR_FAIL;
  }

  if (val == 0 || val & ~((1 << OB_MAX_COL_MASKS) - 1)) {
    PyErr_Format(
        PyExc_AttributeError,
        "gameOb.collisionGroup = int: KX_GameObject, expected a int bit field, 0 < group < %i",
        (1 << OB_MAX_COL_MASKS));
    return PY_SET_ATTR_FAIL;
  }

  self->SetCollisionGroup(val);
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_collisionMask(EXP_PyObjectPlus *self_v,
                                                  const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyLong_FromLong(self->GetCollisionMask());
}

int KX_GameObject::pyattr_set_collisionMask(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef,
                                            PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  int val = PyLong_AsLong(value);

  if (val == -1 && PyErr_Occurred()) {
    PyErr_SetString(PyExc_TypeError,
                    "gameOb.collisionMask = int: KX_GameObject, expected an int bit field");
    return PY_SET_ATTR_FAIL;
  }

  if (val == 0 || val & ~((1 << OB_MAX_COL_MASKS) - 1)) {
    PyErr_Format(
        PyExc_AttributeError,
        "gameOb.collisionMask = int: KX_GameObject, expected a int bit field, 0 < mask < %i",
        (1 << OB_MAX_COL_MASKS));
    return PY_SET_ATTR_FAIL;
  }

  self->SetCollisionMask(val);
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_scene(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  KX_Scene *scene = self->GetScene();
  if (scene) {
    return scene->GetProxy();
  }
  Py_RETURN_NONE;
}

PyObject *KX_GameObject::pyattr_get_group_object(EXP_PyObjectPlus *self_v,
                                                 const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  KX_GameObject *pivot = self->GetDupliGroupObject();
  if (pivot) {
    return pivot->GetProxy();
  }
  Py_RETURN_NONE;
}

PyObject *KX_GameObject::pyattr_get_life(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

  EXP_Value *life = self->GetProperty("::timebomb");
  if (life)
    // this convert the timebomb seconds to frames, hard coded 60.0f (assuming 60fps)
    // value hardcoded in KX_Scene::AddReplicaObject()
    return PyFloat_FromDouble(life->GetNumber() * 60.0);
  else
    Py_RETURN_NONE;
}

PyObject *KX_GameObject::pyattr_get_mass(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  PHY_IPhysicsController *spc = self->GetPhysicsController();
  return PyFloat_FromDouble(spc ? spc->GetMass() : 0.0f);
}

int KX_GameObject::pyattr_set_mass(EXP_PyObjectPlus *self_v,
                                   const EXP_PYATTRIBUTE_DEF *attrdef,
                                   PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  PHY_IPhysicsController *spc = self->GetPhysicsController();
  MT_Scalar val = PyFloat_AsDouble(value);
  if (val < 0.0f) { /* also accounts for non float */
    PyErr_SetString(PyExc_AttributeError,
                    "gameOb.mass = float: KX_GameObject, expected a float zero or above");
    return PY_SET_ATTR_FAIL;
  }

  if (spc)
    spc->SetMass(val);

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_friction(EXP_PyObjectPlus *self_v,
                                             const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  PHY_IPhysicsController *spc = self->GetPhysicsController();
  return PyFloat_FromDouble(spc ? spc->GetFriction() : 0.0f);
}

int KX_GameObject::pyattr_set_friction(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef,
                                       PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  PHY_IPhysicsController *spc = self->GetPhysicsController();
  MT_Scalar val = PyFloat_AsDouble(value);
  if (val < 0.0f) { /* also accounts for non float */
    PyErr_SetString(PyExc_AttributeError,
                    "gameOb.friction = float: KX_GameObject, expected a float zero or above");
    return PY_SET_ATTR_FAIL;
  }

  if (spc)
    spc->SetFriction(val);

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_is_suspend_dynamics(EXP_PyObjectPlus *self_v,
                                                        const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

  // Only objects with a physics controller can be suspended
  PYTHON_CHECK_PHYSICS_CONTROLLER(self, attrdef->m_name.c_str(), nullptr);

  return PyBool_FromLong(self->IsDynamicsSuspended());
}

PyObject *KX_GameObject::pyattr_get_lin_vel_min(EXP_PyObjectPlus *self_v,
                                                const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  PHY_IPhysicsController *spc = self->GetPhysicsController();
  return PyFloat_FromDouble(spc ? spc->GetLinVelocityMin() : 0.0f);
}

int KX_GameObject::pyattr_set_lin_vel_min(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef,
                                          PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  PHY_IPhysicsController *spc = self->GetPhysicsController();
  MT_Scalar val = PyFloat_AsDouble(value);
  if (val < 0.0f) { /* also accounts for non float */
    PyErr_SetString(
        PyExc_AttributeError,
        "gameOb.linVelocityMin = float: KX_GameObject, expected a float zero or above");
    return PY_SET_ATTR_FAIL;
  }

  if (spc)
    spc->SetLinVelocityMin(val);

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_lin_vel_max(EXP_PyObjectPlus *self_v,
                                                const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  PHY_IPhysicsController *spc = self->GetPhysicsController();
  return PyFloat_FromDouble(spc ? spc->GetLinVelocityMax() : 0.0f);
}

int KX_GameObject::pyattr_set_lin_vel_max(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef,
                                          PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  PHY_IPhysicsController *spc = self->GetPhysicsController();
  MT_Scalar val = PyFloat_AsDouble(value);
  if (val < 0.0f) { /* also accounts for non float */
    PyErr_SetString(
        PyExc_AttributeError,
        "gameOb.linVelocityMax = float: KX_GameObject, expected a float zero or above");
    return PY_SET_ATTR_FAIL;
  }

  if (spc)
    spc->SetLinVelocityMax(val);

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_ang_vel_min(EXP_PyObjectPlus *self_v,
                                                const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  PHY_IPhysicsController *spc = self->GetPhysicsController();
  return PyFloat_FromDouble(spc ? spc->GetAngularVelocityMin() : 0.0f);
}

int KX_GameObject::pyattr_set_ang_vel_min(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef,
                                          PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  PHY_IPhysicsController *spc = self->GetPhysicsController();
  MT_Scalar val = PyFloat_AsDouble(value);
  if (val < 0.0f) { /* also accounts for non float */
    PyErr_SetString(
        PyExc_AttributeError,
        "gameOb.angularVelocityMin = float: KX_GameObject, expected a nonnegative float");
    return PY_SET_ATTR_FAIL;
  }

  if (spc)
    spc->SetAngularVelocityMin(val);

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_ang_vel_max(EXP_PyObjectPlus *self_v,
                                                const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  PHY_IPhysicsController *spc = self->GetPhysicsController();
  return PyFloat_FromDouble(spc ? spc->GetAngularVelocityMax() : 0.0f);
}

int KX_GameObject::pyattr_set_ang_vel_max(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef,
                                          PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  PHY_IPhysicsController *spc = self->GetPhysicsController();
  MT_Scalar val = PyFloat_AsDouble(value);
  if (val < 0.0f) { /* also accounts for non float */
    PyErr_SetString(
        PyExc_AttributeError,
        "gameOb.angularVelocityMax = float: KX_GameObject, expected a nonnegative float");
    return PY_SET_ATTR_FAIL;
  }

  if (spc)
    spc->SetAngularVelocityMax(val);

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_layer(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyLong_FromLong(self->GetLayer());
}

#  define MAX_LAYERS ((1 << 20) - 1)
int KX_GameObject::pyattr_set_layer(EXP_PyObjectPlus *self_v,
                                    const EXP_PYATTRIBUTE_DEF *attrdef,
                                    PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  int layer = PyLong_AsLong(value);

  if (layer == -1 && PyErr_Occurred()) {
    PyErr_Format(
        PyExc_TypeError, "expected an integer for attribute \"%s\"", attrdef->m_name.c_str());
    return PY_SET_ATTR_FAIL;
  }

  if (layer < 1) {
    PyErr_Format(PyExc_TypeError,
                 "expected an integer greater than 1 for attribute \"%s\"",
                 attrdef->m_name.c_str());
    return PY_SET_ATTR_FAIL;
  }
  else if (layer > MAX_LAYERS) {
    PyErr_Format(PyExc_TypeError,
                 "expected an integer less than %i for attribute \"%s\"",
                 MAX_LAYERS,
                 attrdef->m_name.c_str());
    return PY_SET_ATTR_FAIL;
  }

  self->SetLayer(layer);
  return PY_SET_ATTR_SUCCESS;
}
#  undef MAX_LAYERS

PyObject *KX_GameObject::pyattr_get_visible(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyBool_FromLong(self->GetVisible());
}

int KX_GameObject::pyattr_set_visible(EXP_PyObjectPlus *self_v,
                                      const EXP_PYATTRIBUTE_DEF *attrdef,
                                      PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  int param = PyObject_IsTrue(value);
  if (param == -1) {
    PyErr_SetString(PyExc_AttributeError,
                    "gameOb.visible = bool: KX_GameObject, expected True or False");
    return PY_SET_ATTR_FAIL;
  }

  self->SetVisible(param, false);
  return PY_SET_ATTR_SUCCESS;
}

/** Smooth Motion choices by their RNA names (Object > Physics > Smooth Motion). */
static const char *const smooth_motion_names[] = {"SCENE", "NONE", "INTERPOLATE", "EXTRAPOLATE"};

PyObject *KX_GameObject::pyattr_get_smooth_motion(EXP_PyObjectPlus *self_v,
                                                  const EXP_PYATTRIBUTE_DEF * /*attrdef*/)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyUnicode_FromString(smooth_motion_names[int(self->GetSmoothMotion())]);
}

int KX_GameObject::pyattr_set_smooth_motion(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF * /*attrdef*/,
                                            PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  const char *name = PyUnicode_Check(value) ? PyUnicode_AsUTF8(value) : nullptr;
  for (int mode = 0; name != nullptr && mode < int(ARRAY_SIZE(smooth_motion_names)); mode++) {
    if (STREQ(name, smooth_motion_names[mode])) {
      self->SetSmoothMotion(char(mode));
      return PY_SET_ATTR_SUCCESS;
    }
  }
  PyErr_SetString(PyExc_AttributeError,
                  "gameOb.smoothMotion = str: KX_GameObject, expected 'SCENE', 'NONE', "
                  "'INTERPOLATE' or 'EXTRAPOLATE'");
  return PY_SET_ATTR_FAIL;
}

PyObject *KX_GameObject::pyattr_get_physicsCulling(EXP_PyObjectPlus *self_v,
                                                   const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyBool_FromLong(self->GetActivityCullingInfo().m_flags &
                         ActivityCullingInfo::ACTIVITY_PHYSICS);
}

int KX_GameObject::pyattr_set_physicsCulling(EXP_PyObjectPlus *self_v,
                                             const EXP_PYATTRIBUTE_DEF *attrdef,
                                             PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  int param = PyObject_IsTrue(value);
  if (param == -1) {
    PyErr_SetString(PyExc_AttributeError,
                    "gameOb.physicsCulling = bool: KX_GameObject, expected True or False");
    return PY_SET_ATTR_FAIL;
  }

  self->SetActivityCulling(ActivityCullingInfo::ACTIVITY_PHYSICS, param);
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_logicCulling(EXP_PyObjectPlus *self_v,
                                                 const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyBool_FromLong(self->GetActivityCullingInfo().m_flags &
                         ActivityCullingInfo::ACTIVITY_LOGIC);
}

int KX_GameObject::pyattr_set_logicCulling(EXP_PyObjectPlus *self_v,
                                           const EXP_PYATTRIBUTE_DEF *attrdef,
                                           PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  int param = PyObject_IsTrue(value);
  if (param == -1) {
    PyErr_SetString(PyExc_AttributeError,
                    "gameOb.logicCulling = bool: KX_GameObject, expected True or False");
    return PY_SET_ATTR_FAIL;
  }

  self->SetActivityCulling(ActivityCullingInfo::ACTIVITY_LOGIC, param);
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_physicsCullingRadius(EXP_PyObjectPlus *self_v,
                                                         const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyFloat_FromDouble(std::sqrt(self->GetActivityCullingInfo().m_physicsRadius));
}

int KX_GameObject::pyattr_set_physicsCullingRadius(EXP_PyObjectPlus *self_v,
                                                   const EXP_PYATTRIBUTE_DEF *attrdef,
                                                   PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  const float val = PyFloat_AsDouble(value);
  if (val < 0.0f) {  // Also accounts for non float.
    PyErr_SetString(
        PyExc_AttributeError,
        "gameOb.physicsCullingRadius = float: KX_GameObject, expected a float zero or above");
    return PY_SET_ATTR_FAIL;
  }

  self->GetActivityCullingInfo().m_physicsRadius = val * val;

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_logicCullingRadius(EXP_PyObjectPlus *self_v,
                                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyFloat_FromDouble(std::sqrt(self->GetActivityCullingInfo().m_logicRadius));
}

int KX_GameObject::pyattr_set_logicCullingRadius(EXP_PyObjectPlus *self_v,
                                                 const EXP_PYATTRIBUTE_DEF *attrdef,
                                                 PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  const float val = PyFloat_AsDouble(value);
  if (val < 0.0f) {  // Also accounts for non float.
    PyErr_SetString(
        PyExc_AttributeError,
        "gameOb.logicCullingRadius = float: KX_GameObject, expected a float zero or above");
    return PY_SET_ATTR_FAIL;
  }

  self->GetActivityCullingInfo().m_logicRadius = val * val;

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_worldPosition(EXP_PyObjectPlus *self_v,
                                                  const EXP_PYATTRIBUTE_DEF *attrdef)
{
#  ifdef USE_MATHUTILS
  return Vector_CreatePyObject_cb(EXP_PROXY_FROM_REF_BORROW(self_v),
                                  3,
                                  mathutils_kxgameob_vector_cb_index,
                                  MATHUTILS_VEC_CB_POS_GLOBAL);
#  else
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyObjectFrom(self->NodeGetWorldPosition());
#  endif
}

int KX_GameObject::pyattr_set_worldPosition(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef,
                                            PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  MT_Vector3 pos;
  if (!PyVecTo(value, pos))
    return PY_SET_ATTR_FAIL;

  self->NodeSetWorldPosition(pos);
  self->NodeUpdateGS(0.f);
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_localPosition(EXP_PyObjectPlus *self_v,
                                                  const EXP_PYATTRIBUTE_DEF *attrdef)
{
#  ifdef USE_MATHUTILS
  return Vector_CreatePyObject_cb(EXP_PROXY_FROM_REF_BORROW(self_v),
                                  3,
                                  mathutils_kxgameob_vector_cb_index,
                                  MATHUTILS_VEC_CB_POS_LOCAL);
#  else
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyObjectFrom(self->NodeGetLocalPosition());
#  endif
}

int KX_GameObject::pyattr_set_localPosition(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef,
                                            PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  MT_Vector3 pos;
  if (!PyVecTo(value, pos))
    return PY_SET_ATTR_FAIL;

  self->NodeSetLocalPosition(pos);
  self->NodeUpdateGS(0.f);
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_localInertia(EXP_PyObjectPlus *self_v,
                                                 const EXP_PYATTRIBUTE_DEF *attrdef)
{
#  ifdef USE_MATHUTILS
  return Vector_CreatePyObject_cb(EXP_PROXY_FROM_REF_BORROW(self_v),
                                  3,
                                  mathutils_kxgameob_vector_cb_index,
                                  MATHUTILS_VEC_CB_INERTIA_LOCAL);
#  else
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  if (self->GetPhysicsController1())
    return PyObjectFrom(self->GetPhysicsController1()->GetLocalInertia());
  return PyObjectFrom(MT_Vector3(0.0f, 0.0f, 0.0f));
#  endif
}

PyObject *KX_GameObject::pyattr_get_worldOrientation(EXP_PyObjectPlus *self_v,
                                                     const EXP_PYATTRIBUTE_DEF *attrdef)
{
#  ifdef USE_MATHUTILS
  return Matrix_CreatePyObject_cb(EXP_PROXY_FROM_REF_BORROW(self_v),
                                  3,
                                  3,
                                  mathutils_kxgameob_matrix_cb_index,
                                  MATHUTILS_MAT_CB_ORI_GLOBAL);
#  else
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyObjectFrom(self->NodeGetWorldOrientation());
#  endif
}

int KX_GameObject::pyattr_set_worldOrientation(EXP_PyObjectPlus *self_v,
                                               const EXP_PYATTRIBUTE_DEF *attrdef,
                                               PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

  /* if value is not a sequence PyOrientationTo makes an error */
  MT_Matrix3x3 rot;
  if (!PyOrientationTo(value, rot, "gameOb.worldOrientation = sequence: KX_GameObject, "))
    return PY_SET_ATTR_FAIL;

  self->NodeSetGlobalOrientation(rot);

  self->NodeUpdateGS(0.f);
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_localOrientation(EXP_PyObjectPlus *self_v,
                                                     const EXP_PYATTRIBUTE_DEF *attrdef)
{
#  ifdef USE_MATHUTILS
  return Matrix_CreatePyObject_cb(EXP_PROXY_FROM_REF_BORROW(self_v),
                                  3,
                                  3,
                                  mathutils_kxgameob_matrix_cb_index,
                                  MATHUTILS_MAT_CB_ORI_LOCAL);
#  else
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyObjectFrom(self->NodeGetLocalOrientation());
#  endif
}

int KX_GameObject::pyattr_set_localOrientation(EXP_PyObjectPlus *self_v,
                                               const EXP_PYATTRIBUTE_DEF *attrdef,
                                               PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

  /* if value is not a sequence PyOrientationTo makes an error */
  MT_Matrix3x3 rot;
  if (!PyOrientationTo(value, rot, "gameOb.localOrientation = sequence: KX_GameObject, "))
    return PY_SET_ATTR_FAIL;

  self->NodeSetLocalOrientation(rot);
  self->NodeUpdateGS(0.f);
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_worldScaling(EXP_PyObjectPlus *self_v,
                                                 const EXP_PYATTRIBUTE_DEF *attrdef)
{
#  ifdef USE_MATHUTILS
  return Vector_CreatePyObject_cb(EXP_PROXY_FROM_REF_BORROW(self_v),
                                  3,
                                  mathutils_kxgameob_vector_cb_index,
                                  MATHUTILS_VEC_CB_SCALE_GLOBAL);
#  else
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyObjectFrom(self->NodeGetWorldScaling());
#  endif
}

int KX_GameObject::pyattr_set_worldScaling(EXP_PyObjectPlus *self_v,
                                           const EXP_PYATTRIBUTE_DEF *attrdef,
                                           PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  MT_Vector3 scale;
  if (!PyVecTo(value, scale))
    return PY_SET_ATTR_FAIL;

  self->NodeSetWorldScale(scale);
  self->NodeUpdateGS(0.f);
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_localScaling(EXP_PyObjectPlus *self_v,
                                                 const EXP_PYATTRIBUTE_DEF *attrdef)
{
#  ifdef USE_MATHUTILS
  return Vector_CreatePyObject_cb(EXP_PROXY_FROM_REF_BORROW(self_v),
                                  3,
                                  mathutils_kxgameob_vector_cb_index,
                                  MATHUTILS_VEC_CB_SCALE_LOCAL);
#  else
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyObjectFrom(self->NodeGetLocalScaling());
#  endif
}

int KX_GameObject::pyattr_set_localScaling(EXP_PyObjectPlus *self_v,
                                           const EXP_PYATTRIBUTE_DEF *attrdef,
                                           PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  MT_Vector3 scale;
  if (!PyVecTo(value, scale))
    return PY_SET_ATTR_FAIL;

  self->NodeSetLocalScale(scale);
  self->NodeUpdateGS(0.f);
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_localTransform(EXP_PyObjectPlus *self_v,
                                                   const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

  return PyObjectFrom(self->NodeGetLocalTransform().toMatrix());
}

int KX_GameObject::pyattr_set_localTransform(EXP_PyObjectPlus *self_v,
                                             const EXP_PYATTRIBUTE_DEF *attrdef,
                                             PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  MT_Matrix4x4 temp;
  if (!PyMatTo(value, temp))
    return PY_SET_ATTR_FAIL;

  float transform[4][4];
  float loc[3], size[3];
  float rot[3][3];
  MT_Matrix3x3 orientation;

  temp.getValue(*transform);
  mat4_to_loc_rot_size(loc, rot, size, transform);

  self->NodeSetLocalPosition(MT_Vector3(loc));

  // MT_Matrix3x3's constructor expects a 4x4 matrix
  orientation = MT_Matrix3x3();
  orientation.setValue3x3(*rot);
  self->NodeSetLocalOrientation(orientation);

  self->NodeSetLocalScale(MT_Vector3(size));

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_worldTransform(EXP_PyObjectPlus *self_v,
                                                   const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

  return PyObjectFrom(MT_Matrix4x4(self->NodeGetWorldTransform()));
}

int KX_GameObject::pyattr_set_worldTransform(EXP_PyObjectPlus *self_v,
                                             const EXP_PYATTRIBUTE_DEF *attrdef,
                                             PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  MT_Matrix4x4 temp;
  if (!PyMatTo(value, temp))
    return PY_SET_ATTR_FAIL;

  float transform[4][4];
  float loc[3], size[3];
  float rot[3][3];
  MT_Matrix3x3 orientation;

  temp.getValue(*transform);
  mat4_to_loc_rot_size(loc, rot, size, transform);

  self->NodeSetWorldPosition(MT_Vector3(loc));

  // MT_Matrix3x3's constructor expects a 4x4 matrix
  orientation = MT_Matrix3x3();
  orientation.setValue3x3(*rot);
  self->NodeSetGlobalOrientation(orientation);

  self->NodeSetWorldScale(MT_Vector3(size));

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_worldLinearVelocity(EXP_PyObjectPlus *self_v,
                                                        const EXP_PYATTRIBUTE_DEF *attrdef)
{
#  ifdef USE_MATHUTILS
  return Vector_CreatePyObject_cb(EXP_PROXY_FROM_REF_BORROW(self_v),
                                  3,
                                  mathutils_kxgameob_vector_cb_index,
                                  MATHUTILS_VEC_CB_LINVEL_GLOBAL);
#  else
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyObjectFrom(GetLinearVelocity(false));
#  endif
}

int KX_GameObject::pyattr_set_worldLinearVelocity(EXP_PyObjectPlus *self_v,
                                                  const EXP_PYATTRIBUTE_DEF *attrdef,
                                                  PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  MT_Vector3 velocity;
  if (!PyVecTo(value, velocity))
    return PY_SET_ATTR_FAIL;

  self->setLinearVelocity(velocity, false);

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_localLinearVelocity(EXP_PyObjectPlus *self_v,
                                                        const EXP_PYATTRIBUTE_DEF *attrdef)
{
#  ifdef USE_MATHUTILS
  return Vector_CreatePyObject_cb(EXP_PROXY_FROM_REF_BORROW(self_v),
                                  3,
                                  mathutils_kxgameob_vector_cb_index,
                                  MATHUTILS_VEC_CB_LINVEL_LOCAL);
#  else
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyObjectFrom(GetLinearVelocity(true));
#  endif
}

int KX_GameObject::pyattr_set_localLinearVelocity(EXP_PyObjectPlus *self_v,
                                                  const EXP_PYATTRIBUTE_DEF *attrdef,
                                                  PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  MT_Vector3 velocity;
  if (!PyVecTo(value, velocity))
    return PY_SET_ATTR_FAIL;

  self->setLinearVelocity(velocity, true);

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_worldAngularVelocity(EXP_PyObjectPlus *self_v,
                                                         const EXP_PYATTRIBUTE_DEF *attrdef)
{
#  ifdef USE_MATHUTILS
  return Vector_CreatePyObject_cb(EXP_PROXY_FROM_REF_BORROW(self_v),
                                  3,
                                  mathutils_kxgameob_vector_cb_index,
                                  MATHUTILS_VEC_CB_ANGVEL_GLOBAL);
#  else
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyObjectFrom(GetAngularVelocity(false));
#  endif
}

int KX_GameObject::pyattr_set_worldAngularVelocity(EXP_PyObjectPlus *self_v,
                                                   const EXP_PYATTRIBUTE_DEF *attrdef,
                                                   PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  MT_Vector3 velocity;
  if (!PyVecTo(value, velocity))
    return PY_SET_ATTR_FAIL;

  self->setAngularVelocity(velocity, false);

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_localAngularVelocity(EXP_PyObjectPlus *self_v,
                                                         const EXP_PYATTRIBUTE_DEF *attrdef)
{
#  ifdef USE_MATHUTILS
  return Vector_CreatePyObject_cb(EXP_PROXY_FROM_REF_BORROW(self_v),
                                  3,
                                  mathutils_kxgameob_vector_cb_index,
                                  MATHUTILS_VEC_CB_ANGVEL_LOCAL);
#  else
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyObjectFrom(GetAngularVelocity(true));
#  endif
}

int KX_GameObject::pyattr_set_localAngularVelocity(EXP_PyObjectPlus *self_v,
                                                   const EXP_PYATTRIBUTE_DEF *attrdef,
                                                   PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  MT_Vector3 velocity;
  if (!PyVecTo(value, velocity))
    return PY_SET_ATTR_FAIL;

  self->setAngularVelocity(velocity, true);

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_gravity(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef)
{
#  ifdef USE_MATHUTILS
  return Vector_CreatePyObject_cb(EXP_PROXY_FROM_REF_BORROW(self_v),
                                  3,
                                  mathutils_kxgameob_vector_cb_index,
                                  MATHUTILS_VEC_CB_GRAVITY);
#  else
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyObjectFrom(GetGravity());
#  endif
}

int KX_GameObject::pyattr_set_gravity(EXP_PyObjectPlus *self_v,
                                      const EXP_PYATTRIBUTE_DEF *attrdef,
                                      PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  MT_Vector3 gravity;
  if (!PyVecTo(value, gravity))
    return PY_SET_ATTR_FAIL;

  self->SetGravity(gravity);

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_linearDamping(EXP_PyObjectPlus *self_v,
                                                  const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyFloat_FromDouble(self->getLinearDamping());
}

int KX_GameObject::pyattr_set_linearDamping(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef,
                                            PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  float val = PyFloat_AsDouble(value);
  self->setLinearDamping(val);
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_angularDamping(EXP_PyObjectPlus *self_v,
                                                   const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyFloat_FromDouble(self->getAngularDamping());
}

int KX_GameObject::pyattr_set_angularDamping(EXP_PyObjectPlus *self_v,
                                             const EXP_PYATTRIBUTE_DEF *attrdef,
                                             PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  float val = PyFloat_AsDouble(value);
  self->setAngularDamping(val);
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_timeOffset(EXP_PyObjectPlus *self_v,
                                               const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  SG_Node *sg_parent;
  if ((sg_parent = self->GetSGNode()->GetSGParent()) != nullptr && sg_parent->IsSlowParent()) {
    return PyFloat_FromDouble(
        static_cast<KX_SlowParentRelation *>(sg_parent->GetParentRelation())->GetTimeOffset());
  }
  else {
    return PyFloat_FromDouble(0.0f);
  }
}

int KX_GameObject::pyattr_set_timeOffset(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef,
                                         PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  MT_Scalar val = PyFloat_AsDouble(value);
  SG_Node *sg_parent = self->GetSGNode()->GetSGParent();
  if (val < 0.0f) { /* also accounts for non float */
    PyErr_SetString(PyExc_AttributeError,
                    "gameOb.timeOffset = float: KX_GameObject, expected a float zero or above");
    return PY_SET_ATTR_FAIL;
  }
  if (sg_parent && sg_parent->IsSlowParent())
    static_cast<KX_SlowParentRelation *>(sg_parent->GetParentRelation())->SetTimeOffset(val);
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_state(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  int state = 0;
  state |= self->GetState();
  return PyLong_FromLong(state);
}

int KX_GameObject::pyattr_set_state(EXP_PyObjectPlus *self_v,
                                    const EXP_PYATTRIBUTE_DEF *attrdef,
                                    PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  int state_i = PyLong_AsLong(value);
  unsigned int state = 0;

  if (state_i == -1 && PyErr_Occurred()) {
    PyErr_SetString(PyExc_TypeError,
                    "gameOb.state = int: KX_GameObject, expected an int bit field");
    return PY_SET_ATTR_FAIL;
  }

  state |= state_i;
  if ((state & ((1 << 30) - 1)) == 0) {
    PyErr_SetString(PyExc_AttributeError,
                    "gameOb.state = int: KX_GameObject, state bitfield was not between 0 and 30 "
                    "(1<<0 and 1<<29)");
    return PY_SET_ATTR_FAIL;
  }
  self->SetState(state);
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_meshes(EXP_PyObjectPlus *self_v,
                                           const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  PyObject *meshes = PyList_New(self->m_meshes.size());
  int i;

  for (i = 0; i < (int)self->m_meshes.size(); i++) {
    KX_MeshProxy *meshproxy = new KX_MeshProxy(self->m_meshes[i]);
    PyList_SET_ITEM(meshes, i, meshproxy->NewProxy(true));
  }

  return meshes;
}

PyObject *KX_GameObject::pyattr_get_obcolor(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef)
{
#  ifdef USE_MATHUTILS
  return Vector_CreatePyObject_cb(EXP_PROXY_FROM_REF_BORROW(self_v),
                                  4,
                                  mathutils_kxgameob_vector_cb_index,
                                  MATHUTILS_VEC_CB_OBJECT_COLOR);
#  else
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  return PyObjectFrom(self->GetObjectColor());
#  endif
}

int KX_GameObject::pyattr_set_obcolor(EXP_PyObjectPlus *self_v,
                                      const EXP_PYATTRIBUTE_DEF *attrdef,
                                      PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  MT_Vector4 obcolor;
  if (!PyVecTo(value, obcolor))
    return PY_SET_ATTR_FAIL;
  self->SetObjectColor(obcolor);
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_components(EXP_PyObjectPlus *self_v,
                                               const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  EXP_ListValue<KX_PythonComponent> *components = self->GetComponents();
  return components ? components->GetProxy() :
                      (new EXP_ListValue<KX_PythonComponent>())->NewProxy(true);
}

static int kx_game_object_get_sensors_size_cb(void *self_v)
{
  return ((KX_GameObject *)self_v)->GetSensors().size();
}

static PyObject *kx_game_object_get_sensors_item_cb(void *self_v, int index)
{
  return ((KX_GameObject *)self_v)->GetSensors()[index]->GetProxy();
}

static const std::string kx_game_object_get_sensors_item_name_cb(void *self_v, int index)
{
  return ((KX_GameObject *)self_v)->GetSensors()[index]->GetName();
}

/* These are experimental! */
PyObject *KX_GameObject::pyattr_get_sensors(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef)
{
  return (new EXP_ListWrapper(self_v,
                              ((KX_GameObject *)self_v)->GetProxy(),
                              nullptr,
                              kx_game_object_get_sensors_size_cb,
                              kx_game_object_get_sensors_item_cb,
                              kx_game_object_get_sensors_item_name_cb,
                              nullptr))
      ->NewProxy(true);
}

static int kx_game_object_get_controllers_size_cb(void *self_v)
{
  return ((KX_GameObject *)self_v)->GetControllers().size();
}

static PyObject *kx_game_object_get_controllers_item_cb(void *self_v, int index)
{
  return ((KX_GameObject *)self_v)->GetControllers()[index]->GetProxy();
}

static const std::string kx_game_object_get_controllers_item_name_cb(void *self_v, int index)
{
  return ((KX_GameObject *)self_v)->GetControllers()[index]->GetName();
}

PyObject *KX_GameObject::pyattr_get_controllers(EXP_PyObjectPlus *self_v,
                                                const EXP_PYATTRIBUTE_DEF *attrdef)
{
  return (new EXP_ListWrapper(self_v,
                              ((KX_GameObject *)self_v)->GetProxy(),
                              nullptr,
                              kx_game_object_get_controllers_size_cb,
                              kx_game_object_get_controllers_item_cb,
                              kx_game_object_get_controllers_item_name_cb,
                              nullptr))
      ->NewProxy(true);
}

static int kx_game_object_get_actuators_size_cb(void *self_v)
{
  return ((KX_GameObject *)self_v)->GetActuators().size();
}

static PyObject *kx_game_object_get_actuators_item_cb(void *self_v, int index)
{
  return ((KX_GameObject *)self_v)->GetActuators()[index]->GetProxy();
}

static const std::string kx_game_object_get_actuators_item_name_cb(void *self_v, int index)
{
  return ((KX_GameObject *)self_v)->GetActuators()[index]->GetName();
}

PyObject *KX_GameObject::pyattr_get_actuators(EXP_PyObjectPlus *self_v,
                                              const EXP_PYATTRIBUTE_DEF *attrdef)
{
  return (new EXP_ListWrapper(self_v,
                              ((KX_GameObject *)self_v)->GetProxy(),
                              nullptr,
                              kx_game_object_get_actuators_size_cb,
                              kx_game_object_get_actuators_item_cb,
                              kx_game_object_get_actuators_item_name_cb,
                              nullptr))
      ->NewProxy(true);
}
/* End experimental */

PyObject *KX_GameObject::pyattr_get_children(EXP_PyObjectPlus *self_v,
                                             const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  EXP_ListValue<KX_GameObject> *list = new EXP_ListValue<KX_GameObject>(self->GetChildren());
  /* The list must not own any data because is temporary and we can't
   * ensure that it will freed before item's in it (e.g python owner). */
  list->SetReleaseOnDestruct(false);
  return list->NewProxy(true);
}

PyObject *KX_GameObject::pyattr_get_children_recursive(EXP_PyObjectPlus *self_v,
                                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  EXP_ListValue<KX_GameObject> *list = new EXP_ListValue<KX_GameObject>(
      self->GetChildrenRecursive());
  /* The list must not own any data because is temporary and we can't
   * ensure that it will freed before item's in it (e.g python owner). */
  list->SetReleaseOnDestruct(false);
  return list->NewProxy(true);
}

PyObject *KX_GameObject::pyattr_get_attrDict(EXP_PyObjectPlus *self_v,
                                             const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

  if (self->m_attr_dict == nullptr)
    self->m_attr_dict = PyDict_New();

  Py_INCREF(self->m_attr_dict);
  return self->m_attr_dict;
}

PyObject *KX_GameObject::pyattr_get_debug(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

  return PyBool_FromLong(self->GetScene()->ObjectInDebugList(self));
}

int KX_GameObject::pyattr_set_debug(EXP_PyObjectPlus *self_v,
                                    const EXP_PYATTRIBUTE_DEF *attrdef,
                                    PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  int param = PyObject_IsTrue(value);

  if (param == -1) {
    PyErr_SetString(PyExc_AttributeError,
                    "gameOb.debug = bool: KX_GameObject, expected True or False");
    return PY_SET_ATTR_FAIL;
  }

  self->SetUseDebugProperties(param, false);

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_debugRecursive(EXP_PyObjectPlus *self_v,
                                                   const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

  return PyBool_FromLong(self->GetScene()->ObjectInDebugList(self));
}

int KX_GameObject::pyattr_set_debugRecursive(EXP_PyObjectPlus *self_v,
                                             const EXP_PYATTRIBUTE_DEF *attrdef,
                                             PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  int param = PyObject_IsTrue(value);

  if (param == -1) {
    PyErr_SetString(PyExc_AttributeError,
                    "gameOb.debugRecursive = bool: KX_GameObject, expected True or False");
    return PY_SET_ATTR_FAIL;
  }

  self->SetUseDebugProperties(param, true);

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_lodManager(EXP_PyObjectPlus *self_v,
                                               const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

  return (self->m_lodManager) ? self->m_lodManager->GetProxy() : Py_None;
}

int KX_GameObject::pyattr_set_lodManager(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef,
                                         PyObject *value)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

  KX_LodManager *lodManager = nullptr;
  if (!ConvertPythonToLodManager(value, &lodManager, true, "gameobj.lodManager: KX_GameObject")) {
    return PY_SET_ATTR_FAIL;
  }

  self->SetLodManager(lodManager);
  self->GetScene()->AddObjToLodObjList(self);

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::PyApplyForce(PyObject *args)
{
  int local = 0;
  PyObject *pyvect;

  if (PyArg_ParseTuple(args, "O|i:applyForce", &pyvect, &local)) {
    MT_Vector3 force;
    if (PyVecTo(pyvect, force)) {
      ApplyForce(force, (local != 0));
      Py_RETURN_NONE;
    }
  }
  return nullptr;
}

PyObject *KX_GameObject::PyApplyTorque(PyObject *args)
{
  int local = 0;
  PyObject *pyvect;

  if (PyArg_ParseTuple(args, "O|i:applyTorque", &pyvect, &local)) {
    MT_Vector3 torque;
    if (PyVecTo(pyvect, torque)) {
      ApplyTorque(torque, (local != 0));
      Py_RETURN_NONE;
    }
  }
  return nullptr;
}

PyObject *KX_GameObject::PyApplyRotation(PyObject *args)
{
  int local = 0;
  PyObject *pyvect;

  if (PyArg_ParseTuple(args, "O|i:applyRotation", &pyvect, &local)) {
    MT_Vector3 rotation;
    if (PyVecTo(pyvect, rotation)) {
      ApplyRotation(rotation, (local != 0));
      Py_RETURN_NONE;
    }
  }
  return nullptr;
}

PyObject *KX_GameObject::PyApplyMovement(PyObject *args)
{
  int local = 0;
  PyObject *pyvect;

  if (PyArg_ParseTuple(args, "O|i:applyMovement", &pyvect, &local)) {
    MT_Vector3 movement;
    if (PyVecTo(pyvect, movement)) {
      ApplyMovement(movement, (local != 0));
      Py_RETURN_NONE;
    }
  }
  return nullptr;
}

PyObject *KX_GameObject::PyGetLinearVelocity(PyObject *args)
{
  // only can get the velocity if we have a physics object connected to us...
  int local = 0;
  if (PyArg_ParseTuple(args, "|i:getLinearVelocity", &local)) {
    return PyObjectFrom(GetLinearVelocity((local != 0)));
  }
  else {
    return nullptr;
  }
}

PyObject *KX_GameObject::PySetLinearVelocity(PyObject *args)
{
  int local = 0;
  PyObject *pyvect;

  if (PyArg_ParseTuple(args, "O|i:setLinearVelocity", &pyvect, &local)) {
    MT_Vector3 velocity;
    if (PyVecTo(pyvect, velocity)) {
      setLinearVelocity(velocity, (local != 0));
      Py_RETURN_NONE;
    }
  }
  return nullptr;
}

PyObject *KX_GameObject::PyGetAngularVelocity(PyObject *args)
{
  // only can get the velocity if we have a physics object connected to us...
  int local = 0;
  if (PyArg_ParseTuple(args, "|i:getAngularVelocity", &local)) {
    return PyObjectFrom(GetAngularVelocity((local != 0)));
  }
  else {
    return nullptr;
  }
}

PyObject *KX_GameObject::PySetAngularVelocity(PyObject *args)
{
  int local = 0;
  PyObject *pyvect;

  if (PyArg_ParseTuple(args, "O|i:setAngularVelocity", &pyvect, &local)) {
    MT_Vector3 velocity;
    if (PyVecTo(pyvect, velocity)) {
      setAngularVelocity(velocity, (local != 0));
      Py_RETURN_NONE;
    }
  }
  return nullptr;
}

PyObject *KX_GameObject::PySetDamping(PyObject *args)
{
  float linear;
  float angular;

  if (!PyArg_ParseTuple(args, "ff:setDamping", &linear, &angular)) {
    return nullptr;
  }

  setDamping(linear, angular);
  Py_RETURN_NONE;
}

PyObject *KX_GameObject::PySetCcdMotionThreshold(PyObject *args)
{
  float motion_threshold;

  if (!PyArg_ParseTuple(args, "f:setCcdMotionThreshold", &motion_threshold)) {
    return nullptr;
  }

  if ((motion_threshold < 0.0f) || (motion_threshold > 100.0f)) {
    PyErr_SetString(PyExc_TypeError,
                    "gameOb.setCcdMotionThreshold: KX_GameObject, "
                    "expected a float in range 0.0 - 100.0");
    return nullptr;
  }

  setCcdMotionThreshold(motion_threshold);
  Py_RETURN_NONE;
}

PyObject *KX_GameObject::PySetCcdSweptSphereRadius(PyObject *args)
{
  float swept_sphere_radius;

  if (!PyArg_ParseTuple(args, "f:setCcdSweptSphereRadius", &swept_sphere_radius)) {
    return nullptr;
  }

  if ((swept_sphere_radius < 0.0f) || (swept_sphere_radius > 10.0f)) {
    PyErr_SetString(PyExc_TypeError,
                    "gameOb.setCcdSweptSphereRadius: KX_GameObject, "
                    "expected a float in range 0.0 - 10.0");
    return nullptr;
  }

  setCcdSweptSphereRadius(swept_sphere_radius);
  Py_RETURN_NONE;
}

PyObject *KX_GameObject::PySetVisible(PyObject *args)
{
  int visible, recursive = 0;
  if (!PyArg_ParseTuple(args, "i|i:setVisible", &visible, &recursive)) {
    return nullptr;
  }

  SetVisible(visible ? true : false, recursive ? true : false);
  Py_RETURN_NONE;
}

PyObject *KX_GameObject::PySetOcclusion(PyObject *args)
{
  int occlusion, recursive = 0;
  if (!PyArg_ParseTuple(args, "i|i:setOcclusion", &occlusion, &recursive)) {
    return nullptr;
  }

  SetOccluder(occlusion ? true : false, recursive ? true : false);
  Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyGetVelocity(PyObject *args)
{
  // only can get the velocity if we have a physics object connected to us...
  MT_Vector3 point(0.0f, 0.0f, 0.0f);
  PyObject *pypos = nullptr;

  if (!PyArg_ParseTuple(args, "|O:getVelocity", &pypos) || (pypos && !PyVecTo(pypos, point))) {
    return nullptr;
  }

  return PyObjectFrom(GetVelocity(point));
}

PyObject *KX_GameObject::PyGetReactionForce()
{
  // only can get the velocity if we have a physics object connected to us...

  // XXX - Currently not working with bullet intergration, see KX_BulletPhysicsController.cpp's
  // getReactionForce
#  if 0
	if (GetPhysicsController1())
		return PyObjectFrom(GetPhysicsController1()->getReactionForce());
	return PyObjectFrom(dummy_point);
#  endif

  return PyObjectFrom(MT_Vector3(0.0f, 0.0f, 0.0f));
}

PyObject *KX_GameObject::PyEnableRigidBody()
{
  if (GetPhysicsController())
    GetPhysicsController()->SetRigidBody(true);

  Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyDisableRigidBody()
{
  if (GetPhysicsController())
    GetPhysicsController()->SetRigidBody(false);

  Py_RETURN_NONE;
}

PyObject *KX_GameObject::PySetParent(PyObject *args, PyObject *kwds)
{
  SCA_LogicManager *logicmgr = GetScene()->GetLogicManager();
  PyObject *pyobj;
  KX_GameObject *obj;
  int addToCompound = 1, ghost = 1;

  static const char *kwlist[] = {"parent", "compound", "ghost", nullptr};
  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwds,
                                   "O|ii:setParent",
                                   const_cast<char **>(kwlist),
                                   &pyobj,
                                   &addToCompound,
                                   &ghost))
  {
    return nullptr;  // Python sets a simple error
  }
  if (!ConvertPythonToGameObject(
          logicmgr, pyobj, &obj, true, "gameOb.setParent(obj): KX_GameObject"))
    return nullptr;

  if (obj)
    SetParent(obj, addToCompound, ghost);
  Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyRemoveParent()
{
  RemoveParent();
  Py_RETURN_NONE;
}

PyObject *KX_GameObject::PySetCollisionMargin(PyObject *value)
{
  float collisionMargin = PyFloat_AsDouble(value);

  if (collisionMargin == -1 && PyErr_Occurred()) {
    PyErr_SetString(PyExc_TypeError, "expected a float");
    return nullptr;
  }

  PYTHON_CHECK_PHYSICS_CONTROLLER(this, "setCollisionMargin", nullptr);

  m_pPhysicsController->SetMargin(collisionMargin);
  Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyCollide(PyObject *value)
{
  KX_Scene *scene = GetScene();
  KX_GameObject *other;

  if (!ConvertPythonToGameObject(
          scene->GetLogicManager(), value, &other, false, "gameOb.collide(obj): KX_GameObject"))
  {
    return nullptr;
  }

  if (!m_pPhysicsController || !other->GetPhysicsController()) {
    PyErr_SetString(PyExc_TypeError, "expected objects with physics controller");
    return nullptr;
  }

  PHY_IPhysicsEnvironment *env = scene->GetPhysicsEnvironment();
  PHY_CollisionTestResult testResult = env->CheckCollision(m_pPhysicsController,
                                                           other->GetPhysicsController());

  PyObject *result = PyTuple_New(2);
  if (!testResult.collide) {
    PyTuple_SET_ITEM(result, 0, Py_False);
    PyTuple_SET_ITEM(result, 1, Py_None);
    Py_INCREF(Py_False);
    Py_INCREF(Py_None);
  }
  else {
    PyTuple_SET_ITEM(result, 0, Py_True);
    Py_INCREF(Py_True);

    if (testResult.collData) {
      KX_CollisionContactPointList *contactPointList = new KX_CollisionContactPointList(
          testResult.collData, testResult.isFirst);
      contactPointList->SetOwnsCollData(true); /* Transfer ownership; Python GC will free it. */
      PyTuple_SET_ITEM(result, 1, contactPointList->NewProxy(true));
    }
    else {
      PyTuple_SET_ITEM(result, 1, Py_None);
      Py_INCREF(Py_None);
    }
  }

  return result;
}

PyObject *KX_GameObject::PyApplyImpulse(PyObject *args)
{
  PyObject *pyattach;
  PyObject *pyimpulse;
  int local = 0;

  PYTHON_CHECK_PHYSICS_CONTROLLER(this, "applyImpulse", nullptr);

  if (PyArg_ParseTuple(args, "OO|i:applyImpulse", &pyattach, &pyimpulse, &local)) {
    MT_Vector3 attach;
    MT_Vector3 impulse;
    if (PyVecTo(pyattach, attach) && PyVecTo(pyimpulse, impulse)) {
      m_pPhysicsController->ApplyImpulse(attach, impulse, (local != 0));
      Py_RETURN_NONE;
    }
  }

  return nullptr;
}

PyObject *KX_GameObject::PySuspendPhysics(PyObject *args)
{
  int freeConstraints = false;

  if (!PyArg_ParseTuple(args, "|i:suspendPhysics", &freeConstraints)) {
    return nullptr;
  }

  if (GetPhysicsController()) {
    GetPhysicsController()->SuspendPhysics((bool)freeConstraints);
  }
  Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyRestorePhysics()
{
  if (GetPhysicsController()) {
    GetPhysicsController()->RestorePhysics();
  }
  Py_RETURN_NONE;
}

PyObject *KX_GameObject::PySuspendDynamics(PyObject *args)
{
  bool ghost = false;

  if (!PyArg_ParseTuple(args, "|b", &ghost)) {
    return nullptr;
  }

  if (GetPhysicsController())
    GetPhysicsController()->SuspendDynamics(ghost);

  Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyRestoreDynamics()
{
  // Child objects must be static, so we block changing to dynamic
  if (GetPhysicsController() && !GetParent())
    GetPhysicsController()->RestoreDynamics();
  Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyAlignAxisToVect(PyObject *args, PyObject *kwds)
{
  PyObject *pyvect;
  int axis = 2;  // z axis is the default
  float fac = 1.0f;

  static const char *kwlist[] = {"vect", "axis", "factor", nullptr};
  if (PyArg_ParseTupleAndKeywords(
          args, kwds, "O|if:alignAxisToVect", const_cast<char **>(kwlist), &pyvect, &axis, &fac))
  {
    MT_Vector3 vect;
    if (PyVecTo(pyvect, vect)) {
      if (fac > 0.0f) {
        if (fac > 1.0f)
          fac = 1.0f;

        AlignAxisToVect(vect, axis, fac);
        NodeUpdateGS(0.f);
      }
      Py_RETURN_NONE;
    }
  }
  return nullptr;
}

PyObject *KX_GameObject::PyGetAxisVect(PyObject *value)
{
  MT_Vector3 vect;
  if (PyVecTo(value, vect)) {
    return PyObjectFrom(NodeGetWorldOrientation() * vect);
  }
  return nullptr;
}

PyObject *KX_GameObject::PyGetPhysicsId()
{
  PHY_IPhysicsController *ctrl = GetPhysicsController();
  unsigned long long physid = 0;
  if (ctrl) {
    physid = (unsigned long long)ctrl;
  }
  return PyLong_FromUnsignedLongLong(physid);
}

PyObject *KX_GameObject::PyGetPropertyNames()
{
  PyObject *list = ConvertKeysToPython();

  if (m_attr_dict) {
    PyObject *key, *value;
    Py_ssize_t pos = 0;

    while (PyDict_Next(m_attr_dict, &pos, &key, &value)) {
      PyList_Append(list, key);
    }
  }
  return list;
}

PyObject *KX_GameObject::pyattr_get_blender_object(EXP_PyObjectPlus *self_v,
                                                   const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
  blender::Object *ob = self->GetBlenderObject();
  if (ob) {
    PyObject *py_blender_object = pyrna_id_CreatePyObject(&ob->id);
    return py_blender_object;
  }
  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC_O(KX_GameObject,
                      getDistanceTo,
                      "getDistanceTo(other): get distance to another point/KX_GameObject")
{
  MT_Vector3 b;
  if (PyVecTo(value, b)) {
    return PyFloat_FromDouble(NodeGetWorldPosition().distance(b));
  }
  PyErr_Clear();

  SCA_LogicManager *logicmgr = GetScene()->GetLogicManager();
  KX_GameObject *other;
  if (ConvertPythonToGameObject(
          logicmgr, value, &other, false, "gameOb.getDistanceTo(value): KX_GameObject"))
  {
    return PyFloat_FromDouble(NodeGetWorldPosition().distance(other->NodeGetWorldPosition()));
  }

  return nullptr;
}

EXP_PYMETHODDEF_DOC_O(
    KX_GameObject,
    getVectTo,
    "getVectTo(other): get vector and the distance to another point/KX_GameObject\n"
    "Returns a 3-tuple with (distance,worldVector,localVector)\n")
{
  MT_Vector3 toPoint, fromPoint;
  MT_Vector3 toDir, locToDir;
  MT_Scalar distance;

  SCA_LogicManager *logicmgr = GetScene()->GetLogicManager();
  PyObject *returnValue;

  if (!PyVecTo(value, toPoint)) {
    PyErr_Clear();

    KX_GameObject *other;
    if (ConvertPythonToGameObject(
            logicmgr, value, &other, false, "")) /* error will be overwritten */
    {
      toPoint = other->NodeGetWorldPosition();
    }
    else {
      PyErr_SetString(
          PyExc_TypeError,
          "gameOb.getVectTo(other): KX_GameObject, expected a 3D Vector or KX_GameObject type");
      return nullptr;
    }
  }

  fromPoint = NodeGetWorldPosition();
  toDir = toPoint - fromPoint;
  distance = toDir.length();

  if (MT_fuzzyZero(distance)) {
    locToDir = toDir = MT_Vector3(0.0f, 0.0f, 0.0f);
    distance = 0.0f;
  }
  else {
    toDir.normalize();
    locToDir = toDir * NodeGetWorldOrientation();
  }

  returnValue = PyTuple_New(3);
  if (returnValue) {  // very unlikely to fail, python sets a memory error here.
    PyTuple_SET_ITEM(returnValue, 0, PyFloat_FromDouble(distance));
    PyTuple_SET_ITEM(returnValue, 1, PyObjectFrom(toDir));
    PyTuple_SET_ITEM(returnValue, 2, PyObjectFrom(locToDir));
  }
  return returnValue;
}

KX_GameObject::RayCastData::RayCastData(const std::string &prop, bool xray, unsigned int mask)
    : m_prop(prop), m_xray(xray), m_mask(mask), m_hitObject(nullptr)
{
}

static bool CheckRayCastObject(KX_GameObject *obj, KX_GameObject::RayCastData *rayData)
{
  const std::string &prop = rayData->m_prop;
  const unsigned int mask = rayData->m_mask;
  // Check if the object had a given property (if this one is non empty) and have the correct group
  // mask (if this one is different from 0xFFFF).
  return ((prop.empty() || obj->GetProperty(prop)) &&
          (mask == ((1u << OB_MAX_COL_MASKS) - 1) || obj->GetCollisionGroup() & mask));
}

bool KX_GameObject::RayHit(KX_ClientObjectInfo *client, KX_RayCast *result, RayCastData *rayData)
{
  KX_GameObject *obj = client->m_gameobject;

  // if X-ray option is selected, the unwanted objects were not tested, so get here only with true
  // hit if not, all objects were tested and the front one may not be the correct one.
  if (rayData->m_xray || CheckRayCastObject(obj, rayData)) {
    rayData->m_hitObject = obj;
  }
  // return true to stop RayCast::RayTest from looping, the above test was decisive
  // We would want to loop only if we want to get more than one hit point
  return true;
}

/* this function is used to pre-filter the object before casting the ray on them.
 * This is useful for "X-Ray" option when we want to see "through" unwanted object.
 */
bool KX_GameObject::NeedRayCast(KX_ClientObjectInfo *client, RayCastData *rayData)
{
  KX_GameObject *obj = client->m_gameobject;

  // if X-Ray option is selected, skip object that don't match the criteria as we see through them
  // if not, test all objects because we don't know yet which one will be on front
  return (!rayData->m_xray || CheckRayCastObject(obj, rayData));
}

EXP_PYMETHODDEF_DOC(
    KX_GameObject,
    rayCastTo,
    "rayCastTo(other,dist,prop): look towards another point/KX_GameObject and return first object "
    "hit within dist that matches prop\n"
    " prop = property name that object must have; can be omitted => detect any object\n"
    " dist = max distance to look (can be negative => look behind); 0 or omitted => detect up to "
    "other\n"
    " other = 3-tuple or object reference")
{
  MT_Vector3 toPoint;
  PyObject *pyarg;
  float dist = 0.0f;
  const char *propName = "";
  SCA_LogicManager *logicmgr = GetScene()->GetLogicManager();

  static const char *kwlist[] = {"other", "dist", "prop", nullptr};
  if (!PyArg_ParseTupleAndKeywords(
          args, kwds, "O|fs:rayCastTo", const_cast<char **>(kwlist), &pyarg, &dist, &propName))
  {
    return nullptr;  // python sets simple error
  }

  if (!PyVecTo(pyarg, toPoint)) {
    KX_GameObject *other;
    PyErr_Clear();

    if (ConvertPythonToGameObject(
            logicmgr, pyarg, &other, false, "")) /* error will be overwritten */
    {
      toPoint = other->NodeGetWorldPosition();
    }
    else {
      PyErr_SetString(PyExc_TypeError,
                      "gameOb.rayCastTo(other,dist,prop): KX_GameObject, the first argument to "
                      "rayCastTo must be a vector or a KX_GameObject");
      return nullptr;
    }
  }
  MT_Vector3 fromPoint = NodeGetWorldPosition();

  if (dist != 0.0f)
    toPoint = fromPoint + dist * (toPoint - fromPoint).safe_normalized();

  PHY_IPhysicsEnvironment *pe = GetScene()->GetPhysicsEnvironment();
  PHY_IPhysicsController *spc = GetPhysicsController();
  KX_GameObject *parent = GetParent();
  if (!spc && parent)
    spc = parent->GetPhysicsController();

  RayCastData rayData(propName, false, (1u << OB_MAX_COL_MASKS) - 1);
  KX_RayCast::Callback<KX_GameObject, RayCastData> callback(this, spc, &rayData);
  if (KX_RayCast::RayTest(pe, fromPoint, toPoint, callback) && rayData.m_hitObject) {
    return rayData.m_hitObject->GetProxy();
  }

  Py_RETURN_NONE;
}

/* faster then Py_BuildValue since some scripts call raycast a lot */
static PyObject *none_tuple_3()
{
  PyObject *ret = PyTuple_New(3);
  PyTuple_SET_ITEM(ret, 0, Py_None);
  PyTuple_SET_ITEM(ret, 1, Py_None);
  PyTuple_SET_ITEM(ret, 2, Py_None);

  Py_INCREF(Py_None);
  Py_INCREF(Py_None);
  Py_INCREF(Py_None);
  return ret;
}
static PyObject *none_tuple_4()
{
  PyObject *ret = PyTuple_New(4);
  PyTuple_SET_ITEM(ret, 0, Py_None);
  PyTuple_SET_ITEM(ret, 1, Py_None);
  PyTuple_SET_ITEM(ret, 2, Py_None);
  PyTuple_SET_ITEM(ret, 3, Py_None);

  Py_INCREF(Py_None);
  Py_INCREF(Py_None);
  Py_INCREF(Py_None);
  Py_INCREF(Py_None);
  return ret;
}

static PyObject *none_tuple_5()
{
  PyObject *ret = PyTuple_New(5);
  PyTuple_SET_ITEM(ret, 0, Py_None);
  PyTuple_SET_ITEM(ret, 1, Py_None);
  PyTuple_SET_ITEM(ret, 2, Py_None);
  PyTuple_SET_ITEM(ret, 3, Py_None);
  PyTuple_SET_ITEM(ret, 4, Py_None);

  Py_INCREF(Py_None);
  Py_INCREF(Py_None);
  Py_INCREF(Py_None);
  Py_INCREF(Py_None);
  Py_INCREF(Py_None);
  return ret;
}

EXP_PYMETHODDEF_DOC(
    KX_GameObject,
    rayCast,
    "rayCast(to,from,dist,prop,face,xray,poly,mask): cast a ray and return 3-tuple "
    "(object,hit,normal) or 4-tuple (object,hit,normal,polygon) or 4-tuple "
    "(object,hit,normal,polygon,hituv) of contact point with object within dist that matches "
    "prop.\n"
    " If no hit, return (None,None,None) or (None,None,None,None) or (None,None,None,None,None).\n"
    " to   = 3-tuple or object reference for destination of ray (if object, use center of "
    "object)\n"
    " from = 3-tuple or object reference for origin of ray (if object, use center of object)\n"
    "        Can be None or omitted => start from self object center\n"
    " dist = max distance to look (can be negative => look behind); 0 or omitted => detect up to "
    "to\n"
    " prop = property name that object must have; can be omitted => detect any object\n"
    " face = normal option: 1=>return face normal; 0 or omitted => normal is oriented towards "
    "origin\n"
    " xray = X-ray option: 1=>skip objects that don't match prop; 0 or omitted => stop on first "
    "object\n"
    " poly = polygon option: 1=>return value is a 4-tuple and the 4th element is a KX_PolyProxy "
    "object\n"
    "                           which can be None if hit object has no mesh or if there is no "
    "hit\n"
    "                        2=>return value is a 5-tuple, the 4th element is the KX_PolyProxy "
    "object\n"
    "                           and the 5th element is the vector of UV coordinates at the hit "
    "point of the None if there is no UV mapping\n"
    "        If 0 or omitted, return value is a 3-tuple\n"
    " mask = collision mask: the collision mask that ray can hit, 0 < mask < 65536\n"
    "Note: The object on which you call this method matters: the ray will ignore it.\n"
    "      prop and xray option interact as follow:\n"
    "        prop off, xray off: return closest hit or no hit if there is no object on the full "
    "extend of the ray\n"
    "        prop off, xray on : idem\n"
    "        prop on,  xray off: return closest hit if it matches prop, no hit otherwise\n"
    "        prop on,  xray on : return closest hit matching prop or no hit if there is no object "
    "matching prop on the full extend of the ray\n")
{
  MT_Vector3 toPoint;
  MT_Vector3 fromPoint;
  PyObject *pyto;
  PyObject *pyfrom = Py_None;
  float dist = 0.0f;
  const char *propName = "";
  KX_GameObject *other;
  int face = 0, xray = 0, poly = 0;
  int mask = (1 << OB_MAX_COL_MASKS) - 1;
  SCA_LogicManager *logicmgr = GetScene()->GetLogicManager();

  static const char *kwlist[] = {
      "objto", "objfrom", "dist", "prop", "face", "xray", "poly", "mask", nullptr};
  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwds,
                                   "O|Ofsiiii:rayCast",
                                   const_cast<char **>(kwlist),
                                   &pyto,
                                   &pyfrom,
                                   &dist,
                                   &propName,
                                   &face,
                                   &xray,
                                   &poly,
                                   &mask))
  {
    return nullptr;  // Python sets a simple error
  }

  if (!PyVecTo(pyto, toPoint)) {
    PyErr_Clear();

    if (ConvertPythonToGameObject(
            logicmgr, pyto, &other, false, "")) /* error will be overwritten */
    {
      toPoint = other->NodeGetWorldPosition();
    }
    else {
      PyErr_SetString(PyExc_TypeError,
                      "the first argument to rayCast must be a vector or a KX_GameObject");
      return nullptr;
    }
  }
  if (pyfrom == Py_None) {
    fromPoint = NodeGetWorldPosition();
  }
  else if (!PyVecTo(pyfrom, fromPoint)) {
    PyErr_Clear();

    if (ConvertPythonToGameObject(
            logicmgr, pyfrom, &other, false, "")) /* error will be overwritten */
    {
      fromPoint = other->NodeGetWorldPosition();
    }
    else {
      PyErr_SetString(PyExc_TypeError,
                      "gameOb.rayCast(to,from,dist,prop,face,xray,poly,mask): KX_GameObject, the "
                      "second optional argument to rayCast must be a vector or a KX_GameObject");
      return nullptr;
    }
  }

  if (mask == 0 || mask & ~((1 << OB_MAX_COL_MASKS) - 1)) {
    PyErr_Format(PyExc_TypeError,
                 "gameOb.rayCast(to,from,dist,prop,face,xray,poly,mask): KX_GameObject, mask "
                 "argument to rayCast must be a int bitfield, 0 < mask < %i",
                 (1 << OB_MAX_COL_MASKS));
    return nullptr;
  }

  if (dist != 0.0f) {
    MT_Vector3 toDir = toPoint - fromPoint;
    if (MT_fuzzyZero(toDir)) {
      // return Py_BuildValue("OOO", Py_None, Py_None, Py_None);
      return none_tuple_3();
    }
    toDir.normalize();
    toPoint = fromPoint + (dist)*toDir;
  }
  else if (MT_fuzzyZero((toPoint - fromPoint))) {
    // return Py_BuildValue("OOO", Py_None, Py_None, Py_None);
    return none_tuple_3();
  }

  PHY_IPhysicsEnvironment *pe = GetScene()->GetPhysicsEnvironment();
  PHY_IPhysicsController *spc = GetPhysicsController();
  KX_GameObject *parent = GetParent();
  if (!spc && parent)
    spc = parent->GetPhysicsController();

  // to get the hit results
  RayCastData rayData(propName, xray, mask);
  KX_RayCast::Callback<KX_GameObject, RayCastData> callback(
      this, spc, &rayData, face, (poly == 2));

  if (KX_RayCast::RayTest(pe, fromPoint, toPoint, callback) && rayData.m_hitObject) {
    PyObject *returnValue = (poly == 2) ? PyTuple_New(5) :
                            (poly)      ? PyTuple_New(4) :
                                          PyTuple_New(3);
    if (returnValue) {  // unlikely this would ever fail, if it does python sets an error
      PyTuple_SET_ITEM(returnValue, 0, rayData.m_hitObject->GetProxy());
      PyTuple_SET_ITEM(returnValue, 1, PyObjectFrom(callback.m_hitPoint));
      PyTuple_SET_ITEM(returnValue, 2, PyObjectFrom(callback.m_hitNormal));
      if (poly) {
        if (callback.m_hitMesh) {
          KX_MeshProxy *meshProxy = new KX_MeshProxy(callback.m_hitMesh);
          // if this field is set, then we can trust that m_hitPolygon is a valid polygon
          RAS_Polygon *polygon = callback.m_hitMesh->GetPolygon(callback.m_hitPolygon);
          KX_PolyProxy *polyproxy = new KX_PolyProxy(meshProxy, callback.m_hitMesh, polygon);
          PyTuple_SET_ITEM(returnValue, 3, polyproxy->NewProxy(true));
          if (poly == 2) {
            if (callback.m_hitUVOK)
              PyTuple_SET_ITEM(returnValue, 4, PyObjectFrom(callback.m_hitUV));
            else {
              Py_INCREF(Py_None);
              PyTuple_SET_ITEM(returnValue, 4, Py_None);
            }
          }
        }
        else {
          Py_INCREF(Py_None);
          PyTuple_SET_ITEM(returnValue, 3, Py_None);
          if (poly == 2) {
            Py_INCREF(Py_None);
            PyTuple_SET_ITEM(returnValue, 4, Py_None);
          }
        }
      }
    }
    return returnValue;
  }
  // no hit
  if (poly == 2)
    return none_tuple_5();
  else if (poly)
    return none_tuple_4();
  else
    return none_tuple_3();
}

EXP_PYMETHODDEF_DOC(KX_GameObject,
                    sendMessage,
                    "sendMessage(subject, [body, to])\n"
                    "sends a message in same manner as a message actuator"
                    "subject = Subject of the message (string)"
                    "body = Message body (string)"
                    "to = Name of object to send the message to")
{
  char *subject;
  char *body = (char *)"";
  char *to = (char *)"";

  static const char *kwlist[] = {"subject", "body", "to", nullptr};
  if (!PyArg_ParseTupleAndKeywords(
          args, kwds, "s|ss:sendMessage", const_cast<char **>(kwlist), &subject, &body, &to))
  {
    return nullptr;
  }

  GetScene()->GetNetworkMessageScene()->SendMessage(to, this, subject, body);
  Py_RETURN_NONE;
}

static void layer_check(short &layer, const char *method_name)
{
  if (layer < 0 || layer >= MAX_ACTION_LAYERS) {
    CM_PythonFunctionWarning("KX_GameObject",
                             method_name,
                             "given layer (" << layer << ") is out of range (0 - "
                                             << (MAX_ACTION_LAYERS - 1) << "), setting to 0.");
    layer = 0;
  }
}

EXP_PYMETHODDEF_DOC(KX_GameObject,
                    playAction,
                    "playAction(name, start_frame, end_frame, layer=0, priority=0 blendin=0, "
                    "play_mode=ACT_MODE_PLAY, layer_weight=0.0, ipo_flags=0, speed=1.0)\n"
                    "Plays an action\n")
{
  const char *name;
  float start, end, blendin = 0.f, speed = 1.f, layer_weight = 0.f;
  short layer = 0, priority = 0;
  short ipo_flags = 0;
  short play_mode = 0;
  short blend_mode = 0;

  static const char *kwlist[] = {"name",
                                 "start_frame",
                                 "end_frame",
                                 "layer",
                                 "priority",
                                 "blendin",
                                 "play_mode",
                                 "layer_weight",
                                 "ipo_flags",
                                 "speed",
                                 "blend_mode",
                                 nullptr};

  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwds,
                                   "sff|hhfhfhfh:playAction",
                                   const_cast<char **>(kwlist),
                                   &name,
                                   &start,
                                   &end,
                                   &layer,
                                   &priority,
                                   &blendin,
                                   &play_mode,
                                   &layer_weight,
                                   &ipo_flags,
                                   &speed,
                                   &blend_mode))
  {
    return nullptr;
  }

  layer_check(layer, "playAction");

  if (play_mode < 0 || play_mode > BL_Action::ACT_MODE_MAX) {
    CM_PythonFunctionWarning("KX_GameObject",
                             "playAction",
                             "given play_mode (" << play_mode << ") is out of range (0 - "
                                                 << (BL_Action::ACT_MODE_MAX - 1)
                                                 << "), setting to ACT_MODE_PLAY");
    play_mode = BL_Action::ACT_MODE_PLAY;
  }

  if (blend_mode < 0 || blend_mode > BL_Action::ACT_BLEND_MAX) {
    CM_PythonFunctionWarning("KX_GameObject",
                             "playAction",
                             "given blend_mode (" << blend_mode << ") is out of range (0 - "
                                                  << (BL_Action::ACT_BLEND_MAX - 1)
                                                  << "), setting to ACT_BLEND_BLEND");
    blend_mode = BL_Action::ACT_BLEND_BLEND;
  }

  if (layer_weight < 0.f || layer_weight > 1.f) {
    CM_PythonFunctionWarning(
        "KX_GameObject",
        "playAction",
        "given layer_weight (" << layer_weight << ") is out of range (0.0 - 1.0), setting to 0.0");
    layer_weight = 0.f;
  }

  PlayAction(name,
             start,
             end,
             layer,
             priority,
             blendin,
             play_mode,
             layer_weight,
             ipo_flags,
             speed,
             blend_mode);

  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_GameObject,
                    stopAction,
                    "stopAction(layer=0)\n"
                    "Stop playing the action on the given layer\n")
{
  short layer = 0;

  if (!PyArg_ParseTuple(args, "|h:stopAction", &layer)) {
    return nullptr;
  }

  layer_check(layer, "stopAction");

  StopAction(layer);

  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_GameObject,
                    getActionFrame,
                    "getActionFrame(layer=0)\n"
                    "Gets the current frame of the action playing in the supplied layer\n")
{
  short layer = 0;

  if (!PyArg_ParseTuple(args, "|h:getActionFrame", &layer)) {
    return nullptr;
  }

  layer_check(layer, "getActionFrame");

  return PyFloat_FromDouble(GetActionFrame(layer));
}

EXP_PYMETHODDEF_DOC(KX_GameObject,
                    getActionName,
                    "getActionName(layer=0)\n"
                    "Gets the name of the current action playing in the supplied layer\n")
{
  short layer = 0;

  if (!PyArg_ParseTuple(args, "|h:getActionName", &layer)) {
    return nullptr;
  }

  layer_check(layer, "getActionName");

  return PyUnicode_FromStdString(GetActionName(layer));
}

EXP_PYMETHODDEF_DOC(KX_GameObject,
                    setActionFrame,
                    "setActionFrame(frame, layer=0)\n"
                    "Set the current frame of the action playing in the supplied layer\n")
{
  short layer = 0;
  float frame;

  if (!PyArg_ParseTuple(args, "f|h:setActionFrame", &frame, &layer)) {
    return nullptr;
  }

  layer_check(layer, "setActionFrame");

  SetActionFrame(layer, frame);

  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_GameObject,
                    isPlayingAction,
                    "isPlayingAction(layer=0)\n"
                    "Checks to see if there is an action playing in the given layer\n")
{
  short layer = 0;

  if (!PyArg_ParseTuple(args, "|h:isPlayingAction", &layer)) {
    return nullptr;
  }

  layer_check(layer, "isPlayingAction");

  return PyBool_FromLong(!IsActionDone(layer));
}

EXP_PYMETHODDEF_DOC(KX_GameObject,
                    resetSmoothing,
                    "resetSmoothing(blend=0.0)\n"
                    "The object's last move is a jump (a teleport): with Smooth Motion it and its\n"
                    "children are drawn there at once, or slide there over blend seconds.\n")
{
  float blend = 0.0f;
  static const char *kwlist[] = {"blend", nullptr};
  if (!PyArg_ParseTupleAndKeywords(
          args, kwds, "|f:resetSmoothing", const_cast<char **>(kwlist), &blend))
  {
    return nullptr;
  }
  if (!std::isfinite(blend) || blend < 0.0f) {
    PyErr_SetString(PyExc_ValueError,
                    "gameOb.resetSmoothing(blend): KX_GameObject, blend must be 0 or more seconds");
    return nullptr;
  }

  ResetSmoothing(blend);
  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_GameObject,
                    addDebugProperty,
                    "addDebugProperty(name, visible=1)\n"
                    "Added or remove a debug property to the debug list.\n")
{
  KX_Scene *scene = GetScene();
  char *name;
  int visible = 1;

  if (!PyArg_ParseTuple(args, "s|i:debugProperty", &name, &visible)) {
    return nullptr;
  }

  if (visible) {
    if (!scene->PropertyInDebugList(this, name))
      scene->AddDebugProperty(this, name);
  }
  else {
    scene->RemoveDebugProperty(this, name);
  }

  Py_RETURN_NONE;
}

/* dict style access */

/* Matches python dict.get(key, [default]) */
PyObject *KX_GameObject::Pyget(PyObject *args)
{
  PyObject *key;
  PyObject *def = Py_None;
  PyObject *ret;

  if (!PyArg_ParseTuple(args, "O|O:get", &key, &def)) {
    return nullptr;
  }

  if (PyUnicode_Check(key)) {
    EXP_Value *item = GetProperty(_PyUnicode_AsString(key));
    if (item) {
      ret = item->ConvertValueToPython();
      if (ret)
        return ret;
      else
        return item->GetProxy();
    }
  }

  if (m_attr_dict && (ret = PyDict_GetItem(m_attr_dict, key))) {
    Py_INCREF(ret);
    return ret;
  }

  Py_INCREF(def);
  return def;
}

bool ConvertPythonToGameObject(SCA_LogicManager *manager,
                               PyObject *value,
                               KX_GameObject **object,
                               bool py_none_ok,
                               const char *error_prefix)
{
  if (value == nullptr) {
    PyErr_Format(PyExc_TypeError, "%s, python pointer nullptr, should never happen", error_prefix);
    *object = nullptr;
    return false;
  }

  if (value == Py_None) {
    *object = nullptr;

    if (py_none_ok) {
      return true;
    }
    else {
      PyErr_Format(PyExc_TypeError,
                   "%s, expected KX_GameObject or a KX_GameObject name, None is invalid",
                   error_prefix);
      return false;
    }
  }

  if (PyUnicode_Check(value)) {
    *object = (KX_GameObject *)manager->GetGameObjectByName(
        std::string(_PyUnicode_AsString(value)));

    if (*object) {
      return true;
    }
    else {
      PyErr_Format(PyExc_ValueError,
                   "%s, requested name \"%s\" did not match any KX_GameObject in this scene",
                   error_prefix,
                   _PyUnicode_AsString(value));
      return false;
    }
  }

  if (PyObject_TypeCheck(value, &KX_GameObject::Type)) {
    *object = static_cast<KX_GameObject *> EXP_PROXY_REF(value);

    /* sets the error */
    if (*object == nullptr) {
      PyErr_Format(PyExc_SystemError, "%s, " EXP_PROXY_ERROR_MSG, error_prefix);
      return false;
    }

    return true;
  }

  *object = nullptr;

  if (py_none_ok) {
    PyErr_Format(PyExc_TypeError, "%s, expect a KX_GameObject, a string or None", error_prefix);
  }
  else {
    PyErr_Format(PyExc_TypeError, "%s, expect a KX_GameObject or a string", error_prefix);
  }

  return false;
}
#endif  // WITH_PYTHON
