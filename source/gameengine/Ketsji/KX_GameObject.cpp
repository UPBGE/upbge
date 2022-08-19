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

#include "BKE_lib_id.h"
#include "BKE_mball.h"
#include "BKE_modifier.h"
#include "BKE_object.h"
#include "DEG_depsgraph_query.h"
#include "DRW_render.h"
#include "WM_api.h"

#include "BL_Action.h"
#include "BL_ActionManager.h"
#include "BL_SceneConverter.h"
#include "KX_ClientObjectInfo.h"
#include "KX_CollisionContactPoints.h"
#include "KX_Globals.h"
#include "KX_LodLevel.h"
#include "KX_LodManager.h"
#include "KX_MeshProxy.h"
#include "KX_NetworkMessageScene.h"  //Needed for sendMessage()
#include "KX_NodeRelationships.h"
#include "KX_PolyProxy.h"
#include "KX_PyMath.h"
#include "KX_PythonComponent.h"
#include "KX_RayCast.h"
#include "SCA_ISensor.h"
#include "SG_Controller.h"

#ifdef WITH_PYTHON
#  include "EXP_PythonCallBack.h"
#  include "bpy_rna.h"
#  include "python_utildefines.h"
#endif

static MT_Vector3 dummy_point = MT_Vector3(0.0f, 0.0f, 0.0f);
static MT_Vector3 dummy_scaling = MT_Vector3(1.0f, 1.0f, 1.0f);
static MT_Matrix3x3 dummy_orientation = MT_Matrix3x3(
    1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);

KX_GameObject::ActivityCullingInfo::ActivityCullingInfo()
    : m_flags(ACTIVITY_NONE), m_physicsRadius(0.0f), m_logicRadius(0.0f)
{
}

KX_GameObject::KX_GameObject()
    : SCA_IObject(),
      m_isReplica(false),            // eevee
      m_visibleAtGameStart(false),   // eevee
      m_forceIgnoreParentTx(false),  // eevee
      m_previousLodLevel(-1),        // eevee
      m_layer(0),
      m_lodManager(nullptr),
      m_currentLodLevel(0),
      m_pBlenderObject(nullptr),
      m_pBlenderGroupObject(nullptr),
      m_bIsNegativeScaling(false),
      m_objectColor(1.0f, 1.0f, 1.0f, 1.0f),
      m_bVisible(true),
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

  unit_m4(m_prevObmat);  // eevee
};

KX_GameObject::~KX_GameObject()
{
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

  Object *ob = GetBlenderObject();

  if (ob) {
    if (ob->gameflag & OB_OVERLAY_COLLECTION) {
      ob->gameflag &= ~OB_OVERLAY_COLLECTION;
    }
  }

  if (m_pSGNode) {
    KX_Scene *scene = GetScene();

    if (scene->m_isRuntime) {
      HideOriginalObject();
      RemoveReplicaObject();
    }
    else {  // at scene exit
      if (ob && strcmp(ob->id.name, "OBgame_default_cam") != 0) {
        SetVisible(m_visibleAtGameStart, false);
      }
      RemoveReplicaObject();

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
void KX_GameObject::SetBlenderObject(Object *obj)
{
  m_pBlenderObject = obj;
  if (obj) {
    Scene *scene = GetScene()->GetBlenderScene();
    ViewLayer *view_layer = BKE_view_layer_default_view(scene);
    Base *base = BKE_view_layer_base_find(view_layer, obj);
    if (base) {  // base can be nullptr for objects in instanced collections
      m_visibleAtGameStart = (base->flag & BASE_HIDDEN) == 0;
    }
  }
}

void KX_GameObject::SyncTransformWithDepsgraph()
{
  Object *ob = GetBlenderObject();
  if (ob) {
    float loc[3], rot[3][3], size[3];
    mat4_to_loc_rot_size(loc, rot, size, ob->obmat);
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

void KX_GameObject::TagForTransformUpdate(bool is_overlay_pass, bool is_last_render_pass)
{
  float obmat[4][4];
  NodeGetWorldTransform().getValue(&obmat[0][0]);
  bool staticObject = true;
  if (GetSGNode()->IsDirty(SG_Node::DIRTY_RENDER)) {
    staticObject = false;
    /* Wait the end of all render passes (main + custom viewports)
     * to clear dirty render because we want the objects to
     * be tagged for transform update for each render pass.
     */
    bool multiple_render_passes = KX_GetActiveEngine()->GetRenderingCameras().size() > 1;
    if (multiple_render_passes && !is_last_render_pass) {
      // wait
    }
    else if (multiple_render_passes && is_last_render_pass) {
      GetSGNode()->ClearDirty(SG_Node::DIRTY_RENDER);
      copy_m4_m4(m_prevObmat, obmat);  // This is used for ImageRender but could be changed...
    }
    else {
      GetSGNode()->ClearDirty(SG_Node::DIRTY_RENDER);
      copy_m4_m4(m_prevObmat, obmat);
    }
  }

  bContext *C = KX_GetActiveEngine()->GetContext();
  Main *bmain = CTX_data_main(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_on_load(C);

  Object *ob_orig = GetBlenderObject();

  bool skip_transform = ob_orig->transflag & OB_TRANSFLAG_OVERRIDE_GAME_PRIORITY;
  /* Don't tag non overlay collection objects in overlay collection render pass */
  skip_transform = skip_transform ||
                   ((ob_orig->gameflag & OB_OVERLAY_COLLECTION) == 0 && is_overlay_pass);
  /* Don't tag overlay collection objects in non overlay collection render pass */
  skip_transform = skip_transform ||
                   (ob_orig->gameflag & OB_OVERLAY_COLLECTION && !is_overlay_pass);

  if (ob_orig && !skip_transform) {

    bool applyTransformToOrig = GetScene()->OrigObCanBeTransformedInRealtime(ob_orig);

    if (applyTransformToOrig) {
      copy_m4_m4(ob_orig->obmat, obmat);
      BKE_object_apply_mat4(
          ob_orig, ob_orig->obmat, false, ob_orig->parent && ob_orig->partype != PARVERT1);
    }

    if (!staticObject || m_forceIgnoreParentTx) {
      std::vector<KX_GameObject *> children = GetChildren();
      if (children.size() > 0) {
        std::vector<Object *> childrenObjects;
        for (KX_GameObject *go : children) {
          Object *child = go->GetBlenderObject();
          if (child) {
            childrenObjects.push_back(child);
          }
        }
        GetScene()->IgnoreParentTxBGE(bmain, depsgraph, ob_orig, childrenObjects);
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
  }

  m_forceIgnoreParentTx = false;
}

void KX_GameObject::TagForTransformUpdateEvaluated()
{
  float obmat[4][4];
  NodeGetWorldTransform().getValue(&obmat[0][0]);

  bContext *C = KX_GetActiveEngine()->GetContext();
  Depsgraph *depsgraph = CTX_data_depsgraph_on_load(C);

  Object *ob_orig = GetBlenderObject();

  bool skip_transform = ob_orig->transflag & OB_TRANSFLAG_OVERRIDE_GAME_PRIORITY;

  if (skip_transform) {
    SyncTransformWithDepsgraph();
  }

  if (ob_orig && !skip_transform) {
    Object *ob_eval = DEG_get_evaluated_object(depsgraph, ob_orig);
    copy_m4_m4(ob_eval->obmat, obmat);
    BKE_object_apply_mat4(ob_eval, ob_eval->obmat, false, true);
  }
}

void KX_GameObject::ReplicateBlenderObject()
{
  Object *ob = GetBlenderObject();

  if (ob) {
    bContext *C = KX_GetActiveEngine()->GetContext();
    Main *bmain = CTX_data_main(C);
    Object *newob;
    BKE_id_copy_ex(bmain, &ob->id, (ID **)&newob, 0);
    id_us_min(&newob->id);
    Scene *scene = GetScene()->GetBlenderScene();
    ViewLayer *view_layer = BKE_view_layer_default_view(scene);
    BKE_collection_object_add_from(bmain,
                                   scene,
                                   BKE_view_layer_camera_find(view_layer),
                                   newob);  // add replica where is the active camera
    newob->base_flag |= (BASE_VISIBLE_VIEWLAYER | BASE_VISIBLE_DEPSGRAPH);
    newob->visibility_flag &= ~OB_HIDE_VIEWPORT;
    GetScene()->TagForCollectionRemap();

    if (ob->parent) {
      if (GetScene()->GetLastReplicatedParentObject()) {
        newob->parent = GetScene()->GetLastReplicatedParentObject();
        if (ob->parent && ob->parent->type == OB_ARMATURE) {
          ModifierData *mod;
          for (mod = (ModifierData *)newob->modifiers.first; mod; mod = mod->next) {
            if (mod->type == eModifierType_Armature) {
              ((ArmatureModifierData *)mod)->object = newob->parent;
            }
          }
        }
        GetScene()->ResetLastReplicatedParentObject();
      }
    }

    // To check again
    const NodeList &children = GetSGNode()->GetSGChildren();
    if (children.size() > 0) {
      GetScene()->SetLastReplicatedParentObject(newob);
    }

    DEG_relations_tag_update(bmain);

    m_pBlenderObject = newob;
    m_isReplica = true;
  }
}
void KX_GameObject::RemoveReplicaObject()
{
  Object *ob = GetBlenderObject();
  if (ob && m_isReplica) {
    bContext *C = KX_GetActiveEngine()->GetContext();
    Main *bmain = CTX_data_main(C);
    BKE_id_delete(bmain, ob);
    SetBlenderObject(nullptr);
    DEG_relations_tag_update(bmain);
  }
}

void KX_GameObject::HideOriginalObject()
{
  Object *ob = GetBlenderObject();
  if (ob && !m_isReplica &&
      (ob->base_flag & (BASE_VISIBLE_VIEWLAYER | BASE_VISIBLE_DEPSGRAPH)) != 0) {
    Scene *scene = GetScene()->GetBlenderScene();
    ViewLayer *view_layer = BKE_view_layer_default_view(scene);
    Base *base = BKE_view_layer_base_find(view_layer, ob);
    if (base) {  // As for SetVisible, there are cases (when we use bpy...) where Objects have no
                 // base.
      base->flag |= BASE_HIDDEN;
      BKE_layer_collection_sync(scene, view_layer);
      DEG_id_tag_update(&scene->id, ID_RECALC_BASE_FLAGS);
      GetScene()->m_hiddenObjectsDuringRuntime.push_back(ob);
    }
  }
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

void KX_GameObject::AddDummyLodManager(RAS_MeshObject *meshObj, Object *ob)
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
}

float *KX_GameObject::GetPrevObmat()
{
  return (float *)m_prevObmat;
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
  }
}

void KX_GameObject::SetDupliGroupObject(KX_GameObject *obj)
{
  obj->AddRef();
  m_pDupliGroupObject = obj;
}

void KX_GameObject::AddConstraint(bRigidBodyJointConstraint *cons)
{
  m_constraints.push_back(cons);
}

std::vector<bRigidBodyJointConstraint *> KX_GameObject::GetConstraints()
{
  return m_constraints;
}

void KX_GameObject::ClearConstraints()
{
  m_constraints.clear();
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

void KX_GameObject::SetParent(KX_GameObject *obj, bool addToCompound, bool ghost)
{
  // check on valid node in case a python controller holds a reference to a deleted object
  if (obj &&
      GetSGNode()->GetSGParent() != obj->GetSGNode() &&  // not already parented to same object
      !GetSGNode()->IsAncessor(obj->GetSGNode()) &&      // no parenting loop
      this != obj)                                       // not the object itself
  {
    if (!(GetScene()->GetInactiveList()->SearchValue(obj) !=
          GetScene()->GetObjectList()->SearchValue(this))) {
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
        fabs(scale1[1]) < (MT_Scalar)FLT_EPSILON || fabs(scale1[2]) < (MT_Scalar)FLT_EPSILON) {
      return;
    }

    KX_Scene *scene = GetScene();
    // Remove us from our old parent and set our new parent
    RemoveParent();
    obj->GetSGNode()->AddChild(GetSGNode());

    if (m_pPhysicsController) {
      m_pPhysicsController->SuspendDynamics(ghost);
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
          rootobj->m_pPhysicsController->IsCompound()) {
        rootobj->m_pPhysicsController->AddCompoundChild(m_pPhysicsController);
      }
    }
  }
}

void KX_GameObject::RemoveParent()
{
  // check on valid node in case a python controller holds a reference to a deleted object
  if (GetSGNode()->GetSGParent()) {
    // get the root object to remove us from compound object if needed
    KX_GameObject *rootobj = (KX_GameObject *)GetSGNode()->GetRootSGParent()->GetSGClientObject();
    // Set us to the right spot
    GetSGNode()->SetLocalScale(GetSGNode()->GetWorldScaling());
    GetSGNode()->SetLocalOrientation(GetSGNode()->GetWorldOrientation());
    GetSGNode()->SetLocalPosition(GetSGNode()->GetWorldPosition());

    // Remove us from our parent
    GetSGNode()->DisconnectFromParent();
    NodeUpdateGS(0.f);

    KX_Scene *scene = GetScene();
    // the object is now a root object, add it to the parentlist
    EXP_ListValue<KX_GameObject> *rootlist = scene->GetRootParentList();
    if (!rootlist->SearchValue(this))
      // object was not in root list, add it now and increment ref count
      rootlist->Add(CM_AddRef(this));
    if (m_pPhysicsController) {
      // in case this controller was added as a child shape to the parent
      if (rootobj != nullptr && rootobj->m_pPhysicsController != nullptr &&
          rootobj->m_pPhysicsController->IsCompound()) {
        rootobj->m_pPhysicsController->RemoveCompoundChild(m_pPhysicsController);
      }
      m_pPhysicsController->RestoreDynamics();
      if (m_pPhysicsController->IsDynamic() &&
          (rootobj != nullptr && rootobj->m_pPhysicsController)) {
        // dynamic object should remember the velocity they had while being parented
        MT_Vector3 childPoint = GetSGNode()->GetWorldPosition();
        MT_Vector3 rootPoint = rootobj->GetSGNode()->GetWorldPosition();
        MT_Vector3 relPoint;
        relPoint = (childPoint - rootPoint);
        MT_Vector3 linVel = rootobj->m_pPhysicsController->GetVelocity(relPoint);
        MT_Vector3 angVel = rootobj->m_pPhysicsController->GetAngularVelocity();
        m_pPhysicsController->SetLinearVelocity(linVel, false);
        m_pPhysicsController->SetAngularVelocity(angVel, false);
      }
    }
  }
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

bAction *KX_GameObject::GetCurrentAction(short layer)
{
  return GetActionManager()->GetCurrentAction(layer);
}

void KX_GameObject::SetPlayMode(short layer, short mode)
{
  GetActionManager()->SetPlayMode(layer, mode);
}

void KX_GameObject::ProcessReplica()
{
  KX_PythonProxy::ProcessReplica();

  ReplicateBlenderObject();
  GetScene()->GetBlenderSceneConverter()->RegisterGameObject(this, m_pBlenderObject);

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
  MT_Matrix3x3 rotmat(drot);

  GetSGNode()->RelativeRotate(rotmat, local);

  if (m_pPhysicsController) {  // (IsDynamic())
    m_pPhysicsController->RelativeRotate(rotmat, local);
  }
  NodeUpdateGS(0.0f);
}

void KX_GameObject::UpdateBlenderObjectMatrix(Object *blendobj)
{
  if (!blendobj)
    blendobj = m_pBlenderObject;
  if (blendobj) {
    float obmat[4][4];
    NodeGetWorldTransform().getValue(&obmat[0][0]);
    copy_m4_m4(blendobj->obmat, obmat);
    /* Making sure it's updated. (To move volumes) */
    invert_m4_m4(blendobj->imat, blendobj->obmat);
  }
}

void KX_GameObject::UpdateBuckets()
{
}

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

void KX_GameObject::UpdateLod(const MT_Vector3 &cam_pos, float lodfactor)
{
  if (!m_lodManager) {
    return;
  }

  KX_Scene *scene = GetScene();
  const float distance2 = NodeGetWorldPosition().distance2(cam_pos) * (lodfactor * lodfactor);
  KX_LodLevel *lodLevel = m_lodManager->GetLevel(scene, m_currentLodLevel, distance2);

  bool updatePhysicsShape = false;
  if (GetBlenderObject()->gameflag & OB_LOD_UPDATE_PHYSICS) {
    if (GetPhysicsController()) {
      /* As m_previousLodLevel is initialized to -1,
       * the physics shape will be ensured on first frame
       * to match the lodLevel or the absence of lodLevel
       */
      if (m_currentLodLevel != m_previousLodLevel) {
        updatePhysicsShape = true;
        m_previousLodLevel = m_currentLodLevel;
      }
    }
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
    bContext *C = KX_GetActiveEngine()->GetContext();
    Depsgraph *depsgraph = CTX_data_expect_evaluated_depsgraph(C);

    /* Here we want to change the object which will be rendered, then the evaluated object by the
     * depsgraph */
    Object *ob_eval = DEG_get_evaluated_object(depsgraph, GetBlenderObject());

    Object *eval_lod_ob = DEG_get_evaluated_object(depsgraph, currentLodLevel->GetObject());
    /* Try to get the object with all modifiers applied */
    ob_eval->data = eval_lod_ob->data;
  }

  if (updatePhysicsShape) {
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

void KX_GameObject::UpdateTransformFunc(SG_Node *node, void *gameobj, void *scene)
{
  ((KX_GameObject *)gameobj)->UpdateTransform();
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
  return m_bVisible;
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

void KX_GameObject::SetVisible(bool v, bool recursive)
{
  Object *ob = GetBlenderObject();
  if (ob) {
    Scene *scene = GetScene()->GetBlenderScene();
    ViewLayer *view_layer = BKE_view_layer_default_view(scene);
    Base *base = BKE_view_layer_base_find(view_layer, ob);
    if (base) {  // Base can be NULL for objects in linked collections...
      if (v) {
        base->flag &= ~BASE_HIDDEN;
      }
      else {
        base->flag |= BASE_HIDDEN;
      }

      BKE_layer_collection_sync(scene, view_layer);
      if (ob->gameflag & OB_OVERLAY_COLLECTION) {
        GetScene()->AppendToIdsToUpdateInOverlayPass(&scene->id, ID_RECALC_BASE_FLAGS);
      }
      else {
        GetScene()->AppendToIdsToUpdateInAllRenderPasses(&scene->id, ID_RECALC_BASE_FLAGS);
      }
    }
  }

  if (recursive) {
    setVisible_recursive(GetSGNode(), v);
  }

  m_bVisible = v;
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
  m_objectColor = rgbavec;
  Object *ob_orig = GetBlenderObject();
  if (ob_orig && GetScene()->OrigObCanBeTransformedInRealtime(ob_orig) &&
      ELEM(ob_orig->type, OB_MESH, OB_CURVES_LEGACY, OB_SURF, OB_FONT, OB_MBALL)) {
    copy_v4_v4(ob_orig->color, m_objectColor.getValue());
    DEG_id_tag_update(&ob_orig->id, ID_RECALC_SHADING | ID_RECALC_TRANSFORM);
    WM_main_add_notifier(NC_OBJECT | ND_DRAW, &ob_orig->id);
  }
}

const MT_Vector4 &KX_GameObject::GetObjectColor()
{
  return m_objectColor;
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
        fabs(p_scale[2]) < (MT_Scalar)FLT_EPSILON) {
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
        fabs(scale[2]) < (MT_Scalar)FLT_EPSILON) {
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

MT_Transform KX_GameObject::NodeGetLocalTransform() const
{
  return m_pSGNode->GetLocalTransform();
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

/* Suspend/ resume: for the dynamic behavior, there is a simple
 * method. For the residual motion, there is not. I wonder what the
 * correct solution is for Sumo. Remove from the motion-update tree?
 *
 * So far, only switch the physics and logic.
 * */

// void KX_GameObject::ResumeDynamics(void)
//{
//  if (m_logicSuspended) {
//    SCA_IObject::ResumeLogic();
//    // Child objects must be static, so we block changing to dynamic
//    if (GetPhysicsController() && !GetParent())
//      GetPhysicsController()->RestoreDynamics();
//
//    m_logicSuspended = false;
//  }
//}
//
// void KX_GameObject::SuspendDynamics()
//{
//  if (!m_logicSuspended) {
//    SCA_IObject::SuspendLogic();
//    if (GetPhysicsController())
//      GetPhysicsController()->SuspendDynamics();
//    m_logicSuspended = true;
//  }
//}

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

static int mathutils_kxgameob_generic_check(BaseMathObject *bmo)
{
  KX_GameObject *self = static_cast<KX_GameObject *> EXP_PROXY_REF(bmo->cb_user);
  if (self == nullptr)
    return -1;

  return 0;
}

static int mathutils_kxgameob_vector_get(BaseMathObject *bmo, int subtype)
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

static int mathutils_kxgameob_vector_set(BaseMathObject *bmo, int subtype)
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

static int mathutils_kxgameob_vector_get_index(BaseMathObject *bmo, int subtype, int index)
{
  /* lazy, avoid repeteing the case statement */
  if (mathutils_kxgameob_vector_get(bmo, subtype) == -1)
    return -1;
  return 0;
}

static int mathutils_kxgameob_vector_set_index(BaseMathObject *bmo, int subtype, int index)
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

static int mathutils_kxgameob_matrix_get(BaseMathObject *bmo, int subtype)
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

static int mathutils_kxgameob_matrix_set(BaseMathObject *bmo, int subtype)
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
    {"collide", (PyCFunction) KX_GameObject::sPyCollide, METH_O},
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
                                   &use_phys)) {
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
                            "gameOb.reinstancePhysicsMesh(obj, mesh, dupli): KX_GameObject"))) {
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
          logicmgr, value, &gameobj, false, "gameOb.replacePhysicsShape(obj): KX_GameObject")) {
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
                                   &ghost)) {
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

  if (!ConvertPythonToGameObject(scene->GetLogicManager(), value, &other, false, "gameOb.collide(obj): KX_GameObject")) {
    return nullptr;
  }

  if (!m_pPhysicsController || !other->GetPhysicsController()) {
    PyErr_SetString(PyExc_TypeError, "expected objects with physics controller");
    return nullptr;
  }

  PHY_IPhysicsEnvironment *env = scene->GetPhysicsEnvironment();
  PHY_CollisionTestResult testResult = env->CheckCollision(m_pPhysicsController, other->GetPhysicsController());

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
      KX_CollisionContactPointList *contactPointList = new KX_CollisionContactPointList(testResult.collData, testResult.isFirst);
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
          args, kwds, "O|if:alignAxisToVect", const_cast<char **>(kwlist), &pyvect, &axis, &fac)) {
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
  Object *ob = self->GetBlenderObject();
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
          logicmgr, value, &other, false, "gameOb.getDistanceTo(value): KX_GameObject")) {
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
          args, kwds, "O|fs:rayCastTo", const_cast<char **>(kwlist), &pyarg, &dist, &propName)) {
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
                                   &mask)) {
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
          args, kwds, "s|ss:sendMessage", const_cast<char **>(kwlist), &subject, &body, &to)) {
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
                                   &blend_mode)) {
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
