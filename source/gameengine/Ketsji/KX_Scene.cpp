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
 * Ketsji scene. Holds references to all scene data.
 */

/** \file gameengine/Ketsji/KX_Scene.cpp
 *  \ingroup ketsji
 */

#ifdef _MSC_VER
#  pragma warning(disable : 4786)
#endif

#include "KX_Scene.h"

#include "BKE_collection.hh"
#include "BKE_duplilist.hh"
#include "BKE_geometry_set.hh"
#include "BKE_global.hh"
#include "BKE_instances.hh"
#include "BKE_idprop.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mball.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BKE_scene.hh"
#include "BKE_screen.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_memory_utils.hh"
#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_task.h"
#include "BLI_time.h"
#include "BKE_node_legacy_types.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_anim_types.h"
#include "DNA_camera_types.h"
#include "DNA_collection_types.h"
#include "DNA_light_types.h"
#include "DNA_lightprobe_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_world_types.h"
#include "GEO_realize_instances.hh"
#include "RNA_access.hh"
#include "RNA_prototypes.hh"
#include "DNA_property_types.h"
#include "DNA_rigidbody_types.h"
#include "DRW_render.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"
#include "GPU_context.hh"
#include "GPU_state.hh"
#include "GPU_viewport.hh"
#include "wm_draw.hh"
#include "wm_event_system.hh"
#include "xr/wm_xr.hh"

#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "BL_ArmatureObject.h"
#include "BL_Converter.h"
#include "BL_DataConversion.h"
#include "BL_SceneConverter.h"
#include "CM_List.h"
#include "EXP_FloatValue.h"
#include "KX_2DFilterManager.h"
#include "KX_BlenderCanvas.h"
#include "KX_Camera.h"
#include "KX_CollisionEventManager.h"
#include "KX_FontObject.h"
#include "KX_Globals.h"
#include "KX_Light.h"
#include "KX_LodManager.h"
#include "KX_MotionState.h"
#include "KX_NetworkMessageScene.h"
#include "KX_NodeRelationships.h"
#include "KX_ObstacleSimulation.h"
#include "KX_PhysicsEngineEnums.h"
#include "KX_PyMath.h"
#include "PHY_IPhysicsController.h"
#include "PHY_IPhysicsEnvironment.h"
#include "RAS_BucketManager.h"
#include "RAS_FrameBuffer.h"
#include "SCA_2DFilterActuator.h"
#include "SCA_ActuatorEventManager.h"
#include "SCA_BasicEventManager.h"
#include "SCA_JoystickManager.h"
#include "SCA_KeyboardManager.h"
#include "SCA_MouseManager.h"
#include "SCA_TimeEventManager.h"
#include "SG_Controller.h"

#ifdef WITH_GAMEENGINE_LOGICNODES
#  include "LN_Manager.h"
#  include "LN_UIService.h"
#endif

#ifdef WITH_PYTHON
#  include "EXP_PythonCallBack.h"
#  include "bpy_rna.hh"
#endif

using namespace blender;

/** The copies of one camera pass drawn as groups (KX_Scene::BeginDupliDrawGroups). */
struct KX_DupliDrawGroups {
  /** What copies of one group share: Blender's grouped drawing gives a whole group one drawn
   * object, one object color, one facing and one visibility. */
  struct Key {
    blender::Object *object = nullptr;
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool negative_scale = false;
    bool keep_shadow = false;
    /** Moved in this pass: the group of still copies then keeps its shadows (EEVEE redraws the
     * shadows of a whole group when any of its copies moved). */
    bool moved = false;

    uint64_t hash() const
    {
      uint32_t bits[4];
      std::memcpy(bits, color, sizeof(bits));
      const uint64_t flags = uint64_t(negative_scale) | (uint64_t(keep_shadow) << 1) |
                             (uint64_t(moved) << 2);
      return blender::get_default_hash(blender::get_default_hash(object, bits[0], bits[1]),
                                       blender::get_default_hash(bits[2], bits[3]),
                                       flags);
    }
    friend bool operator==(const Key &a, const Key &b)
    {
      return a.object == b.object && std::memcmp(a.color, b.color, sizeof(a.color)) == 0 &&
             a.negative_scale == b.negative_scale && a.keep_shadow == b.keep_shadow &&
             a.moved == b.moved;
    }
  };

  /** Every grouped copy's DupliObject: room for all copies is reserved before a pass fills it,
   * so the groups' pointers into it stay valid. */
  blender::Vector<blender::DupliObject> instances;
  blender::Vector<blender::DRWGameInstanceGroup> groups;
  /** One pointer list per group, kept with its memory between passes. */
  blender::Vector<std::unique_ptr<blender::VectorList<blender::DupliObject *>>> lists;
  blender::Map<Key, int> group_of;

  void clear()
  {
    instances.clear();
    groups.clear();
    group_of.clear();
    for (std::unique_ptr<blender::VectorList<blender::DupliObject *>> &list : lists) {
      list->clear();
    }
  }
};

/**
 * Whether Blender's grouped drawing draws a copy of `ob` (its evaluated object) as the copy's own
 * entry would: draw_context.cc's `supports_handle_ranges` (a mesh not shown as bounds, without
 * particle hair or fluid), and nothing drawn beside the geometry (particles, instances of its
 * own, other geometry from Geometry Nodes).
 */
static bool dupli_group_drawable(const blender::Object &ob)
{
  if (ob.type != OB_MESH || ob.dt == OB_BOUNDBOX || (ob.transflag & OB_DUPLI) != 0) {
    return false;
  }
  if (!BLI_listbase_is_empty(&ob.particlesystem) ||
      BKE_modifiers_findby_type(&ob, eModifierType_Fluid) != nullptr)
  {
    return false;
  }
  if (const blender::bke::GeometrySet *geometry = ob.runtime->geometry_set_eval) {
    if (geometry->has_instances() || geometry->has_pointcloud() || geometry->has_curves() ||
        geometry->has_volume() || geometry->has_grease_pencil())
    {
      return false;
    }
  }
  return true;
}

/** The game scene whose copies a drawing of `depsgraph` shows (each scene has its own copies). */
static KX_Scene *dupli_copies_scene(KX_KetsjiEngine *engine, blender::Depsgraph *depsgraph)
{
  auto *scenes = engine->CurrentScenes();
  if (scenes == nullptr || scenes->GetCount() == 0 || depsgraph == nullptr) {
    return nullptr;
  }
  const blender::Scene *drawn_scene = DEG_get_input_scene(depsgraph);
  for (KX_Scene *scene : *scenes) {
    if (scene->GetBlenderScene() == drawn_scene) {
      return scene;
    }
  }
  return scenes->GetFront();
}

/** Blender's drawing has one hook for the game's Dupli copies, which every scene draws through:
 * it goes with the last scene. Each scene's end took it away before, so a scene deleted while
 * others go on (the one Load Scene merges, Remove Scene) left their copies undrawn on the
 * per-copy path. Scenes may be made on LibLoad's worker thread. */
static std::atomic<int> g_dupli_provider_scenes = 0;

static void bge_dupli_provider(DEGObjectIterData *data)
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  if (engine == nullptr) {
    return;
  }

  blender::bContext *C = engine->GetContext();
  if (C == nullptr) {
    return;
  }
  /* The graph being drawn, and the game scene it draws. */
  blender::Depsgraph *depsgraph = data->graph != nullptr ? data->graph :
                                                           CTX_data_depsgraph_pointer(C);
  KX_Scene *kx_scene = dupli_copies_scene(engine, depsgraph);
  if (kx_scene == nullptr) {
    return;
  }

  const std::vector<KX_GameObject *> &dupli_list = kx_scene->GetDupliListVector();
  kx_scene->FreeRetiredDupliDrawOriginals(engine->GetRenderFrame());
  const uint64_t update_count = DEG_get_update_count(depsgraph);

  for (KX_GameObject *game_obj : dupli_list) {
    if (game_obj && game_obj->IsUpbgeDupliInstance()) {
      if (!game_obj->GetVisible()) {
        continue;
      }
      /* Drawn in a group of this camera pass (KX_Scene::BeginDupliDrawGroups). */
      if (kx_scene->IsDrawnInDupliGroup(game_obj)) {
        continue;
      }
      /* Set Camera Visibility: the camera drawing now does not draw this copy, or draws it only
       * into shadows and reflections. */
      bool keep_shadow = false;
      KX_Scene *object_scene = game_obj->GetScene();
      const bool hidden = object_scene != nullptr &&
                          object_scene->IsHiddenInCameraPass(game_obj, keep_shadow);
      if (hidden && !keep_shadow) {
        continue;
      }
      /* A Dupli Base's copy draws from the base, a grouped Auto object from its group's. */
      blender::Object *ob_src = game_obj->GetDupliDrawBase();
      if (ob_src == nullptr) {
        continue;
      }
      blender::Object *blender_obj = DEG_get_evaluated(depsgraph, ob_src);
      float mat[4][4];
      game_obj->NodeGetWorldTransform().getValue(&mat[0][0]);

      if (blender_obj) {
        float color[4];
        game_obj->GetObjectColor().getValue(color);
        add_bge_object(data, blender_obj, mat, color);
        blender::Object &drawn = data->bge_objects.back()->temp_object;
        /* Its own motion and shadow records in the renderers, not its base's. */
        game_obj->SetDupliDrawIdentity(drawn, mat, update_count);
        if (hidden) {
          drawn.visibility_flag |= OB_HIDE_CAMERA;
        }
      }
    }
  }
}

static void rebase_physics_interpolation_hierarchy(SG_Node *node)
{
  if (node == nullptr) {
    return;
  }

  if (KX_GameObject *gameobj = static_cast<KX_GameObject *>(node->GetSGClientObject())) {
    gameobj->RebasePhysicsInterpolationState();
    /* A copy of an object with its own Smooth Motion. */
    if (ELEM(gameobj->GetSmoothMotion(), OB_SMOOTH_MOTION_INTERPOLATE, OB_SMOOTH_MOTION_EXTRAPOLATE))
    {
      if (KX_Scene *scene = gameobj->GetScene()) {
        scene->NoteSmoothMotion();
      }
    }
  }
  for (SG_Node *child : node->GetSGChildren()) {
    rebase_physics_interpolation_hierarchy(child);
  }
}

/** Smooth Motion of an object without a parent whose choice is Scene Default. */
static char scene_smooth_motion(const bool scene_interpolates)
{
  return scene_interpolates ? OB_SMOOTH_MOTION_INTERPOLATE : OB_SMOOTH_MOTION_NONE;
}

static bool is_smoothed(const char mode)
{
  return ELEM(mode, OB_SMOOTH_MOTION_INTERPOLATE, OB_SMOOTH_MOTION_EXTRAPOLATE);
}

/** From a root down: a node without an object (a parent inverse) passes its parent's choice on. */
static void store_smooth_motion_history(SG_Node *node, const char inherited)
{
  char mode = inherited;
  if (KX_GameObject *gameobj = static_cast<KX_GameObject *>(node->GetSGClientObject())) {
    mode = gameobj->ResolveSmoothMotion(inherited);
    gameobj->StoreSmoothMotionHistory(is_smoothed(mode));
  }
  for (SG_Node *child : node->GetSGChildren()) {
    store_smooth_motion_history(child, mode);
  }
}

void KX_Scene::StorePhysicsInterpolationState(const bool scene_interpolates)
{
  const char root_mode = scene_smooth_motion(scene_interpolates);
  for (KX_GameObject *gameobj : *GetObjectList()) {
    SG_Node *node = gameobj->GetSGNode();
    if (node != nullptr && node->GetSGParent() == nullptr) {
      store_smooth_motion_history(node, root_mode);
    }
  }
}

struct SmoothMotionWalk {
  KX_Scene *scene;
  KX_GameObject::SmoothMotionFrame frame;
  bool skip_invisible;
};

/** From a root down: parents are drawn first, so what is attached can be drawn with them. */
static void apply_smooth_motion(const SmoothMotionWalk &walk,
                                SG_Node *node,
                                const char inherited,
                                const KX_GameObject::SmoothMotionFollow &parent,
                                KX_GameObject::SmoothMotionReset reset)
{
  char mode = inherited;
  KX_GameObject::SmoothMotionFollow follow = parent;
  if (KX_GameObject *gameobj = static_cast<KX_GameObject *>(node->GetSGClientObject())) {
    mode = gameobj->ResolveSmoothMotion(inherited);
    if (gameobj->HasSmoothMotionReset()) {
      /* Its own jump, else a parent's: a teleported object's children jump with it. */
      reset = gameobj->TakeSmoothMotionReset();
    }
    /* Optional fast path: an invisible object with nothing attached keeps its pose. A camera
     * draws the view, seen or not. */
    const bool skip = walk.skip_invisible && node->GetSGChildren().empty() &&
                      gameobj->GetGameObjectType() != SCA_IObject::OBJ_CAMERA &&
                      !gameobj->GetVisible();
    follow = skip ? KX_GameObject::SmoothMotionFollow() :
                    gameobj->ApplySmoothMotion(walk.frame, mode, parent, reset, walk.scene);
  }
  for (SG_Node *child : node->GetSGChildren()) {
    apply_smooth_motion(walk, child, mode, follow, reset);
  }
}

void KX_Scene::ApplyPhysicsInterpolation(const double interpolation,
                                         const bool scene_interpolates,
                                         const double frame_seconds)
{
  SmoothMotionWalk walk;
  walk.scene = this;
  walk.frame.interpolation = interpolation;
  walk.frame.number = ++m_smoothMotionFrame;
  walk.frame.seconds = frame_seconds;
  walk.skip_invisible = m_skipInvisibleInterpolation;
  const char root_mode = scene_smooth_motion(scene_interpolates);
  for (KX_GameObject *gameobj : *GetObjectList()) {
    SG_Node *node = gameobj->GetSGNode();
    if (node != nullptr && node->GetSGParent() == nullptr) {
      apply_smooth_motion(walk,
                          node,
                          root_mode,
                          KX_GameObject::SmoothMotionFollow(),
                          KX_GameObject::SmoothMotionReset());
    }
  }
}

void KX_Scene::ClearPhysicsInterpolationState()
{
  for (KX_GameObject *gameobj : m_interpolatedObjects) {
    BLI_assert(gameobj != nullptr && gameobj->m_interpolationListRegistered);
    gameobj->ClearPhysicsInterpolationState();
    gameobj->m_interpolationListRegistered = false;
  }
  m_interpolatedObjects.clear();
}

void KX_Scene::RebaseKinematicPhysics()
{
  for (KX_GameObject *gameobj : *GetObjectList()) {
    PHY_IPhysicsController *controller = gameobj ? gameobj->GetPhysicsController() : nullptr;
    if (controller && controller->IsKinematic()) {
      controller->RebaseKinematicMotionState();
    }
  }
}

void KX_Scene::BeginPhysicsInterpolationFrameUpdate()
{
  /* Before this, the changed objects' history was set to their new pose: a frame without a step
   * then drew them a whole step ahead (the On Update stutter on fast screens). */
  SG_Node::SetHistoryFollowsUpdates(true);
}

void KX_Scene::EndPhysicsInterpolationFrameUpdate()
{
  SG_Node::SetHistoryFollowsUpdates(false);
}

static void *KX_SceneReplicationFunc(SG_Node *node, void *gameobj, void *scene)
{
  KX_GameObject *replica =
      ((KX_Scene *)scene)->AddNodeReplicaObject(node, (KX_GameObject *)gameobj);

  if (replica)
    replica->Release();

  return (void *)replica;
}

static void *KX_SceneDestructionFunc(SG_Node *node, void *gameobj, void *scene)
{
  ((KX_Scene *)scene)->RemoveNodeDestructObject(node, (KX_GameObject *)gameobj);

  return nullptr;
};

/** The next stamp of an object cache version: 0 means "not found yet" (see
 * KX_GameObject::InvalidateCollectionMembership). */
static uint32_t next_cache_version(const uint32_t version)
{
  return version + 1 != 0 ? version + 1 : 1;
}

bool KX_Scene::KX_ScenegraphUpdateFunc(SG_Node *node, void *gameobj, void *scene)
{
  return node->Schedule(((KX_Scene *)scene)->m_sghead);
}

bool KX_Scene::KX_ScenegraphRescheduleFunc(SG_Node *node, void *gameobj, void *scene)
{
  return node->Reschedule(((KX_Scene *)scene)->m_sghead);
}

SG_Callbacks KX_Scene::m_callbacks = SG_Callbacks(KX_SceneReplicationFunc,
                                                  KX_SceneDestructionFunc,
                                                  KX_GameObject::UpdateTransformFunc,
                                                  KX_Scene::KX_ScenegraphUpdateFunc,
                                                  KX_Scene::KX_ScenegraphRescheduleFunc);

KX_Scene::KX_Scene(SCA_IInputDevice *inputDevice,
                   const std::string &sceneName,
                   blender::Scene *scene,
                   class RAS_ICanvas *canvas,
                   KX_NetworkMessageManager *messageManager)
    : KX_PythonProxy(),
      m_gameDefaultCamera(nullptr),   // eevee
      m_currentGPUViewport(nullptr),  // eevee
      m_overlayCamera(nullptr),       // eevee (For overlay collections)
      m_overlayCameraSource(nullptr),
      m_overlayCameraReplicaRoot(nullptr),
      m_sceneConverter(nullptr),      // eevee
      m_isPythonMainLoop(false),      // eevee
      m_collectionRemap(false),       // eevee (to uncheck viewport restrictflag)
      m_relationsUpdatePending(false),
      /* Default to skipping culled objects to avoid unnecessary SG updates. */
      m_skipInvisibleInterpolation(true),
      m_keyboardmgr(nullptr),
      m_mousemgr(nullptr),
      m_physicsEnvironment(0),
      m_sceneName(sceneName),
      m_active_camera(nullptr),
      m_overrideCullingCamera(nullptr),
      m_ueberExecutionPriority(0),
      m_blenderScene(scene),
      m_isActivedHysteresis(false),
      m_lodHysteresisValue(0),
      m_isRuntime(true)  // eevee
{
  m_dbvt_culling = false;
  m_dbvt_occlusion_res = 0;
  m_activityCulling = false;
  m_objectlist = new EXP_ListValue<KX_GameObject>();
  m_parentlist = new EXP_ListValue<KX_GameObject>();
  m_lightlist = new EXP_ListValue<KX_LightObject>();
  m_inactivelist = new EXP_ListValue<KX_GameObject>();
  m_cameralist = new EXP_ListValue<KX_Camera>();
  m_fontlist = new EXP_ListValue<KX_FontObject>();
  m_upbgeDupliInstancesList.clear();

  m_filterManager = new KX_2DFilterManager();
  m_logicmgr = new SCA_LogicManager();

#ifdef WITH_GAMEENGINE_LOGICNODES
  m_logicNodeManager = std::make_shared<LN_Manager>(*this);
#endif

  m_timemgr = new SCA_TimeEventManager(m_logicmgr);
  m_keyboardmgr = new SCA_KeyboardManager(m_logicmgr, inputDevice);
  m_mousemgr = new SCA_MouseManager(m_logicmgr, inputDevice);

  SCA_ActuatorEventManager *actmgr = new SCA_ActuatorEventManager(m_logicmgr);
  SCA_BasicEventManager *basicmgr = new SCA_BasicEventManager(m_logicmgr);

  m_logicmgr->RegisterEventManager(actmgr);
  m_logicmgr->RegisterEventManager(m_keyboardmgr);
  m_logicmgr->RegisterEventManager(m_mousemgr);
  m_logicmgr->RegisterEventManager(m_timemgr);
  m_logicmgr->RegisterEventManager(basicmgr);

  SCA_JoystickManager *joymgr = new SCA_JoystickManager(m_logicmgr);
  m_logicmgr->RegisterEventManager(joymgr);

  m_networkMessageScene = new KX_NetworkMessageScene(messageManager);

  m_rootnode = nullptr;

  m_bucketmanager = new RAS_BucketManager();

  bool showObstacleSimulation = (scene->gm.flag & GAME_SHOW_OBSTACLE_SIMULATION) != 0;
  switch (scene->gm.obstacleSimulation) {
    case OBSTSIMULATION_TOI_rays:
      m_obstacleSimulation = new KX_ObstacleSimulationTOI_rays((MT_Scalar)scene->gm.levelHeight,
                                                               showObstacleSimulation);
      break;
    case OBSTSIMULATION_TOI_cells:
      m_obstacleSimulation = new KX_ObstacleSimulationTOI_cells((MT_Scalar)scene->gm.levelHeight,
                                                                showObstacleSimulation);
      break;
    default:
      m_obstacleSimulation = nullptr;
  }

#ifdef WITH_PYTHON
  m_attr_dict = nullptr;
  m_removeCallbacks = nullptr;

  for (unsigned short i = 0; i < MAX_DRAW_CALLBACK; ++i) {
    m_drawCallbacks[i] = nullptr;
  }
#endif

  /**************EEVEE INTEGRATION******************/

  /* 2.8+: Always set scene->lay to 1
   * to avoid potential conversion issues:
   * https://github.com/UPBGE/upbge/issues/1525
   */
  scene->lay = 1;

  m_kxobWithLod = {};
  m_obVisibilityFlag = {};
  m_backupOverlayFlag = -1;

  /* REMINDER TO SET blender::bContext */
  /* 1.MAIN, 2.blender::wmWindowManager, 3.blender::wmWindow, 4.blender::bScreen, 5.ScreenArea, 6.blender::ARegion,
   * 7.blender::Scene */

  /* In the case of SetScene actuator (not game restart or load .blend)
   * We might need to Set blender::bContext Variables here to be sure to have
   * the good environment.
   */
  ReinitBlenderContextVariables();

  blender::bContext *C = KX_GetActiveEngine()->GetContext();
  blender::Main *bmain = CTX_data_main(C);

  /* Update 3D view cameras and RV3D->persp state and ensure the ViewLayer is updated */
  ED_screen_scene_change(C, CTX_wm_window(C), scene, true);

  ViewLayer *view_layer = BKE_view_layer_default_view(scene);

  /* This ensures a depsgraph is allocated and activates it.
   * It is needed in KX_Scene constructor because we'll need
   * a depsgraph in BlenderDataConversion + Ensure evaluation
   * to avoid potential bugs (https://github.com/UPBGE/upbge/issues/1629)
   */
  CTX_data_ensure_evaluated_depsgraph(C);

  /* Always create a default camera in case no valid active camera is found
   * https://github.com/UPBGE/upbge/issues/1829
   * This camera will be added to kxscene.objects list only if needed */
  m_gameDefaultCamera = BKE_object_add_only_object(bmain, OB_CAMERA, "game_default_cam");
  m_gameDefaultCamera->data = (blender::ID *)BKE_object_obdata_add_from_type(
      bmain, OB_CAMERA, NULL);
  BKE_collection_object_add(bmain, scene->master_collection, m_gameDefaultCamera);
  /* Fix crash at start with some files: See 68589a31ebfb79165f99a979357d237e5413e904 */
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  Base *defaultCamBase = BKE_view_layer_base_find(view_layer, m_gameDefaultCamera);
  defaultCamBase->flag |= BASE_HIDDEN;
  DEG_relations_tag_update(bmain);

  m_overlay_collections = {};
  m_imageRenderCameraList = {};
  m_idsToUpdateInAllRenderPasses = {};
  m_idsToUpdateInOverlayPass = {};

  /* To backup and restore object_to_world */
  m_backupObList = {};

  /* Configure Shading types and overlays according to
   * (viewport render or not) and (blenderplayer or not)
   */
  CTX_wm_view3d(C)->shading.type = eDrawType(KX_GetActiveEngine()->ShadingTypeRuntime());

  if (!KX_GetActiveEngine()->UseViewportRender()) {
    /* We want to indicate that we are in bge runtime. The flag can be used in draw code but in
     * depsgraph code too later */
    scene->flag |= SCE_INTERACTIVE;
  }
  else {
    scene->flag |= SCE_INTERACTIVE_VIEWPORT;
  }

  DEG_register_bge_object_provider(bge_dupli_provider);
  g_dupli_provider_scenes++;

  /* Fix black shading issue with addObject https://github.com/UPBGE/upbge/issues/1354 */
  GPU_shader_force_unbind();
  /****************************************************/
}

KX_Scene::~KX_Scene()
{

#ifdef WITH_PYTHON
  RunOnRemoveCallbacks();
#endif  // WITH_PYTHON

  /* EEVEE INTEGRATION */

  m_isRuntime = false;  // eevee

  ReinitBlenderContextVariables();

  blender::Scene *scene = GetBlenderScene();
  blender::bContext *C = KX_GetActiveEngine()->GetContext();
  blender::Main *bmain = CTX_data_main(C);
  blender::Depsgraph *depsgraph = CTX_data_depsgraph_on_load(C);

  ClearOverlayStateForTeardown();

  /* Restore Objects object_to_world mat */
  if (scene->gm.flag & GAME_USE_UNDO) {
    RestoreObjectsMatToWorld();
    TagForObjectsMatToWorldRestore();
  }

  /* Renderers shared by camera passes belong to the scene. When the current one is shared, this
   * frees it and clears the current pointer, so it is not freed again below. */
  FreeSharedGPUViewports();

  /* The active camera never frees the current viewport itself (see #KX_Camera::RemoveGPUViewport),
   * so scene teardown must always release it to avoid restart-time leaks. Keep the pointer value
   * until objects are destroyed so camera destructors skip freeing it again. */
  const bool had_current_gpu_viewport = (m_currentGPUViewport != nullptr);
  if (had_current_gpu_viewport) {
    GPU_viewport_free(m_currentGPUViewport);
  }

  if (KX_GetActiveEngine()->UseViewportRender()) {
    // Free the allocated profile a last time.
    DRW_game_viewport_render_loop_end();
  }

  /* Fixes issue when switching .blend erm...*/
  GPU_shader_force_unbind();

  // Put that before we flush depsgraph updates at scene exit
  scene->flag &= ~SCE_INTERACTIVE;
  scene->flag &= ~SCE_INTERACTIVE_VIEWPORT;

  /* End of EEVEE INTEGRATION */

  // The release of debug properties used to be in SCA_IScene::~SCA_IScene
  // It's still there but we remove all properties here otherwise some
  // reference might be hanging and causing late release of objects
  RemoveAllDebugProperties();

#ifdef WITH_GAMEENGINE_LOGICNODES
  if (m_logicNodeManager) {
    m_logicNodeManager->BeginShutdown();
    m_logicNodeManager.reset();
  }
#endif

  while (GetRootParentList()->GetCount() > 0) {
    KX_GameObject *parentobj = GetRootParentList()->GetValue(0);
    this->RemoveObject(parentobj, KX_ObjectRemovalReason::SceneTeardown);
    BKE_view_layer_synced_ensure(*bmain, scene, BKE_view_layer_default_view(scene));
  }

  if (had_current_gpu_viewport) {
    m_currentGPUViewport = nullptr;
  }

  if (m_obstacleSimulation) {
    delete m_obstacleSimulation;
  }

  if (m_objectlist)
    m_objectlist->Release();

  if (m_gameDefaultCamera) {
    BKE_collection_object_remove(bmain, scene->master_collection, m_gameDefaultCamera, false);
    BKE_id_free(bmain, m_gameDefaultCamera);
    m_gameDefaultCamera = nullptr;
    DEG_relations_tag_update(bmain);
  }

  if (m_parentlist)
    m_parentlist->Release();

  if (m_inactivelist)
    m_inactivelist->Release();

  if (m_lightlist)
    m_lightlist->Release();

  if (m_cameralist) {
    m_cameralist->Release();
  }

  if (m_fontlist) {
    m_fontlist->Release();
  }

  m_upbgeDupliInstancesList.clear();

  if (m_filterManager) {
    delete m_filterManager;
  }

  if (m_logicmgr)
    delete m_logicmgr;

  if (m_physicsEnvironment) {
    delete m_physicsEnvironment;
    m_physicsEnvironment = nullptr;
  }

  if (m_networkMessageScene)
    delete m_networkMessageScene;

  if (m_bucketmanager) {
    delete m_bucketmanager;
  }
  if (m_sceneConverter) {
    delete m_sceneConverter;
  }

  if (--g_dupli_provider_scenes == 0) {
    DEG_unregister_bge_object_provider();
  }
  /* Nothing draws this scene's Dupli copies any more. */
  FreeRetiredDupliDrawOriginals(UINT64_MAX - 4);
  /* The removed copies' Blender objects kept for reuse go like the others did. */
  DeleteParkedCopies(UINT64_MAX);
  /* Merged scenery's and Auto groups' Blender objects (their members get their visibility back
   * below). */
  DeleteMergedBatches();
  DeleteSharedDrawBases();

  RestoreVisibilityFlag();

  BKE_scene_view_layers_synced_ensure(*bmain, scene);
  // Flush depsgraph updates a last time at ge exit
  BKE_scene_graph_update_tagged(depsgraph, bmain);
  /* No object draws these copies any more (each released its own in ~KX_GameObject). */
  for (const auto &item : m_replaceMeshEvalMeshes) {
    BKE_id_free(nullptr, &item.second->id);
  }
  m_replaceMeshEvalMeshes.clear();

#ifdef WITH_PYTHON
  if (m_attr_dict) {
    PyDict_Clear(m_attr_dict);
    /* Py_CLEAR: Py_DECREF's and nullptr's */
    Py_CLEAR(m_attr_dict);
  }

  // These may be nullptr but the macro checks.
  Py_CLEAR(m_removeCallbacks);
  /* these may be nullptr but the macro checks */
  for (unsigned short i = 0; i < MAX_DRAW_CALLBACK; ++i) {
    Py_CLEAR(m_drawCallbacks[i]);
  }
#endif
}

/*******************EEVEE INTEGRATION******************/

/* Each instance knows its place in the list: adding and removing one costs the same however many
 * there are (a scan per object made a level of 10,000 grouped objects start and end slowly). The
 * drawing order does not matter. */
void KX_Scene::AddUpbgeDupliInstanceToList(KX_GameObject *gameobj)
{
  const int index = gameobj->GetDupliListIndex();
  if (index >= 0 && index < int(m_upbgeDupliInstancesList.size()) &&
      m_upbgeDupliInstancesList[index] == gameobj)
  {
    return;
  }
  gameobj->SetDupliListIndex(int(m_upbgeDupliInstancesList.size()));
  m_upbgeDupliInstancesList.push_back(gameobj);
}

void KX_Scene::RemoveUpbgeDupliInstanceFromList(KX_GameObject *gameobj)
{
  const int index = gameobj->GetDupliListIndex();
  if (index < 0 || index >= int(m_upbgeDupliInstancesList.size()) ||
      m_upbgeDupliInstancesList[index] != gameobj)
  {
    return;
  }
  KX_GameObject *last = m_upbgeDupliInstancesList.back();
  m_upbgeDupliInstancesList[index] = last;
  last->SetDupliListIndex(index);
  m_upbgeDupliInstancesList.pop_back();
  gameobj->SetDupliListIndex(-1);
}

void KX_Scene::ForgetSharedMember(KX_GameObject *object)
{
  if (blender::Object *ob = object->GetBlenderObject()) {
    auto it = m_sharedMemberOfObject.find(ob);
    if (it != m_sharedMemberOfObject.end() && it->second == object) {
      m_sharedMemberOfObject.erase(it);
    }
  }
}

void KX_Scene::RetireDupliDrawOriginal(blender::Object *draw_original)
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  m_retiredDupliDrawOriginals.emplace_back(engine != nullptr ? engine->GetRenderFrame() : 0,
                                           draw_original);
}

void KX_Scene::FreeRetiredDupliDrawOriginals(const uint64_t frame)
{
  /* Every camera's renderer that drew since has dropped the address. */
  constexpr uint64_t frames_kept = 4;
  while (!m_retiredDupliDrawOriginals.empty() &&
         m_retiredDupliDrawOriginals.front().first + frames_kept <= frame)
  {
    delete m_retiredDupliDrawOriginals.front().second;
    m_retiredDupliDrawOriginals.pop_front();
  }
}

void KX_Scene::BeginDupliDrawGroups(blender::Depsgraph *depsgraph,
                                    const bool is_overlay_pass,
                                    const bool workbench)
{
  m_dupliGroupPass = 0;
  if (!m_dupliDrawGroups) {
    m_dupliDrawGroups = std::make_shared<KX_DupliDrawGroups>();
  }
  KX_DupliDrawGroups &draw = *m_dupliDrawGroups;
  draw.clear();
  /* UPBGE_DUPLI_GROUPS=0 draws every copy with its own entry, as before (tests compare both). */
  static const bool groups_off = [] {
    const char *value = getenv("UPBGE_DUPLI_GROUPS");
    return value != nullptr && value[0] == '0';
  }();
  /* Use Viewport Render draws through the editor's own drawing: the copies' own entries. */
  if (groups_off || m_upbgeDupliInstancesList.empty() || depsgraph == nullptr ||
      KX_GetActiveEngine()->UseViewportRender())
  {
    return;
  }
  m_dupliGroupPass = ++m_dupliGroupPassCount;
  const uint64_t update_count = DEG_get_update_count(depsgraph);
  /* Workbench's Random color is one per drawn object's name, so one per group: an Auto object
   * (drawn with its own name) keeps its own entry in such a view. */
  const blender::Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);
  const bool random_color = workbench && scene_eval != nullptr &&
                            scene_eval->display.shading.color_type == V3D_SHADING_RANDOM_COLOR;
  draw.instances.reserve(int64_t(m_upbgeDupliInstancesList.size()));

  blender::Object *base = nullptr;
  blender::Object *base_eval = nullptr;
  bool base_other_pass = false;
  bool base_drawable = false;
  for (KX_GameObject *game_obj : m_upbgeDupliInstancesList) {
    if (game_obj == nullptr || !game_obj->IsUpbgeDupliInstance() || !game_obj->GetVisible()) {
      continue;
    }
    /* Set Camera Visibility: not drawn by this camera, or only into its shadows. */
    bool keep_shadow = false;
    KX_Scene *object_scene = game_obj->GetScene();
    const bool hidden = object_scene != nullptr &&
                        object_scene->IsHiddenInCameraPass(game_obj, keep_shadow);
    if (hidden && !keep_shadow) {
      game_obj->SetDupliGroupedPass(m_dupliGroupPass);
      continue;
    }
    blender::Object *draw_base = game_obj->GetDupliDrawBase();
    if (draw_base == nullptr) {
      continue;
    }
    if (draw_base != base) {
      base = draw_base;
      base_eval = DEG_get_evaluated(depsgraph, base);
      /* The main pass draws no menu or HUD objects, an overlay pass only those. */
      base_other_pass = ((base->gameflag & OB_OVERLAY_COLLECTION) != 0) != is_overlay_pass;
      /* Only a base in Blender's graph (its evaluated copy): an original is drawn as before. */
      base_drawable = base_eval != nullptr && base_eval != base &&
                      dupli_group_drawable(*base_eval);
    }
    if (base_eval == nullptr) {
      continue;
    }
    if (base_other_pass) {
      game_obj->SetDupliGroupedPass(m_dupliGroupPass);
      continue;
    }
    if (!base_drawable || (random_color && game_obj->IsShared())) {
      continue;
    }

    float matrix[4][4];
    game_obj->NodeGetWorldTransform().getValue(&matrix[0][0]);
    const uint64_t moved_update = game_obj->UpdateDupliDrawnMove(matrix, update_count);
    KX_DupliDrawGroups::Key key;
    key.object = base_eval;
    game_obj->GetObjectColor().getValue(key.color);
    key.negative_scale = is_negative_m4(matrix);
    key.keep_shadow = hidden;
    key.moved = moved_update == update_count;
    const int index = draw.group_of.lookup_or_add_cb(key, [&]() {
      const int new_index = int(draw.groups.size());
      if (draw.lists.size() <= new_index) {
        draw.lists.append(std::make_unique<blender::VectorList<blender::DupliObject *>>());
      }
      blender::DRWGameInstanceGroup group;
      group.object = base_eval;
      group.instances = draw.lists[new_index].get();
      copy_v4_v4(group.color, key.color);
      group.negative_scale = key.negative_scale;
      group.keep_shadow = key.keep_shadow;
      draw.groups.append(group);
      return new_index;
    });
    blender::DRWGameInstanceGroup &group = draw.groups[index];
    group.last_update_transform = std::max(group.last_update_transform, moved_update);

    draw.instances.append(blender::DupliObject{});
    blender::DupliObject &dupli = draw.instances.last();
    copy_m4_m4(dupli.mat, matrix);
    dupli.ob = base_eval;
    dupli.preview_instance_index = -1;
    /* The copy's key in the renderers (motion, shadows) and its Object Info Random. */
    dupli.persistent_id[0] = game_obj->GetDupliDrawId();
    for (int level = 1; level < MAX_DUPLI_RECUR; level++) {
      dupli.persistent_id[level] = std::numeric_limits<int>::max();
    }
    dupli.random_id = game_obj->GetDupliDrawRandom();
    group.instances->append(&dupli);
    game_obj->SetDupliGroupedPass(m_dupliGroupPass);
  }
}

void KX_Scene::EndDupliDrawGroups()
{
  m_dupliGroupPass = 0;
}

KX_DupliDrawGroups &KX_Scene::GetDupliDrawGroups()
{
  if (!m_dupliDrawGroups) {
    m_dupliDrawGroups = std::make_shared<KX_DupliDrawGroups>();
  }
  return *m_dupliDrawGroups;
}

bool KX_Scene::IsDrawnInDupliGroup(const KX_GameObject *object) const
{
  return m_dupliGroupPass != 0 && object->GetDupliGroupedPass() == m_dupliGroupPass;
}

/**
 * Registers an object whose SG node currently contains a temporary render pose.
 *
 * The per-object flag makes the common duplicate check O(1). The dense scene list lets the next
 * frame restore only objects that were actually interpolated instead of scanning the whole scene.
 */
void KX_Scene::RegisterInterpolatedObject(KX_GameObject *gameobj)
{
  if (gameobj == nullptr || gameobj->m_interpolationListRegistered) {
    return;
  }

  m_interpolatedObjects.push_back(gameobj);
  gameobj->m_interpolationListRegistered = true;
}

/** Removes an object from non-owning interpolation lists before it can be destroyed. */
void KX_Scene::UnregisterInterpolatedObject(KX_GameObject *gameobj)
{
  if (gameobj == nullptr) {
    return;
  }

  if (gameobj->m_interpolationListRegistered) {
    m_interpolatedObjects.erase(
        std::remove(m_interpolatedObjects.begin(), m_interpolatedObjects.end(), gameobj),
        m_interpolatedObjects.end());
    gameobj->m_interpolationListRegistered = false;
  }
}

/**
 * Sets whether to skip invisible interpolation.
 *
 * This is used for visibility gating, where objects are only rendered when they are visible.
 * If set to true, invisible objects will not be interpolated.
 *
 * @param skip Whether to skip invisible interpolation.
 */
void KX_Scene::SetSkipInvisibleInterpolation(bool skip)
{
  m_skipInvisibleInterpolation = skip;
}

bool KX_Scene::GetSkipInvisibleInterpolation() const
{
  return m_skipInvisibleInterpolation;
}

void KX_Scene::ReinitBlenderContextVariables()
{
  blender::bContext *C = KX_GetActiveEngine()->GetContext();
  blender::wmWindowManager *wm = CTX_wm_manager(C);
  blender::wmWindow *win = (blender::wmWindow *)wm->windows.first;
  blender::bScreen *screen = WM_window_get_active_screen(win);

  for (blender::ScrArea *sa = (blender::ScrArea *)screen->areabase.first; sa; sa = sa->next) {
    /* We choose the biggest blender::ScrArea to match the behaviour in WM_init_game */
    if (sa->spacetype == SPACE_VIEW3D && sa == BKE_screen_find_big_area(screen, SPACE_VIEW3D, 0)) {
      blender::ListBase *regionbase = &sa->regionbase;
      for (blender::ARegion *region = (blender::ARegion *)regionbase->first; region;
           region = region->next)
      {
        if (region->regiontype == RGN_TYPE_WINDOW) {
          if (region->regiondata) {
            CTX_wm_window_set(C, win);
            CTX_wm_screen_set(C, screen);
            CTX_wm_area_set(C, sa);
            CTX_wm_region_set(C, region);
            CTX_data_scene_set(C, GetBlenderScene());
            SpaceType *st;
            ARegionType *art;
            st = BKE_spacetype_from_id(SPACE_VIEW3D);
            art = BKE_regiontype_from_id(st, RGN_TYPE_WINDOW);
            region->runtime->type = art;
            region->regiontype = RGN_TYPE_WINDOW;
            win->scene = GetBlenderScene();
            screen->active_region = region;
            return;
          }
        }
      }
    }
  }
}

KX_Scene::EditorViewState KX_Scene::SaveEditorViewState()
{
  EditorViewState state;
  blender::bContext *C = KX_GetActiveEngine()->GetContext();
  state.context_window = CTX_wm_window(C);
  state.context_screen = CTX_wm_screen(C);
  state.context_area = CTX_wm_area(C);
  state.context_region = CTX_wm_region(C);
  state.context_scene = CTX_data_scene(C);
  blender::wmWindow *win = CTX_wm_window(C);
  if (win == nullptr) {
    blender::wmWindowManager *wm = CTX_wm_manager(C);
    win = wm != nullptr ? static_cast<blender::wmWindow *>(wm->windows.first) : nullptr;
  }
  if (win == nullptr) {
    return state;
  }
  state.window = win;
  state.scene = win->scene;
  state.view_layer_name = win->view_layer_name;
  blender::bScreen *screen = WM_window_get_active_screen(win);
  if (screen == nullptr) {
    return state;
  }
  const auto save_regions = [&](blender::ListBaseT<blender::ARegion> &regions) {
    for (blender::ARegion &region : regions) {
      if (region.regiontype == RGN_TYPE_WINDOW && region.regiondata != nullptr) {
        auto *rv3d = static_cast<blender::RegionView3D *>(region.regiondata);
        state.region_views.push_back({rv3d, int(rv3d->persp), int(rv3d->rflag)});
      }
    }
  };
  for (blender::ScrArea &area : screen->areabase) {
    for (blender::SpaceLink &space : area.spacedata) {
      if (space.spacetype != SPACE_VIEW3D) {
        continue;
      }
      blender::View3D *v3d = reinterpret_cast<blender::View3D *>(&space);
      state.view_cameras.push_back({v3d, v3d->camera});
      /* The shown space's regions are the area's (ED_screen_scene_change does the same). */
      save_regions(&space == area.spacedata.first ? area.regionbase : v3d->regionbase);
    }
  }
  return state;
}

void KX_Scene::RestoreEditorViewState(const EditorViewState &state)
{
  blender::bContext *C = KX_GetActiveEngine()->GetContext();
  CTX_wm_window_set(C, state.context_window);
  CTX_wm_screen_set(C, state.context_screen);
  CTX_wm_area_set(C, state.context_area);
  CTX_wm_region_set(C, state.context_region);
  CTX_data_scene_set(C, state.context_scene);
  if (state.window == nullptr) {
    return;
  }
  state.window->scene = state.scene;
  const size_t name_size = sizeof(state.window->view_layer_name);
  std::strncpy(state.window->view_layer_name, state.view_layer_name.c_str(), name_size - 1);
  state.window->view_layer_name[name_size - 1] = '\0';
  for (const auto &[v3d, camera] : state.view_cameras) {
    v3d->camera = camera;
  }
  for (const EditorViewState::RegionView &view : state.region_views) {
    view.region_view->persp = blender::eRegionView3D_Persp(view.persp);
    view.region_view->rflag = blender::eRegionView3D_Flag(view.rflag);
  }
}

blender::Object *KX_Scene::GetGameDefaultCamera()
{
  return m_gameDefaultCamera;
}

static bool OverlayCollectionsContainObject(const std::vector<blender::Collection *> &collections,
                                            blender::Object *object)
{
  if (object == nullptr) {
    return false;
  }
  return std::any_of(collections.begin(), collections.end(), [object](blender::Collection *item) {
    return item != nullptr && BKE_collection_has_object_recursive(item, object);
  });
}

bool KX_OverlayCollectionResultSucceeded(const KX_OverlayCollectionResult result)
{
  return result == KX_OverlayCollectionResult::Success ||
         result == KX_OverlayCollectionResult::NoChange;
}

const char *KX_OverlayCollectionResultMessage(const KX_OverlayCollectionResult result)
{
  switch (result) {
    case KX_OverlayCollectionResult::Success:
      return "overlay collection request succeeded";
    case KX_OverlayCollectionResult::NoChange:
      return "overlay collection already has the requested state";
    case KX_OverlayCollectionResult::MissingCamera:
      return "overlay collection request has no camera";
    case KX_OverlayCollectionResult::MissingCollection:
      return "overlay collection request has no collection";
    case KX_OverlayCollectionResult::CameraWrongScene:
      return "overlay camera does not belong to the receiving runtime scene";
    case KX_OverlayCollectionResult::CameraUnavailable:
      return "overlay camera is active, used by ImageRender, or has a custom viewport";
    case KX_OverlayCollectionResult::CameraConflict:
      return "active overlay collections in this scene use a different camera";
    case KX_OverlayCollectionResult::CollectionWrongScene:
      return "overlay collection is not linked to the receiving runtime scene";
    case KX_OverlayCollectionResult::CollectionSharedAcrossScenes:
      return "overlay collection is linked to more than one scene";
    case KX_OverlayCollectionResult::CollectionHierarchyConflict:
      return "overlay collection contains an already active child overlay; remove the child "
             "overlay before enabling its parent";
    case KX_OverlayCollectionResult::CollectionOverlap:
      return "overlay collection overlaps an active overlay collection without containing it";
    case KX_OverlayCollectionResult::CollectionHasNoRuntimeObjects:
      return "overlay collection has no converted objects in the receiving runtime scene";
    case KX_OverlayCollectionResult::RuntimeUnavailable:
      return "game-engine runtime context is unavailable";
    case KX_OverlayCollectionResult::ObjectInstantiationFailed:
      return "an inactive overlay collection object could not be instantiated for runtime "
             "rendering";
  }
  return "unknown overlay collection failure";
}

static bool OverlayCollectionBelongsToScene(const KX_Scene &scene,
                                            const blender::Collection *collection)
{
  const blender::Scene *blender_scene = scene.GetBlenderScene();
  return collection != nullptr && blender_scene != nullptr &&
         blender_scene->master_collection != nullptr &&
         (blender_scene->master_collection == collection ||
          BKE_collection_has_collection(blender_scene->master_collection, collection));
}

static bool OverlayCollectionHasRuntimeObjects(const KX_Scene &scene,
                                               blender::Collection *collection)
{
  auto list_contains_collection_object = [collection](EXP_ListValue<KX_GameObject> *objects) {
    if (objects == nullptr) {
      return false;
    }
    for (KX_GameObject *game_object : *objects) {
      blender::Object *object = game_object ? game_object->GetBlenderObject() : nullptr;
      if (object != nullptr && BKE_collection_has_object_recursive(collection, object)) {
        return true;
      }
    }
    return false;
  };
  return list_contains_collection_object(scene.GetObjectList()) ||
         list_contains_collection_object(scene.GetInactiveList());
}

static bool OverlayCollectionContainsCollection(const blender::Collection *parent,
                                                const blender::Collection *child)
{
  return parent != nullptr && child != nullptr &&
         (parent == child || BKE_collection_has_collection(parent, child));
}

static bool OverlayCollectionsShareObject(blender::Collection *a, blender::Collection *b)
{
  if (a == nullptr || b == nullptr) {
    return false;
  }
  FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (a, object) {
    if (object != nullptr && BKE_collection_has_object_recursive(b, object)) {
      return true;
    }
  }
  FOREACH_COLLECTION_OBJECT_RECURSIVE_END;
  return false;
}

static bool StageOverlayReplicaRenderObjects(
    blender::Main &bmain,
    blender::Scene &scene,
    const std::vector<KX_GameObject *> &created_members)
{
  if (scene.master_collection == nullptr) {
    return false;
  }

  /* A globally viewport-disabled authoring collection is intentionally absent from Blender's
   * viewport depsgraph. Overlay replicas therefore need one visible runtime-only collection path;
   * changing their object visibility is not sufficient. Every distinct temporary Blender Object
   * must be linked directly to the scene master collection (KX_GameObject::ReplicateBlenderObject
   * already links each copy there; this links any that is not). The overlay draw flag still
   * excludes these objects from the main pass, while m_overlayMemberSources retains their
   * authored collection identity for interaction, collection visibility, pause processing, and
   * removal. */
  std::unordered_set<blender::Object *> staged_objects;
  staged_objects.reserve(created_members.size());
  for (KX_GameObject *created_member : created_members) {
    if (created_member == nullptr || !created_member->IsReplica()) {
      continue;
    }
    blender::Object *render_object = created_member->GetBlenderObject();
    if (render_object == nullptr || !staged_objects.insert(render_object).second ||
        BKE_collection_has_object(scene.master_collection, render_object))
    {
      continue;
    }
    if (!BKE_collection_object_add(&bmain, scene.master_collection, render_object)) {
      return false;
    }
  }
  return true;
}

bool KX_Scene::CreateOverlayCameraRuntimeInstance(KX_Camera *source_camera,
                                                  blender::Main &bmain,
                                                  blender::Scene &scene)
{
  BLI_assert(m_overlayCamera == nullptr);
  BLI_assert(m_overlayCameraSource == nullptr);
  BLI_assert(m_overlayCameraReplicaRoot == nullptr);
  BLI_assert(m_overlayCameraReplicaMembers.empty());
  if (source_camera == nullptr || source_camera->GetGameObjectType() != SCA_IObject::OBJ_CAMERA) {
    return false;
  }

  /* A converted, already-active auxiliary camera already owns a complete runtime lifecycle. Keep
   * the long-standing lightweight path for it and create an owned instance only for an inactive
   * authoring template. This avoids running an active camera's logic and components twice. */
  if (GetObjectList()->SearchValue(source_camera)) {
    m_overlayCamera = source_camera;
    m_overlayCameraSource = source_camera;
    return true;
  }
  if (!GetInactiveList()->SearchValue(source_camera)) {
    return false;
  }

  /* The authored camera can live in a globally viewport-disabled collection. Such a camera is a
   * conversion-time template: it has no active scene processing, components, animation, or Logic
   * Nodes. Instantiate only the camera itself as a scene-owned runtime root. This preserves its
   * complete authored world transform without duplicating visible parent rigs from the overlay
   * collection, and gives rendering, interaction, and camera-owned behavior one live identity. */
  const std::set<KX_GameObject *> camera_filter = {source_camera};
  std::vector<KX_GameObject *> created_members;
  std::unordered_map<KX_GameObject *, KX_GameObject *> created_mapping;
  KX_GameObject *replica_root = AddReplicaObject(source_camera,
                                                 source_camera,
                                                 0.0f,
                                                 &created_members,
                                                 &camera_filter,
                                                 &created_mapping);
  const auto camera_iter = created_mapping.find(source_camera);
  KX_GameObject *camera_object = camera_iter != created_mapping.end() ? camera_iter->second :
                                                                       nullptr;
  if (replica_root == nullptr || camera_object == nullptr ||
      camera_object->GetGameObjectType() != SCA_IObject::OBJ_CAMERA ||
      replica_root->GetBlenderObject() == nullptr)
  {
    if (replica_root != nullptr) {
      DelayedRemoveObject(replica_root, KX_ObjectRemovalReason::SystemCleanup);
      replica_root->Release();
    }
    return false;
  }

  /* AddReplicaObject's general Add Object placement contract uses the source hierarchy root
   * scale. The overlay camera is intentionally detached into a new root, so restore its complete
   * authored world scale before the first camera matrix or ray is calculated. */
  replica_root->NodeSetWorldScale(source_camera->NodeGetWorldScaling());
  replica_root->GetSGNode()->UpdateWorldData(0.0);
  rebase_physics_interpolation_hierarchy(replica_root->GetSGNode());

  if (!StageOverlayReplicaRenderObjects(bmain, scene, created_members)) {
    DelayedRemoveObject(replica_root, KX_ObjectRemovalReason::SystemCleanup);
    replica_root->Release();
    return false;
  }

  m_overlayCamera = static_cast<KX_Camera *>(camera_object);
  m_overlayCameraSource = source_camera;
  m_overlayCameraReplicaRoot = replica_root;
  m_overlayCameraReplicaMembers.insert(created_members.begin(), created_members.end());

  /* AddReplicaObject returns one caller-owned reference in addition to the scene's object and
   * parent-list references. Overlay ownership is represented by the raw scene-local fields above. */
  replica_root->Release();
  return true;
}

void KX_Scene::RestoreOverlayCameraViewState(KX_Camera *runtime_camera)
{
  /* The overlay camera is rendered last, so its GPU viewport is normally also the scene's current
   * viewport. KX_Camera deliberately refuses to free that borrowed current pointer in its
   * destructor. Transfer the scene and View3D state before releasing an owned camera or returning
   * a borrowed auxiliary camera to ordinary scene use. */
  if (runtime_camera == nullptr) {
    return;
  }
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  blender::bContext *context = engine ? engine->GetContext() : nullptr;
  blender::View3D *view3d = context ? CTX_wm_view3d(context) : nullptr;
  blender::Object *runtime_object = runtime_camera->GetBlenderObject();
  if (view3d != nullptr && view3d->camera == runtime_object) {
    view3d->camera = m_active_camera ? m_active_camera->GetBlenderObject() : nullptr;
  }
  if (m_currentGPUViewport == runtime_camera->m_gpuViewport) {
    m_currentGPUViewport = nullptr;
  }
}

void KX_Scene::DestroyOverlayCameraRuntimeInstance(const bool queue_removal)
{
  KX_Camera *runtime_camera = m_overlayCamera;
  KX_GameObject *replica_root = m_overlayCameraReplicaRoot;
  const std::unordered_set<KX_GameObject *> replica_members = m_overlayCameraReplicaMembers;

  RestoreOverlayCameraViewState(runtime_camera);
  if (replica_root != nullptr) {
    /* Owned temporary cameras are queued for deletion, so release their potentially very large
     * draw viewport immediately. Borrowed active cameras retain theirs for future scene use. */
    BLI_assert(runtime_camera != nullptr);
    if (runtime_camera != nullptr) {
      runtime_camera->RemoveGPUViewport();
      BLI_assert(runtime_camera->m_gpuViewport == nullptr);
    }
  }

  /* Clear every observer-visible pointer before queuing destruction. DelayedRemoveObject can
   * synchronously notify Logic Nodes and components, and no callback may retain this camera as a
   * valid render or interaction view once the final overlay owner has released it. */
  m_overlayCamera = nullptr;
  m_overlayCameraSource = nullptr;
  m_overlayCameraReplicaRoot = nullptr;
  m_overlayCameraReplicaMembers.clear();

  if (!queue_removal || replica_root == nullptr) {
    return;
  }

  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  blender::bContext *context = engine ? engine->GetContext() : nullptr;
  blender::Main *bmain = context ? CTX_data_main(context) : nullptr;
  blender::Scene *scene = GetBlenderScene();
  if (bmain != nullptr && scene != nullptr && scene->master_collection != nullptr) {
    for (KX_GameObject *member : replica_members) {
      blender::Object *object = member && member->IsReplica() ? member->GetBlenderObject() :
                                                               nullptr;
      if (object != nullptr && BKE_collection_has_object(scene->master_collection, object)) {
        BKE_collection_object_remove(bmain, scene->master_collection, object, false);
      }
    }
    TagForCollectionRemap();
  }
  DelayedRemoveObject(replica_root, KX_ObjectRemovalReason::SystemCleanup);
}

static bool OverlayCollectionIsSharedAcrossScenes(const KX_Scene &owner_scene,
                                                  const blender::Collection *collection,
                                                  blender::Main &bmain)
{
  const blender::Scene *owner_blender_scene = owner_scene.GetBlenderScene();
  for (blender::Scene &scene : bmain.scenes) {
    if (&scene != owner_blender_scene && scene.master_collection != nullptr &&
        (scene.master_collection == collection ||
         BKE_collection_has_collection(scene.master_collection, collection)))
    {
      return true;
    }
  }
  return false;
}

bool KX_Scene::AnyRuntimeOverlayContainsObject(blender::Object *object) const
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  EXP_ListValue<KX_Scene> *scenes = engine ? engine->CurrentScenes() : nullptr;
  if (object == nullptr || scenes == nullptr) {
    return false;
  }
  for (KX_Scene *scene : *scenes) {
    if (scene == nullptr) {
      continue;
    }
    if (OverlayCollectionsContainObject(scene->m_overlay_collections, object)) {
      return true;
    }
    for (const auto &item : scene->m_overlayCollectionMembers) {
      const bool contains_object = std::any_of(
          item.second.begin(), item.second.end(), [object](KX_GameObject *game_object) {
            return game_object != nullptr && game_object->GetBlenderObject() == object;
          });
      if (contains_object) {
        return true;
      }
    }
  }
  return false;
}

void KX_Scene::ClearOverlayStateForTeardown()
{
  std::unordered_set<blender::Object *> affected_objects;
  for (blender::Collection *collection : m_overlay_collections) {
    if (collection == nullptr) {
      continue;
    }
    FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (collection, object) {
      if (object != nullptr) {
        affected_objects.insert(object);
      }
    }
    FOREACH_COLLECTION_OBJECT_RECURSIVE_END;
  }
  for (const auto &item : m_overlayCollectionMembers) {
    for (KX_GameObject *game_object : item.second) {
      if (game_object != nullptr && game_object->GetBlenderObject() != nullptr) {
        affected_objects.insert(game_object->GetBlenderObject());
      }
    }
  }
  std::unordered_set<KX_GameObject *> shown_objects;
  for (const auto &item : m_overlayCollectionShownObjects) {
    shown_objects.insert(item.second.begin(), item.second.end());
  }
  for (KX_GameObject *game_object : shown_objects) {
    if (game_object == nullptr) {
      continue;
    }
    game_object->SetOverlayVisible(false);
    game_object->SetPresentationVisible(true);
    blender::Object *object = game_object->GetBlenderObject();
    if (object != nullptr) {
      /* Membership in m_overlayCollectionShownObjects records that Disable in Renders was set
       * before the overlay exposed this active object. Explicit object visibility is a separate
       * runtime layer and must not decide whether that authored render flag is restored. */
      object->visibility_flag |= OB_HIDE_RENDER;
    }
  }

  m_overlay_collections.clear();
  m_overlayCollectionReplicas.clear();
  m_overlayCollectionMembers.clear();
  m_overlayCollectionShownObjects.clear();
  m_overlayMemberSources.clear();
  m_overlayMemberInstance.clear();
  m_overlayCopyOfSource.clear();
  m_overlayCopyZeroRemoved.clear();
  m_overlayCopyRefusedWarned.clear();
  m_overlayInstanceLease.clear();
  m_overlayRuntimeObjects.clear();
  m_runtimeCollectionRenderVisibility.clear();
  m_runtimeVisibleCollections.clear();
  m_overlayResources.Clear();
  m_idsToUpdateInOverlayPass.clear();
  DestroyOverlayCameraRuntimeInstance(false);
  for (blender::Object *object : affected_objects) {
    if (AnyRuntimeOverlayContainsObject(object)) {
      object->gameflag |= OB_OVERLAY_COLLECTION;
    }
    else {
      object->gameflag &= ~OB_OVERLAY_COLLECTION;
    }
  }
}

void KX_Scene::RebuildOverlayRuntimeObjectIndex()
{
  m_overlayRuntimeObjects.clear();

  if (EXP_ListValue<KX_GameObject> *objects = GetObjectList()) {
    for (KX_GameObject *game_object : *objects) {
      blender::Object *object = game_object ? game_object->GetBlenderObject() : nullptr;
      if (object != nullptr && OverlayCollectionsContainObject(m_overlay_collections, object)) {
        m_overlayRuntimeObjects.insert(game_object);
      }
    }
  }
  for (const auto &item : m_overlayCollectionMembers) {
    m_overlayRuntimeObjects.insert(item.second.begin(), item.second.end());
  }
  /* Overlay copies come and go with their overlay, whose bookkeeping counts their Blender
   * objects: never parked for reuse (ParkCopy). */
  for (KX_GameObject *game_object : m_overlayRuntimeObjects) {
    game_object->NoteBlenderObjectChanged();
  }
}

bool KX_Scene::IsOverlayRuntimeObject(const KX_GameObject *game_object) const
{
  return game_object != nullptr &&
         m_overlayRuntimeObjects.contains(const_cast<KX_GameObject *>(game_object));
}

void KX_Scene::ApplyRuntimeCollectionRenderVisibility(KX_GameObject *game_object)
{
  if (game_object == nullptr) {
    return;
  }
  bool visible = true;
  if (!m_runtimeCollectionRenderVisibility.empty()) {
    /* Drawn when one of its collections is: an Add Object copy by its source's collections (its
     * own copy is linked elsewhere; every copy was hidden before 2026-10-15), and with Set
     * Collection Member's changes. An object in no collection is not hidden here. */
    const std::vector<const blender::Collection *> &collections = CollectionsOf(game_object);
    visible = collections.empty() ||
              std::any_of(collections.begin(),
                          collections.end(),
                          [&](const blender::Collection *collection) {
                            return m_runtimeVisibleCollections.contains(collection);
                          });
  }
  /* Set Collection Visibility on an overlay collection is kept by its overlay resource. Both
   * share this layer, so combine them: neither may undo the other. */
  if (visible && m_overlayRuntimeObjects.contains(game_object)) {
    for (blender::Collection *collection : m_overlay_collections) {
      if (m_overlayResources.Has(collection) && !m_overlayResources.CreatorVisible(collection) &&
          RuntimeObjectBelongsToOverlayCollection(game_object, collection))
      {
        visible = false;
        break;
      }
    }
  }
  game_object->SetCollectionVisible(visible);
}

void KX_Scene::RebuildRuntimeCollectionRenderVisibility()
{
  m_runtimeVisibleCollections.clear();
  const blender::Scene *scene = GetBlenderScene();
  if (scene == nullptr || scene->master_collection == nullptr) {
    return;
  }

  const auto collect_visible = [&](const auto &self,
                                   blender::Collection *collection,
                                   const bool parent_visible) -> void {
    if (collection == nullptr) {
      return;
    }
    bool collection_visible = parent_visible;
    if (const auto override = m_runtimeCollectionRenderVisibility.find(collection);
        override != m_runtimeCollectionRenderVisibility.end())
    {
      collection_visible = collection_visible && override->second;
    }
    /* A collection under two parents is visible when one path to it is. */
    if (collection_visible && !m_runtimeVisibleCollections.insert(collection).second) {
      return;
    }
    for (blender::CollectionChild &child : collection->children) {
      self(self, child.collection, collection_visible);
    }
  };
  collect_visible(collect_visible, scene->master_collection, true);

  if (EXP_ListValue<KX_GameObject> *objects = GetObjectList()) {
    for (KX_GameObject *game_object : *objects) {
      ApplyRuntimeCollectionRenderVisibility(game_object);
    }
  }
}

bool KX_Scene::SetRuntimeCollectionRenderVisibility(
    blender::Collection *collection,
    const bool visible,
    const bool include_child_collections)
{
  if (!OverlayCollectionBelongsToScene(*this, collection)) {
    return false;
  }

  /* An overlay collection keeps its visibility in its overlay resource; a plain collection keeps
   * it as an override. Each call replaces the other kind, so a parent's call never leaves a stale
   * value that a later call on the overlay itself cannot undo. */
  std::vector<blender::Collection *> overlays;
  bool overrides_changed = false;
  const auto set_visibility = [&](const auto &self, blender::Collection *item) -> void {
    if (item == nullptr) {
      return;
    }
    if (m_overlayResources.Has(item)) {
      m_overlayResources.SetCreatorVisible(item, visible);
      overrides_changed |= m_runtimeCollectionRenderVisibility.erase(item) != 0;
      overlays.push_back(item);
    }
    else {
      m_runtimeCollectionRenderVisibility[item] = visible;
      overrides_changed = true;
    }
    if (include_child_collections) {
      for (blender::CollectionChild &child : item->children) {
        self(self, child.collection);
      }
    }
  };
  set_visibility(set_visibility, collection);
  if (overrides_changed) {
    RebuildRuntimeCollectionRenderVisibility();
  }
  for (blender::Collection *overlay : overlays) {
    ApplyOverlayResourceVisibility(overlay);
  }
  return true;
}

KX_OverlayCollectionResult KX_Scene::ValidateAddOverlayCollection(
    KX_Camera *overlay_cam, blender::Collection *collection)
{
  overlay_cam = ResolveOverlayRequestCamera(overlay_cam);
  if (overlay_cam == nullptr) {
    return KX_OverlayCollectionResult::MissingCamera;
  }
  if (collection == nullptr) {
    return KX_OverlayCollectionResult::MissingCollection;
  }
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  blender::bContext *context = engine ? engine->GetContext() : nullptr;
  blender::Main *bmain = context ? CTX_data_main(context) : nullptr;
  if (bmain == nullptr) {
    return KX_OverlayCollectionResult::RuntimeUnavailable;
  }
  if (overlay_cam->GetScene() != this) {
    return KX_OverlayCollectionResult::CameraWrongScene;
  }
  if (!OverlayCollectionBelongsToScene(*this, collection)) {
    return KX_OverlayCollectionResult::CollectionWrongScene;
  }
  if (OverlayCollectionIsSharedAcrossScenes(*this, collection, *bmain)) {
    return KX_OverlayCollectionResult::CollectionSharedAcrossScenes;
  }
  if (!CameraIsInactive(overlay_cam)) {
    return KX_OverlayCollectionResult::CameraUnavailable;
  }
  if (m_overlayCameraSource != nullptr && m_overlayCameraSource != overlay_cam &&
      !OverlayCameraMatches(overlay_cam))
  {
    return KX_OverlayCollectionResult::CameraConflict;
  }
  const KX_OverlayCollectionResult independent = m_overlayResources.ValidateIndependentEnable(
      collection);
  if (independent == KX_OverlayCollectionResult::NoChange) {
    return independent;
  }
  for (blender::Collection *active_collection : m_overlay_collections) {
    if (active_collection == collection) {
      continue;
    }
    /* An active recursive parent already renders and owns the requested child domain. */
    if (OverlayCollectionContainsCollection(active_collection, collection)) {
      return KX_OverlayCollectionResult::CollectionHierarchyConflict;
    }
    /* A parent collection must be instantiated as one complete hierarchy. Reusing a previously
     * detached child replica would break inherited transforms when that child's authored parent
     * belongs only to the newly requested parent collection. Fail before mutating runtime state;
     * callers can remove the child and then enable the parent. */
    if (OverlayCollectionContainsCollection(collection, active_collection)) {
      return KX_OverlayCollectionResult::CollectionHierarchyConflict;
    }
    /* Arbitrary multi-linked overlap has no unambiguous removal owner, so fail explicitly instead
     * of creating duplicate draw instances, logic trees, and physics bodies. */
    if (OverlayCollectionsShareObject(collection, active_collection)) {
      return KX_OverlayCollectionResult::CollectionOverlap;
    }
  }
  if (!m_overlayResources.Has(collection) &&
      !OverlayCollectionHasRuntimeObjects(*this, collection))
  {
    return KX_OverlayCollectionResult::CollectionHasNoRuntimeObjects;
  }
  return KX_OverlayCollectionResult::Success;
}

KX_OverlayCollectionResult KX_Scene::AddOverlayCollection(
    KX_Camera *overlay_cam,
    blender::Collection *collection,
    const bool report_failure)
{
  overlay_cam = ResolveOverlayRequestCamera(overlay_cam);
  const KX_OverlayCollectionResult validation = ValidateAddOverlayCollection(overlay_cam,
                                                                             collection);
  if (validation == KX_OverlayCollectionResult::NoChange) {
    return validation;
  }
  if (!KX_OverlayCollectionResultSucceeded(validation)) {
    if (report_failure) {
      CM_Warning(KX_OverlayCollectionResultMessage(validation));
    }
    return validation;
  }
  if (m_overlayResources.Has(collection)) {
    m_overlayResources.SetIndependentEnabled(collection, true);
    /* Its copy 0 went with the player who closed it while another player's copy stayed: the
     * overlay itself shows a copy of its own again. */
    if (m_overlayCopyZeroRemoved.contains(collection) && CopyOverlayTemplates(collection, 0)) {
      m_overlayCopyZeroRemoved.erase(collection);
    }
    ApplyOverlayResourceVisibility(collection);
    return KX_OverlayCollectionResult::Success;
  }
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  blender::bContext *context = engine ? engine->GetContext() : nullptr;
  blender::Main *bmain = context ? CTX_data_main(context) : nullptr;
  blender::Scene *blender_scene = GetBlenderScene();
  if (bmain == nullptr || blender_scene == nullptr ||
      blender_scene->master_collection == nullptr)
  {
    if (report_failure) {
      CM_Warning(
          KX_OverlayCollectionResultMessage(KX_OverlayCollectionResult::RuntimeUnavailable));
    }
    return KX_OverlayCollectionResult::RuntimeUnavailable;
  }
  if (engine != nullptr && engine->UseViewportRender() && !m_viewportRenderOverlayWarned) {
    /* The collection opens (logic, input and pause work) but Use Viewport Render draws only the
     * game camera, so nothing of it shows. Said once per scene, not per frame. */
    m_viewportRenderOverlayWarned = true;
    CM_Warning("Use Viewport Render shows only the game camera: object menus and overlay "
               "collections (such as a HUD) open but are not drawn. Turn Use Viewport Render off "
               "(Render tab > Game Resolution) to see them.");
  }
  const bool created_overlay_camera = (m_overlayCamera == nullptr);
  if (created_overlay_camera &&
      !CreateOverlayCameraRuntimeInstance(overlay_cam, *bmain, *blender_scene))
  {
    if (report_failure) {
      CM_Warning(
          KX_OverlayCollectionResultMessage(KX_OverlayCollectionResult::ObjectInstantiationFailed));
    }
    return KX_OverlayCollectionResult::ObjectInstantiationFailed;
  }
  BLI_assert(m_overlayCamera != nullptr);
  BLI_assert(m_overlayCameraSource == overlay_cam);
  const std::vector<blender::Collection *> previous_collections = m_overlay_collections;
  m_overlay_collections.push_back(collection);

  /* Collection iteration can skip excluded data, but every converted member receives the
   * overlay-only render flag before this update is flushed. */
  FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (collection, collection_object) {
    if ((collection_object->gameflag & OB_OVERLAY_COLLECTION) == 0) {
      collection_object->gameflag |= OB_OVERLAY_COLLECTION;
      AppendToIdsToUpdate(&collection_object->id, ID_RECALC_SYNC_TO_EVAL, true);
    }
  }
  FOREACH_COLLECTION_OBJECT_RECURSIVE_END;

  /* Build the complete hidden-template candidate set before replication mutates the active
   * scene, then replicate only roots within that set. A visible active parent is deliberately not
   * a candidate: its hidden child still needs its own overlay replica. */
  std::vector<KX_GameObject *> overlay_template_objects;
  std::set<KX_GameObject *> overlay_template_object_set;
  const auto append_template = [&](KX_GameObject *gameobj) {
    blender::Object *object = gameobj ? gameobj->GetBlenderObject() : nullptr;
    if (gameobj != overlay_cam && object != nullptr &&
        BKE_collection_has_object_recursive(collection, object) &&
        !OverlayCollectionsContainObject(previous_collections, object))
    {
      const bool inserted = overlay_template_object_set.insert(gameobj).second;
      if (inserted) {
        overlay_template_objects.push_back(gameobj);
      }
    }
  };
  for (KX_GameObject *gameobj : GetInactiveList()) {
    append_template(gameobj);
  }
  /* Disable in Renders does not necessarily move an object to the inactive list. Expose that
   * already-active runtime object through a separate overlay visibility layer, preserving its
   * explicit visibility state and avoiding duplicate logic or physics. Runtime Set Visible false
   * only sets OB_HIDE_VIEWPORT, so it is not mistaken for an authored render template. */
  for (KX_GameObject *gameobj : GetObjectList()) {
    blender::Object *object = gameobj ? gameobj->GetBlenderObject() : nullptr;
    if (gameobj != overlay_cam && object != nullptr &&
        (object->visibility_flag & OB_HIDE_RENDER) != 0 &&
        BKE_collection_has_object_recursive(collection, object) &&
        !OverlayCollectionsContainObject(previous_collections, object))
    {
      m_overlayCollectionShownObjects[collection].insert(gameobj);
      gameobj->SetOverlayVisible(true);
      object->visibility_flag &= ~OB_HIDE_RENDER;
      AppendToIdsToUpdate(&object->id, ID_RECALC_SYNC_TO_EVAL, true);
    }
  }

  bool replicas_created = false;
  /* Each root and the overlay camera are copied on their own; object properties may refer
   * across them. */
  std::map<SCA_IObject *, SCA_IObject *> overlay_copies;
  std::vector<KX_GameObject *> overlay_members;
  if (m_overlayCamera != m_overlayCameraSource) {
    overlay_copies[m_overlayCameraSource] = m_overlayCamera;
    if (created_overlay_camera) {
      overlay_members.push_back(m_overlayCamera);
    }
  }
  const bool instantiation_failed = !ReplicateOverlayTemplates(*bmain,
                                                              *blender_scene,
                                                              collection,
                                                              overlay_template_objects,
                                                              0,
                                                              overlay_copies,
                                                              overlay_members,
                                                              replicas_created);

  if (!instantiation_failed) {
    for (KX_GameObject *member : overlay_members) {
      member->RelinkObjectProperties(overlay_copies);
    }
  }
  if (instantiation_failed) {
    RemoveOverlayCollection(collection, false, false);
    if (report_failure) {
      CM_Warning(
          KX_OverlayCollectionResultMessage(KX_OverlayCollectionResult::ObjectInstantiationFailed));
    }
    return KX_OverlayCollectionResult::ObjectInstantiationFailed;
  }
  if (created_overlay_camera || replicas_created) {
    /* ReplicateBlenderObject already schedules a relations rebuild. Keep collection bases current
     * immediately as well so input queries and the first overlay render see the staged objects in
     * the same frame as the Set Overlay Collection request. */
    TagForCollectionRemap();
    BKE_view_layer_synced_ensure(
        *bmain, blender_scene, BKE_view_layer_default_view(blender_scene));
  }
  RebuildOverlayRuntimeObjectIndex();
  RebuildRuntimeCollectionRenderVisibility();
  m_overlayResources.Create(collection);
  m_overlayResources.SetEstablishedCamera(m_overlayCameraSource);
  m_overlayResources.SetIndependentEnabled(collection, true);
  ApplyOverlayResourceVisibility(collection);
  return KX_OverlayCollectionResult::Success;
}

bool KX_Scene::ReplicateOverlayTemplates(
    blender::Main &bmain,
    blender::Scene &blender_scene,
    blender::Collection *collection,
    const std::vector<KX_GameObject *> &templates,
    const int32_t instance,
    std::map<SCA_IObject *, SCA_IObject *> &r_copies,
    std::vector<KX_GameObject *> &r_members,
    bool &r_created)
{
  const std::set<KX_GameObject *> template_set(templates.begin(), templates.end());
  /* An authored object copied twice in one copy (collection instances) is found as its first. */
  std::set<std::pair<const blender::Object *, int32_t>> indexed;
  for (KX_GameObject *gameobj : templates) {
    if (template_set.find(gameobj->GetParent()) != template_set.end()) {
      continue;
    }

    std::vector<KX_GameObject *> created_members;
    std::unordered_map<KX_GameObject *, KX_GameObject *> created_mapping;
    /* Restrict hierarchy replication to collection members. Without this filter, an inactive
     * child outside the overlay collection could become visible in the normal render pass. Use
     * the source as the placement reference so a selected child whose parent is outside the
     * collection retains its authored world transform as a new replica root. */
    KX_GameObject *replica = AddReplicaObject(gameobj,
                                              gameobj,
                                              0.0f,
                                              &created_members,
                                              &template_set,
                                              &created_mapping);
    blender::Object *replica_object = replica ? replica->GetBlenderObject() : nullptr;
    if (replica_object == nullptr) {
      if (replica != nullptr) {
        DelayedRemoveObject(replica);
        replica->Release();
      }
      return false;
    }

    /* AddReplicaObject preserves the reference position and orientation, but its legacy scale
     * placement multiplies by the reference hierarchy's root scale. That is sufficient for the
     * normal Add Object contract, yet it can omit intermediate parent scales when an overlay
     * member is detached from a parent outside the selected collection. The overlay replica is a
     * new root, so explicitly restore the source's complete authored world scale and resync the
     * generated hierarchy before it can be rendered or queried. */
    replica->NodeSetWorldScale(gameobj->NodeGetWorldScaling());
    replica->GetSGNode()->UpdateWorldData(0.0);
    rebase_physics_interpolation_hierarchy(replica->GetSGNode());
    for (KX_GameObject *created_member : created_members) {
      PHY_IPhysicsController *controller = created_member ?
                                               created_member->GetPhysicsController() :
                                               nullptr;
      if (controller != nullptr) {
        controller->SetTransform();
        controller->RebaseKinematicMotionState();
      }
    }
    for (KX_GameObject *created_member : created_members) {
      blender::Object *object = created_member != nullptr ? created_member->GetBlenderObject() :
                                                            nullptr;
      if (object != nullptr) {
        /* The authored object may be a hidden template. Visibility is instance-local here: only
         * the temporary runtime copy becomes visible in the overlay pass. A copy of an object set
         * to Disable in Renders starts hidden, as that object would in the scene (a tooltip or
         * popup that logic shows later). */
        created_member->SetVisible((object->visibility_flag & OB_HIDE_RENDER) == 0, false);
      }
    }
    r_created = true;
    r_copies.insert(created_mapping.begin(), created_mapping.end());
    r_members.insert(r_members.end(), created_members.begin(), created_members.end());
    const bool registered = RegisterOverlayReplica(
        bmain, blender_scene, collection, replica, created_members, created_mapping, instance);
    for (KX_GameObject *member : created_members) {
      const auto source = m_overlayMemberSources.find(member);
      if (source != m_overlayMemberSources.end() && source->second != nullptr &&
          indexed.insert({source->second, instance}).second)
      {
        m_overlayCopyOfSource[{source->second, instance}] = member;
      }
    }
    // release here because AddReplicaObject AddRef's
    // the object is added to the scene so we don't want python to own a reference
    replica->Release();
    if (!registered) {
      return false;
    }
  }
  return true;
}

bool KX_Scene::RegisterOverlayReplica(
    blender::Main &bmain,
    blender::Scene &blender_scene,
    blender::Collection *collection,
    KX_GameObject *replica,
    const std::vector<KX_GameObject *> &members,
    const std::unordered_map<KX_GameObject *, KX_GameObject *> &mapping,
    const int32_t instance)
{
  for (const auto &[source, copy] : mapping) {
    if (source != nullptr && copy != nullptr) {
      /* A copy of a copy keeps the authored object it came from. */
      const auto authored = m_overlayMemberSources.find(source);
      m_overlayMemberSources[copy] = authored != m_overlayMemberSources.end() ?
                                         authored->second :
                                         source->GetBlenderObject();
    }
  }
  for (KX_GameObject *member : members) {
    blender::Object *object = member != nullptr ? member->GetBlenderObject() : nullptr;
    if (object != nullptr) {
      if ((object->gameflag & OB_DUPLI_UPBGE) == 0) {
        object->visibility_flag &= ~OB_HIDE_RENDER;
      }
      object->gameflag |= OB_OVERLAY_COLLECTION;
      m_overlayCollectionMembers[collection].insert(member);
      if (instance != 0) {
        m_overlayMemberInstance[member] = instance;
      }
    }
  }
  /* A root is removed when the overlay closes; its children go with it. */
  m_overlayCollectionReplicas[collection].insert(replica);
  return StageOverlayReplicaRenderObjects(bmain, blender_scene, members);
}

blender::Collection *KX_Scene::GetOverlayCollectionOf(KX_GameObject *object) const
{
  if (object == nullptr) {
    return nullptr;
  }
  for (const auto &[collection, members] : m_overlayCollectionMembers) {
    if (members.contains(object)) {
      return collection;
    }
  }
  /* A hidden template of an open overlay. */
  blender::Object *authored = object->GetBlenderObject();
  for (blender::Collection *collection : m_overlay_collections) {
    if (authored != nullptr && m_overlayResources.Has(collection) &&
        BKE_collection_has_object_recursive(collection, authored))
    {
      return collection;
    }
  }
  return nullptr;
}

void KX_Scene::AdoptOverlayReplica(
    KX_GameObject *replica,
    const std::vector<KX_GameObject *> &members,
    const std::unordered_map<KX_GameObject *, KX_GameObject *> &mapping,
    const KX_GameObject *adder)
{
  KX_GameObject *source = nullptr;
  for (const auto &[original, copy] : mapping) {
    if (copy == replica) {
      source = original;
    }
  }
  blender::Collection *collection = GetOverlayCollectionOf(source);
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  blender::bContext *context = engine ? engine->GetContext() : nullptr;
  blender::Main *bmain = context ? CTX_data_main(context) : nullptr;
  blender::Scene *blender_scene = GetBlenderScene();
  if (collection == nullptr || replica == nullptr || bmain == nullptr || blender_scene == nullptr)
  {
    return;
  }
  /* A copy made in a player's copy of the menu (a grid slot) belongs to that copy. A hidden
   * template is in no copy: its copy goes to the copy of the object that added it. */
  int32_t instance = GetOverlayInstanceOf(source);
  KX_GameObject *adder_object = const_cast<KX_GameObject *>(adder);
  if (!m_overlayMemberSources.contains(source) && adder_object != nullptr &&
      m_overlayMemberSources.contains(adder_object) &&
      GetOverlayCollectionOf(adder_object) == collection)
  {
    instance = std::max(GetOverlayInstanceOf(adder_object), 0);
  }
  RegisterOverlayReplica(
      *bmain, *blender_scene, collection, replica, members, mapping, instance);
  TagForCollectionRemap();
  BKE_view_layer_synced_ensure(*bmain, blender_scene, BKE_view_layer_default_view(blender_scene));
  RebuildOverlayRuntimeObjectIndex();
  ApplyOverlayResourceVisibility(collection);
  NoteOverlayMembershipChanged(collection);
}

/** The copy number of an overlay object whose copy was removed, until it is deleted. */
static constexpr int32_t k_removed_overlay_instance = -2;

int32_t KX_Scene::GetOverlayInstanceOf(const KX_GameObject *object) const
{
  const auto instance = m_overlayMemberInstance.find(const_cast<KX_GameObject *>(object));
  return instance != m_overlayMemberInstance.end() ? instance->second : 0;
}

void KX_Scene::ForgetOverlayCopy(KX_GameObject *member)
{
  const auto source = m_overlayMemberSources.find(member);
  if (source == m_overlayMemberSources.end()) {
    return;
  }
  const auto copy = m_overlayCopyOfSource.find({source->second, GetOverlayInstanceOf(member)});
  if (copy != m_overlayCopyOfSource.end() && copy->second == member) {
    m_overlayCopyOfSource.erase(copy);
  }
}

KX_GameObject *KX_Scene::OverlayCounterpart(KX_GameObject *object,
                                            const KX_GameObject *viewer) const
{
  /* Nothing to choose until an overlay has a second copy. */
  if (m_overlayMemberInstance.empty() || object == nullptr || viewer == nullptr ||
      object == viewer)
  {
    return object;
  }
  KX_GameObject *viewer_object = const_cast<KX_GameObject *>(viewer);
  const int32_t instance = GetOverlayInstanceOf(viewer);
  if (instance < 0 || instance == GetOverlayInstanceOf(object) ||
      !m_overlayMemberSources.contains(viewer_object))
  {
    return object;
  }
  const auto source = m_overlayMemberSources.find(object);
  /* Copy numbers count within one overlay: the viewer's overlay must be the object's. */
  if (source == m_overlayMemberSources.end() ||
      GetOverlayCollectionOf(object) != GetOverlayCollectionOf(viewer_object))
  {
    return object;
  }
  /* None when the viewer's copy lost its own (removed by logic): never another player's. */
  const auto copy = m_overlayCopyOfSource.find({source->second, instance});
  return copy != m_overlayCopyOfSource.end() ? copy->second : nullptr;
}

void KX_Scene::SetOverlayInstanceLease(blender::Collection *collection,
                                       const int32_t instance,
                                       const int32_t menu_handle)
{
  if (menu_handle == 0) {
    m_overlayInstanceLease.erase({collection, instance});
  }
  else {
    m_overlayInstanceLease[{collection, instance}] = menu_handle;
  }
  ApplyOverlayResourceVisibility(collection);
}

int32_t KX_Scene::AddOverlayInstance(blender::Collection *collection, const int32_t menu_handle)
{
  if (std::find(m_overlay_collections.begin(), m_overlay_collections.end(), collection) ==
          m_overlay_collections.end() ||
      !m_overlayResources.Has(collection))
  {
    return -1;
  }
  /* Objects shown from Disable in Renders are the scene's own objects (their logic and physics
   * run once): there is nothing to copy them from. */
  if (const auto shown = m_overlayCollectionShownObjects.find(collection);
      shown != m_overlayCollectionShownObjects.end() && !shown->second.empty())
  {
    if (m_overlayCopyRefusedWarned.insert(collection).second) {
      CM_Warning("Menu collection '"
                 << (collection->id.name + 2)
                 << "' cannot open for a second player: its objects are active scene objects "
                    "(Disable in Renders). Hide the collection instead (exclude it, or hide it in "
                    "viewports) so each player gets a copy of their own.");
    }
    return -1;
  }
  const int32_t instance = m_nextOverlayInstance++;
  if (!CopyOverlayTemplates(collection, instance)) {
    return -1;
  }
  m_overlayInstanceLease[{collection, instance}] = menu_handle;
  ApplyOverlayResourceVisibility(collection);
  return instance;
}

bool KX_Scene::CopyOverlayTemplates(blender::Collection *collection, const int32_t instance)
{
  const auto position = std::find(
      m_overlay_collections.begin(), m_overlay_collections.end(), collection);
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  blender::bContext *context = engine ? engine->GetContext() : nullptr;
  blender::Main *bmain = context ? CTX_data_main(context) : nullptr;
  blender::Scene *blender_scene = GetBlenderScene();
  if (position == m_overlay_collections.end() || bmain == nullptr || blender_scene == nullptr) {
    return false;
  }
  /* The same templates the overlay copied when it opened: hidden objects of the collection, not
   * those of an overlay opened before it, nor the menu camera. */
  const std::vector<blender::Collection *> earlier(m_overlay_collections.begin(), position);
  std::vector<KX_GameObject *> templates;
  for (KX_GameObject *gameobj : GetInactiveList()) {
    blender::Object *object = gameobj ? gameobj->GetBlenderObject() : nullptr;
    if (gameobj != m_overlayCameraSource && object != nullptr &&
        BKE_collection_has_object_recursive(collection, object) &&
        !OverlayCollectionsContainObject(earlier, object) &&
        std::find(templates.begin(), templates.end(), gameobj) == templates.end())
    {
      templates.push_back(gameobj);
    }
  }
  std::map<SCA_IObject *, SCA_IObject *> copies;
  std::vector<KX_GameObject *> members;
  if (m_overlayCamera != nullptr && m_overlayCamera != m_overlayCameraSource) {
    copies[m_overlayCameraSource] = m_overlayCamera;
  }
  bool created = false;
  const bool replicated = ReplicateOverlayTemplates(
      *bmain, *blender_scene, collection, templates, instance, copies, members, created);
  if (replicated) {
    for (KX_GameObject *member : members) {
      member->RelinkObjectProperties(copies);
    }
  }
  if (created) {
    TagForCollectionRemap();
    BKE_view_layer_synced_ensure(
        *bmain, blender_scene, BKE_view_layer_default_view(blender_scene));
  }
  if (!replicated) {
    RemoveOverlayInstance(collection, instance);
    return false;
  }
  RebuildOverlayRuntimeObjectIndex();
  NoteOverlayMembershipChanged(collection);
  return true;
}

void KX_Scene::RemoveOverlayInstance(blender::Collection *collection, const int32_t instance)
{
  m_overlayInstanceLease.erase({collection, instance});
  if (instance == 0) {
    m_overlayCopyZeroRemoved.insert(collection);
  }
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  blender::bContext *context = engine ? engine->GetContext() : nullptr;
  blender::Main *bmain = context ? CTX_data_main(context) : nullptr;
  /* This copy's members and roots, found before any is marked removed. */
  std::vector<KX_GameObject *> members;
  if (const auto found = m_overlayCollectionMembers.find(collection);
      found != m_overlayCollectionMembers.end())
  {
    for (KX_GameObject *member : found->second) {
      if (member != nullptr && GetOverlayInstanceOf(member) == instance) {
        members.push_back(member);
      }
    }
    for (KX_GameObject *member : members) {
      found->second.erase(member);
    }
  }
  std::vector<KX_GameObject *> roots;
  if (const auto found = m_overlayCollectionReplicas.find(collection);
      found != m_overlayCollectionReplicas.end())
  {
    for (KX_GameObject *root : found->second) {
      if (root != nullptr && GetOverlayInstanceOf(root) == instance) {
        roots.push_back(root);
      }
    }
    for (KX_GameObject *root : roots) {
      found->second.erase(root);
    }
  }
  /* Until they are deleted at the end of the frame (NewRemoveObject forgets them), they are in
   * no copy, so they never count as another player's meanwhile. */
  for (KX_GameObject *member : members) {
    ForgetOverlayCopy(member);
    m_overlayMemberSources.erase(member);
    m_overlayMemberInstance[member] = k_removed_overlay_instance;
  }
  /* Like RemoveOverlayCollection: the roots go, their children with them. */
  for (KX_GameObject *root : roots) {
    m_overlayMemberInstance[root] = k_removed_overlay_instance;
    if (bmain != nullptr && root->IsReplica() && root->GetBlenderObject() != nullptr) {
      BKE_collection_object_remove(bmain, collection, root->GetBlenderObject(), false);
    }
    DelayedRemoveObject(root);
  }
  RebuildOverlayRuntimeObjectIndex();
  NoteOverlayMembershipChanged(collection);
}

KX_OverlayCollectionResult KX_Scene::ValidateRemoveOverlayCollection(
    blender::Collection *collection)
{
  if (collection == nullptr) {
    return KX_OverlayCollectionResult::MissingCollection;
  }
  const KX_OverlayCollectionResult independent = m_overlayResources.ValidateIndependentDisable(
      collection);
  if (independent != KX_OverlayCollectionResult::NoChange) {
    return independent;
  }
  if (std::find(m_overlay_collections.begin(), m_overlay_collections.end(), collection) !=
      m_overlay_collections.end())
  {
    /* Instantiated without an independent request (menu lease or a failed instantiate rollback).
     * Destruction is decided by RemoveOverlayCollection after lease/independent accounting. */
    return KX_OverlayCollectionResult::Success;
  }
  if (!OverlayCollectionBelongsToScene(*this, collection)) {
    return KX_OverlayCollectionResult::CollectionWrongScene;
  }
  return KX_OverlayCollectionResult::NoChange;
}

KX_OverlayCollectionResult KX_Scene::RemoveOverlayCollection(
    blender::Collection *collection,
    const bool report_failure,
    const bool notify_logic_nodes)
{
  const KX_OverlayCollectionResult validation = ValidateRemoveOverlayCollection(collection);
  if (validation == KX_OverlayCollectionResult::NoChange) {
#ifdef WITH_GAMEENGINE_LOGICNODES
    /* Also heal a stale menu lease if an external caller repeats removal after another runtime
     * system already removed the overlay. */
    if (notify_logic_nodes && m_logicNodeManager) {
      m_logicNodeManager->GetUIService().NotifyOverlayCollectionRemoved(collection);
    }
#endif
    return validation;
  }
  if (!KX_OverlayCollectionResultSucceeded(validation)) {
    if (report_failure) {
      CM_Warning(KX_OverlayCollectionResultMessage(validation));
    }
    return validation;
  }
  const bool had_independent = m_overlayResources.IndependentEnabled(collection);
  if (had_independent) {
    m_overlayResources.SetIndependentEnabled(collection, false);
  }
  if (m_overlayResources.ShouldExist(collection)) {
    /* Players' menus keep it open. A copy 0 that no menu shows any more was only the overlay's
     * own: it goes now. */
    if (had_independent && OverlayHasCopyZero(collection) &&
        !m_overlayInstanceLease.contains({collection, 0}))
    {
      RemoveOverlayInstance(collection, 0);
    }
    ApplyOverlayResourceVisibility(collection);
    return had_independent ? KX_OverlayCollectionResult::Success :
                             KX_OverlayCollectionResult::NoChange;
  }
  m_overlayResources.Erase(collection);
  m_overlayCopyZeroRemoved.erase(collection);
  m_overlayCopyRefusedWarned.erase(collection);
  /* Check for already removed collections */
  const auto collection_iter = std::find(
      m_overlay_collections.begin(), m_overlay_collections.end(), collection);
  if (collection_iter != m_overlay_collections.end()) {
    /* Objects that stay in the scene (plain visible ones) must not stay hidden by a menu that
     * covered this overlay. */
    std::vector<KX_GameObject *> released_objects;
    AppendOverlayRuntimeObjectsInCollection(collection, released_objects);
    std::unordered_set<blender::Object *> affected_objects;
    FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (collection, collection_object) {
      if (collection_object != nullptr) {
        affected_objects.insert(collection_object);
      }
    }
    FOREACH_COLLECTION_OBJECT_RECURSIVE_END;

    std::unordered_set<KX_GameObject *> removed_members;
    const auto members_iter = m_overlayCollectionMembers.find(collection);
    if (members_iter != m_overlayCollectionMembers.end()) {
      removed_members = members_iter->second;
      for (KX_GameObject *game_object : members_iter->second) {
        if (game_object != nullptr && game_object->GetBlenderObject() != nullptr) {
          affected_objects.insert(game_object->GetBlenderObject());
        }
      }
      m_overlayCollectionMembers.erase(members_iter);
    }
    std::unordered_set<KX_GameObject *> removed_shown_objects;
    const auto shown_iter = m_overlayCollectionShownObjects.find(collection);
    if (shown_iter != m_overlayCollectionShownObjects.end()) {
      removed_shown_objects = shown_iter->second;
      m_overlayCollectionShownObjects.erase(shown_iter);
    }

    m_overlay_collections.erase(collection_iter);
    if (m_overlay_collections.empty()) {
      /* No overlay pass will consume these entries. Some can refer to temporary Blender objects
       * owned by the replicas removed below, so retaining them would create dangling ID pointers
       * if another overlay were activated later. */
      m_idsToUpdateInOverlayPass.clear();
      DestroyOverlayCameraRuntimeInstance(true);
    }

    /* Remove only replicas created by AddOverlayCollection. An unrelated gameplay replica may
     * also belong to this collection and must never be destroyed as a side effect. */
    std::unordered_set<KX_GameObject *> removed_roots;
    const auto replicas_iter = m_overlayCollectionReplicas.find(collection);
    if (replicas_iter != m_overlayCollectionReplicas.end()) {
      removed_roots = replicas_iter->second;
      m_overlayCollectionReplicas.erase(replicas_iter);
      for (KX_GameObject *replica : removed_roots) {
        if (replica == nullptr) {
          continue;
        }
        const bool still_owned = std::any_of(
            m_overlayCollectionReplicas.begin(),
            m_overlayCollectionReplicas.end(),
            [replica](const auto &item) { return item.second.contains(replica); });
        if (still_owned) {
          continue;
        }
        if (replica->IsReplica() && replica->GetBlenderObject() != nullptr) {
          blender::bContext *C = KX_GetActiveEngine()->GetContext();
          blender::Main *bmain = CTX_data_main(C);
          BKE_collection_object_remove(bmain, collection, replica->GetBlenderObject(), false);
        }
        DelayedRemoveObject(replica);
      }
    }

    for (KX_GameObject *member : removed_members) {
      const bool still_owned = std::any_of(
          m_overlayCollectionMembers.begin(),
          m_overlayCollectionMembers.end(),
          [member](const auto &item) { return item.second.contains(member); });
      if (!still_owned) {
        ForgetOverlayCopy(member);
        m_overlayMemberSources.erase(member);
        m_overlayMemberInstance.erase(member);
      }
    }
    std::erase_if(m_overlayInstanceLease,
                  [collection](const auto &item) { return item.first.first == collection; });
    for (KX_GameObject *game_object : removed_shown_objects) {
      if (game_object == nullptr) {
        continue;
      }
      const bool still_owned = std::any_of(
          m_overlayCollectionShownObjects.begin(),
          m_overlayCollectionShownObjects.end(),
          [game_object](const auto &item) { return item.second.contains(game_object); });
      if (still_owned) {
        continue;
      }
      game_object->SetOverlayVisible(false);
      game_object->SetPresentationVisible(true);
      blender::Object *object = game_object->GetBlenderObject();
      if (object != nullptr) {
        /* This set contains only objects whose authored Disable in Renders flag was removed by
         * AddOverlayCollection, so restore it after the final overlay owner releases the object. */
        object->visibility_flag |= OB_HIDE_RENDER;
      }
      if (object != nullptr) {
        AppendToIdsToUpdate(&object->id, ID_RECALC_SYNC_TO_EVAL, false);
      }
    }

    RebuildOverlayRuntimeObjectIndex();
    RebuildRuntimeCollectionRenderVisibility();
    for (KX_GameObject *game_object : released_objects) {
      game_object->SetPresentationVisible(true);
    }
    ApplyAllOverlayResourceVisibility();

    for (blender::Object *affected_object : affected_objects) {
      const bool was_overlay = (affected_object->gameflag & OB_OVERLAY_COLLECTION) != 0;
      const bool remains_overlay = AnyRuntimeOverlayContainsObject(affected_object);
      if (remains_overlay) {
        affected_object->gameflag |= OB_OVERLAY_COLLECTION;
      }
      else {
        affected_object->gameflag &= ~OB_OVERLAY_COLLECTION;
      }
      if (was_overlay != remains_overlay) {
        AppendToIdsToUpdate(
            &affected_object->id, ID_RECALC_SYNC_TO_EVAL, remains_overlay);
      }
    }
  }
#ifdef WITH_GAMEENGINE_LOGICNODES
  if (notify_logic_nodes && m_logicNodeManager) {
    m_logicNodeManager->GetUIService().NotifyOverlayCollectionRemoved(collection);
  }
#endif
  return KX_OverlayCollectionResult::Success;
}

void KX_Scene::AppendOverlayCollectionRuntimeObjects(blender::Collection *collection,
                                                     std::vector<KX_GameObject *> &r_objects,
                                                     const int32_t instance) const
{
  if (collection == nullptr) {
    return;
  }
  const auto in_instance = [&](const KX_GameObject *object) {
    return instance < 0 || GetOverlayInstanceOf(object) == instance;
  };

  const auto members_iter = m_overlayCollectionMembers.find(collection);
  if (members_iter != m_overlayCollectionMembers.end()) {
    for (KX_GameObject *member : members_iter->second) {
      if (in_instance(member)) {
        r_objects.push_back(member);
      }
    }
  }

  /* Roots are normally also present in the complete member set. Retain the explicit root lookup
   * so a partially instantiated hierarchy remains pause-processable and removable. The manager
   * deduplicates the resulting object list. */
  const auto roots_iter = m_overlayCollectionReplicas.find(collection);
  if (roots_iter != m_overlayCollectionReplicas.end()) {
    for (KX_GameObject *root : roots_iter->second) {
      if (in_instance(root)) {
        r_objects.push_back(root);
      }
    }
  }

  /* A child add is idempotent while a recursive parent overlay is active, so it may not own an
   * exact map entry. Source identity keeps collection-scoped pause processing correct. */
  for (const auto &member_and_source : m_overlayMemberSources) {
    if (member_and_source.first != nullptr && member_and_source.second != nullptr &&
        m_overlayRuntimeObjects.contains(member_and_source.first) &&
        in_instance(member_and_source.first) &&
        BKE_collection_has_object_recursive(collection, member_and_source.second))
    {
      r_objects.push_back(member_and_source.first);
    }
  }
}

bool KX_Scene::RuntimeObjectBelongsToOverlayCollection(KX_GameObject *game_object,
                                                       const blender::Collection *collection,
                                                       const int32_t instance) const
{
  if (game_object == nullptr || collection == nullptr ||
      !m_overlayRuntimeObjects.contains(const_cast<KX_GameObject *>(game_object)) ||
      (instance >= 0 && GetOverlayInstanceOf(game_object) != instance))
  {
    return false;
  }

  if (const auto source = m_overlayMemberSources.find(
          const_cast<KX_GameObject *>(game_object));
      source != m_overlayMemberSources.end())
  {
    return source->second != nullptr &&
           BKE_collection_has_object_recursive(const_cast<blender::Collection *>(collection),
                                               source->second);
  }

  blender::Object *source = game_object->GetBlenderObject();
  return source != nullptr &&
         BKE_collection_has_object_recursive(const_cast<blender::Collection *>(collection),
                                             source);
}

void KX_Scene::AppendOverlayRuntimeObjectsInCollection(blender::Collection *collection,
                                                       std::vector<KX_GameObject *> &r_objects,
                                                       const int32_t instance) const
{
  if (collection == nullptr || m_overlayRuntimeObjects.empty()) {
    return;
  }
  std::unordered_set<const blender::Object *> sources;
  FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (collection, collection_object) {
    sources.insert(collection_object);
  }
  FOREACH_COLLECTION_OBJECT_RECURSIVE_END;
  for (KX_GameObject *game_object : m_overlayRuntimeObjects) {
    const auto member_source = m_overlayMemberSources.find(game_object);
    const blender::Object *source = member_source != m_overlayMemberSources.end() ?
                                        member_source->second :
                                        game_object->GetBlenderObject();
    if (source != nullptr && sources.contains(source) &&
        (instance < 0 || GetOverlayInstanceOf(game_object) == instance))
    {
      r_objects.push_back(game_object);
    }
  }
}

/** The scene's own settings for the evaluated scene's effect switches (each pass starts from
 * them, and they are given back after the draw). */
static void restore_camera_effects(blender::Scene &scene_eval, const blender::Scene &scene)
{
  scene_eval.eevee.flag = scene.eevee.flag;
  scene_eval.r.mode = (scene_eval.r.mode & ~R_MBLUR) | (scene.r.mode & R_MBLUR);
  /* The view's shadow pool and Workbench settings. Field by field: View3DShading also holds an
   * IDProperty pointer that each scene copy owns. */
  scene_eval.eevee.shadow_pool_size = scene.eevee.shadow_pool_size;
  scene_eval.display.shading.light = scene.display.shading.light;
  scene_eval.display.shading.color_type = scene.display.shading.color_type;
  scene_eval.display.shading.flag = scene.display.shading.flag;
  scene_eval.display.shading.cavity_type = scene.display.shading.cavity_type;
  scene_eval.display.shading.curvature_ridge_factor = scene.display.shading.curvature_ridge_factor;
  scene_eval.display.shading.curvature_valley_factor =
      scene.display.shading.curvature_valley_factor;
  scene_eval.display.shading.background_type = scene.display.shading.background_type;
  scene_eval.display.viewport_aa = scene.display.viewport_aa;
  scene_eval.r.alphamode = scene.r.alphamode;
}

static bool camera_matches(const KX_Camera *rule_camera,
                           const KX_Camera *camera,
                           const KX_Camera *overlay_camera,
                           const KX_Camera *overlay_source);

/** The camera's Blender camera data (the default camera has no Blender object). */
static const blender::Camera *camera_data(KX_Camera *kxcam)
{
  blender::Object *obcam = kxcam ? kxcam->GetBlenderObject() : nullptr;
  return (obcam && obcam->type == OB_CAMERA) ? id_cast<blender::Camera *>(obcam->data) : nullptr;
}

KX_CameraRenderMemory::PassSettings KX_Scene::CameraPassSettings(KX_Camera *kxcam,
                                                                 const bool isOverlayPass,
                                                                 const int width,
                                                                 const int height,
                                                                 const int64_t window_pixels)
{
  const blender::Scene *scene = GetBlenderScene();
  KX_CameraRenderMemory::PassSettings settings;
  settings.width = width;
  settings.height = height;
  settings.overlay = isOverlayPass;
  settings.eevee_flag = scene->eevee.flag;
  settings.motion_blur = (scene->r.mode & R_MBLUR) != 0;
  settings.shadow_pool = scene->eevee.shadow_pool_size;
  if (!kxcam) {
    return settings;
  }

  const blender::Camera *cam = camera_data(kxcam);
  /* Off when the camera turns it off, or in a player's view while the window is split, when the
   * scene's Split Screen Quality does. */
  const int camera_flag = cam ? cam->gameflag : 0;
  const bool split_view = GetPlayerViews().IsSplitView(kxcam, GetActiveCamera());
  const int split_flag = split_view ? scene->gm.split_flag : 0;
  const auto off = [&](const int camera_bit, const int split_bit) {
    return (camera_flag & camera_bit) || (split_flag & split_bit);
  };
  /* Without shadows a view's shadow pool shrinks to nothing. */
  if (off(GAME_CAM_DISABLE_SHADOWS, GAME_SPLIT_DISABLE_SHADOWS)) {
    settings.eevee_flag &= ~SCE_EEVEE_SHADOW_ENABLED;
  }
  if (off(GAME_CAM_DISABLE_RAY_TRACING, GAME_SPLIT_DISABLE_RAY_TRACING)) {
    settings.eevee_flag &= ~SCE_EEVEE_SSR_ENABLED;
  }
  if (off(GAME_CAM_DISABLE_VOLUMETRIC_SHADOWS, GAME_SPLIT_DISABLE_VOLUMETRIC_SHADOWS)) {
    settings.eevee_flag &= ~SCE_EEVEE_VOLUMETRIC_SHADOWS;
  }
  if (off(GAME_CAM_DISABLE_MOTION_BLUR, GAME_SPLIT_DISABLE_MOTION_BLUR)) {
    settings.motion_blur = false;
  }
  /* A camera's choice only: without it every frame restarts from the same samples (stable, sharp
   * text with Samples per Frame), as the game restarts EEVEE's accumulation each frame. */
  if (camera_flag & GAME_CAM_DISABLE_TEMPORAL_REPROJECTION) {
    settings.eevee_flag &= ~SCE_EEVEE_TAA_REPROJECTION;
  }
  /* A split screen player's view needs fewer shadow pages than the whole window
   * (KX_CameraRenderMemory::ShadowPoolForSplitView). Other views keep the scene's pool. */
  if ((settings.eevee_flag & SCE_EEVEE_SHADOW_ENABLED) && split_view) {
    settings.shadow_pool = KX_CameraRenderMemory::ShadowPoolForSplitView(
        scene->eevee.shadow_pool_size, int64_t(width) * int64_t(height), window_pixels);
  }

  settings.workbench = (camera_flag & GAME_CAM_USE_WORKBENCH) != 0;
  settings.workbench_flags = camera_flag &
                             (GAME_CAM_WORKBENCH_STUDIO_LIGHT | GAME_CAM_WORKBENCH_MATCAP |
                              GAME_CAM_WORKBENCH_NO_SMOOTH_EDGES |
                              GAME_CAM_WORKBENCH_CAVITY_SCREEN | GAME_CAM_WORKBENCH_CAVITY_WORLD |
                              GAME_CAM_WORKBENCH_OUTLINE | GAME_CAM_WORKBENCH_COLOR_MATERIAL |
                              GAME_CAM_WORKBENCH_COLOR_OBJECT | GAME_CAM_WORKBENCH_COLOR_MORE);
  settings.perspective = kxcam->GetCameraData()->m_perspective;

  settings.temporal_reprojection = (settings.eevee_flag & SCE_EEVEE_TAA_REPROJECTION) != 0;
  settings.ray_tracing = (settings.eevee_flag & SCE_EEVEE_SSR_ENABLED) != 0;
  settings.shadows = (settings.eevee_flag & SCE_EEVEE_SHADOW_ENABLED) != 0;
  settings.depth_of_field = cam && (cam->dof.flag & CAM_DOF_ENABLED) != 0;
  settings.hides_objects = (cam && cam->game_collection) ||
                           std::any_of(m_cameraHiddenObjects.begin(),
                                       m_cameraHiddenObjects.end(),
                                       [&](const auto &hidden) {
                                         return camera_matches(hidden.camera,
                                                               kxcam,
                                                               m_overlayCamera,
                                                               m_overlayCameraSource);
                                       });
  settings.image_render = kxcam->IsUsedForImageRender();
  return settings;
}

bool KX_Scene::DisableCameraEffects(blender::Depsgraph *depsgraph,
                                    KX_Camera *kxcam,
                                    bool isOverlayPass,
                                    const KX_CameraRenderMemory::PassSettings &settings)
{
  if (!kxcam) {
    return false;
  }
  blender::Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);
  restore_camera_effects(*scene_eval, *GetBlenderScene());

  /* Every camera draws through its own renderer (or one shared with passes of equal settings),
   * which reads these at each draw: the other views keep theirs. */
  scene_eval->eevee.flag = blender::eSceneEEVEE_Flag(settings.eevee_flag);
  if (!settings.motion_blur) {
    scene_eval->r.mode &= ~R_MBLUR;
  }
  scene_eval->eevee.shadow_pool_size = settings.shadow_pool;

  if (settings.workbench) {
    /* Use Workbench: the game draws with OB_RENDER or OB_MATERIAL shading, so Workbench reads the
     * scene's Workbench settings (scene.display). The camera picks the lighting, colors, cavity
     * and outline; their strengths, colors, studio light and MatCap stay the scene's. Never
     * X-ray, shadows or depth of field (costly, or history). */
    const int workbench_flags = settings.workbench_flags;
    View3DShading &shading = scene_eval->display.shading;
    shading.light = (workbench_flags & GAME_CAM_WORKBENCH_MATCAP)       ? V3D_LIGHTING_MATCAP :
                    (workbench_flags & GAME_CAM_WORKBENCH_STUDIO_LIGHT) ? V3D_LIGHTING_STUDIO :
                                                                          V3D_LIGHTING_FLAT;
    switch (workbench_flags & (GAME_CAM_WORKBENCH_COLOR_MATERIAL | GAME_CAM_WORKBENCH_COLOR_OBJECT |
                               GAME_CAM_WORKBENCH_COLOR_MORE))
    {
      case GAME_CAM_WORKBENCH_COLOR_MATERIAL:
        shading.color_type = V3D_SHADING_MATERIAL_COLOR;
        break;
      case GAME_CAM_WORKBENCH_COLOR_OBJECT:
        shading.color_type = V3D_SHADING_OBJECT_COLOR;
        break;
      case GAME_CAM_WORKBENCH_COLOR_MATERIAL | GAME_CAM_WORKBENCH_COLOR_OBJECT:
        shading.color_type = V3D_SHADING_SINGLE_COLOR;
        break;
      case GAME_CAM_WORKBENCH_COLOR_MORE | GAME_CAM_WORKBENCH_COLOR_MATERIAL:
        shading.color_type = V3D_SHADING_VERTEX_COLOR;
        break;
      case GAME_CAM_WORKBENCH_COLOR_MORE | GAME_CAM_WORKBENCH_COLOR_OBJECT:
        shading.color_type = V3D_SHADING_RANDOM_COLOR;
        break;
      default:
        shading.color_type = V3D_SHADING_TEXTURE_COLOR;
        break;
    }
    shading.background_type = V3D_SHADING_BACKGROUND_WORLD;
    shading.flag &= ~(V3D_SHADING_OBJECT_OUTLINE | V3D_SHADING_XRAY | V3D_SHADING_SHADOW |
                      V3D_SHADING_CAVITY | V3D_SHADING_DEPTH_OF_FIELD);
    switch (workbench_flags & (GAME_CAM_WORKBENCH_CAVITY_SCREEN | GAME_CAM_WORKBENCH_CAVITY_WORLD))
    {
      case GAME_CAM_WORKBENCH_CAVITY_SCREEN:
        shading.flag |= V3D_SHADING_CAVITY;
        shading.cavity_type = V3D_SHADING_CAVITY_CURVATURE;
        break;
      case GAME_CAM_WORKBENCH_CAVITY_WORLD:
        shading.flag |= V3D_SHADING_CAVITY;
        shading.cavity_type = V3D_SHADING_CAVITY_SSAO;
        break;
      case GAME_CAM_WORKBENCH_CAVITY_SCREEN | GAME_CAM_WORKBENCH_CAVITY_WORLD:
        shading.flag |= V3D_SHADING_CAVITY;
        shading.cavity_type = V3D_SHADING_CAVITY_BOTH;
        break;
      default:
        break;
    }
    /* Files made from the default startup file have both Screen strengths at 0 (new scenes get
     * 1), which would show no Screen cavity at all: use 1 then. */
    if ((shading.flag & V3D_SHADING_CAVITY) && shading.cavity_type != V3D_SHADING_CAVITY_SSAO &&
        shading.curvature_ridge_factor == 0.0f && shading.curvature_valley_factor == 0.0f)
    {
      shading.curvature_ridge_factor = 1.0f;
      shading.curvature_valley_factor = 1.0f;
    }
    if (workbench_flags & GAME_CAM_WORKBENCH_OUTLINE) {
      shading.flag |= V3D_SHADING_OBJECT_OUTLINE;
    }
    /* Smoothing restarts every game frame (the game counts as playback), so one sample. */
    scene_eval->display.viewport_aa = (settings.workbench_flags &
                                       GAME_CAM_WORKBENCH_NO_SMOOTH_EDGES) ?
                                          SCE_DISPLAY_AA_OFF :
                                          SCE_DISPLAY_AA_FXAA;
    /* A menu/HUD pass keeps a see-through background, as with EEVEE (which skips the world
     * there); other views draw the world color behind their objects. */
    if (isOverlayPass) {
      scene_eval->r.alphamode = R_ALPHAPREMUL;
    }
  }

  blender::Object *obcam = kxcam->GetBlenderObject();
  if (isOverlayPass && GetOverlayCamera() && camera_data(kxcam)) {
    if ((m_backupOverlayFlag != scene_eval->eevee.flag)) {
      /* Only tag if overlay settings changed since previous frame */
      AppendToIdsToUpdate(&obcam->id, ID_RECALC_TRANSFORM, true);
    }
    m_backupOverlayFlag = scene_eval->eevee.flag;
  }

  const blender::Camera *cam = camera_data(kxcam);
  const int camera_flag = cam ? cam->gameflag : 0;
  const int split_flag = GetPlayerViews().IsSplitView(kxcam, GetActiveCamera()) ?
                             GetBlenderScene()->gm.split_flag :
                             0;
  /* Workbench draws once: its smoothing restarts every game frame. */
  return settings.workbench || (camera_flag & GAME_CAM_DISABLE_EXTRA_SAMPLES) ||
         (split_flag & GAME_SPLIT_DISABLE_EXTRA_SAMPLES);
}

void KX_Scene::SetCameraVisibility(KX_Camera *camera,
                                   KX_GameObject *object,
                                   const bool visible,
                                   const bool keep_shadow)
{
  const auto rule = std::find_if(
      m_cameraHiddenObjects.begin(), m_cameraHiddenObjects.end(), [&](const auto &hidden) {
        return hidden.camera == camera && hidden.object == object;
      });
  const bool was_hidden = rule != m_cameraHiddenObjects.end();
  const bool was_keep_shadow = was_hidden && rule->keep_shadow;
  const bool now_hidden = !visible && camera != nullptr && object != nullptr;
  if (object != nullptr &&
      (now_hidden != was_hidden || (now_hidden && keep_shadow != was_keep_shadow)))
  {
    NoteDrawChanged(object);
  }

  std::erase_if(m_cameraHiddenObjects, [&](const CameraHiddenObject &hidden) {
    return hidden.camera == camera && hidden.object == object;
  });
  if (now_hidden) {
    m_cameraHiddenObjects.push_back({camera, object, keep_shadow});
    KX_KetsjiEngine *engine = KX_GetActiveEngine();
    if (engine != nullptr && engine->UseViewportRender() && !m_viewportRenderVisibilityWarned) {
      m_viewportRenderVisibilityWarned = true;
      CM_Warning("Use Viewport Render draws every object in the game camera: Set Camera "
                 "Visibility hides nothing there. Turn Use Viewport Render off (Render tab > Game "
                 "Resolution) to see it.");
    }
  }
}

void KX_Scene::NoteDrawChanged(KX_GameObject *object)
{
  if (object->IsUpbgeDupliInstance()) {
    /* Instanced copies share their Blender object: its update stamp stands for all of them. */
    object->TagUpbgeDupliInstanceForTaaReset();
    return;
  }
  blender::Object *ob = object->GetBlenderObject();
  if (ob == nullptr) {
    return;
  }
  /* The cheapest update the renderers notice: the object's shading stamp, nothing evaluated
   * again (no modifiers, a fluid simulation would not start over). */
  AppendToIdsToUpdate(&ob->id, ID_RECALC_SHADING, ob->gameflag & OB_OVERLAY_COLLECTION);
}

/** The camera a rule names for this one: the menu camera's live copy draws for its source. */
static bool camera_matches(const KX_Camera *rule_camera,
                           const KX_Camera *camera,
                           const KX_Camera *overlay_camera,
                           const KX_Camera *overlay_source)
{
  return rule_camera == camera || (camera == overlay_camera && rule_camera == overlay_source);
}

bool KX_Scene::IsHiddenForCamera(const KX_GameObject *object, const KX_Camera *camera) const
{
  return std::any_of(
      m_cameraHiddenObjects.begin(), m_cameraHiddenObjects.end(), [&](const auto &hidden) {
        return hidden.object == object &&
               camera_matches(hidden.camera, camera, m_overlayCamera, m_overlayCameraSource);
      });
}

void KX_Scene::HideCameraHiddenObjects(blender::Depsgraph *depsgraph,
                                       KX_Camera *camera,
                                       const bool workbench,
                                       const bool is_overlay_pass)
{
  RestoreCameraHiddenObjects();
  m_cameraHiddenPass = camera;
  m_cameraHiddenPassWorkbench = workbench;
  PrepareCullPass(depsgraph, camera, workbench, is_overlay_pass);

  const auto hide = [&](KX_GameObject *game_object, const bool keep_shadow) {
    blender::Object *object = game_object->GetBlenderObject();
    blender::Object *object_eval = object ? DEG_get_evaluated(depsgraph, object) : nullptr;
    if (object_eval == nullptr || object_eval == object) {
      return;
    }
    /* Not drawn at all, or (Keep Shadow) drawn only into shadows and reflections: EEVEE's camera
     * ray visibility, which Workbench ignores. */
    m_cameraHiddenRestore.emplace_back(object_eval, int(object_eval->visibility_flag));
    object_eval->visibility_flag |= (keep_shadow && !workbench) ? OB_HIDE_CAMERA :
                                                                  OB_HIDE_VIEWPORT;
  };

  /* Hidden by Set Visible, from every camera. */
  for (KX_GameObject *game_object : m_drawHiddenObjects) {
    hide(game_object, false);
  }
  /* Parked for the next copy of their source (ParkCopy), and the Shared groups' objects (their
   * members are drawn as copies of them). */
  const auto hide_blender_object = [&](blender::Object *object) {
    blender::Object *object_eval = DEG_get_evaluated(depsgraph, object);
    if (object_eval != nullptr && object_eval != object) {
      m_cameraHiddenRestore.emplace_back(object_eval, int(object_eval->visibility_flag));
      object_eval->visibility_flag |= OB_HIDE_VIEWPORT;
    }
  };
  for (const ParkedCopy &parked : m_parkedCopies) {
    hide_blender_object(parked.object);
  }
  for (blender::Object *base : m_sharedDrawBases) {
    hide_blender_object(base);
  }

  for (const CameraHiddenObject &hidden : m_cameraHiddenObjects) {
    /* An instanced copy shares its Blender object: bge_dupli_provider hides its draw copy. */
    if (!camera_matches(hidden.camera, camera, m_overlayCamera, m_overlayCameraSource) ||
        hidden.object->IsUpbgeDupliInstance())
    {
      continue;
    }
    hide(hidden.object, hidden.keep_shadow);
  }

  /* Draw culling: the objects with their own Blender object (copies: IsHiddenInCameraPass). */
  if (m_cullPass.clip || m_cullPass.distances) {
    for (KX_GameObject *game_object : *GetObjectList()) {
      if (!game_object->IsUpbgeDupliInstance() && !game_object->IsMerged() &&
          game_object->GetVisible() && CulledInCameraPass(game_object))
      {
        hide(game_object, false);
      }
    }
  }

  /* The camera's Collection setting: Only draws the collection's objects, Hide all the others
   * (one bit test per object once their collections are known). */
  const blender::Camera *cam = camera_data(camera);
  m_cameraHiddenPassCollection = cam ? CameraCollectionIndex(cam->game_collection) : -1;
  m_cameraHiddenPassCollectionHide = cam && (cam->gameflag & GAME_CAM_COLLECTION_HIDE);
  if (m_cameraHiddenPassCollection < 0) {
    return;
  }
  for (KX_GameObject *game_object : *GetObjectList()) {
    /* Invisible objects aren't drawn anyway. */
    if (game_object->IsUpbgeDupliInstance() || !game_object->GetVisible() ||
        !HiddenByCameraCollection(
            game_object, m_cameraHiddenPassCollection, m_cameraHiddenPassCollectionHide))
    {
      continue;
    }
    hide(game_object, false);
  }
}

void KX_Scene::AddDrawHidden(KX_GameObject *object)
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  object->SetDrawHidden(true, engine != nullptr ? engine->GetRenderFrame() : 0);
  m_drawHiddenObjects.push_back(object);
}

void KX_Scene::RemoveDrawHidden(KX_GameObject *object)
{
  object->SetDrawHidden(false, 0);
  auto it = std::find(m_drawHiddenObjects.begin(), m_drawHiddenObjects.end(), object);
  if (it != m_drawHiddenObjects.end()) {
    *it = m_drawHiddenObjects.back();
    m_drawHiddenObjects.pop_back();
  }
}

void KX_Scene::RetireDrawHiddenObjects(const uint64_t frame)
{
  /* Looked at every second, objects hidden for two: a game that hides and shows the same objects
   * often rebuilds at most once a second, and a level whose hidden objects stay hidden pays them
   * nothing after a while. */
  constexpr uint64_t look_every = 60;
  constexpr uint64_t hidden_for = 120;
  if (m_drawHiddenObjects.empty() || frame < m_drawHiddenRetireFrame + look_every) {
    return;
  }
  m_drawHiddenRetireFrame = frame;
  std::vector<KX_GameObject *> retired;
  for (KX_GameObject *object : m_drawHiddenObjects) {
    if (object->GetDrawHiddenSince() + hidden_for <= frame) {
      retired.push_back(object);
    }
  }
  for (KX_GameObject *object : retired) {
    object->HideInGraph();
  }
}

bool KX_Scene::ParkCopy(blender::Object *object, const uint32_t source_uid)
{
  /* The scene going (its copies are deleted as before). */
  if (!m_isRuntime) {
    return false;
  }
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  blender::Depsgraph *depsgraph = CTX_data_depsgraph_pointer(engine->GetContext());
  /* Hidden at each camera pass in Blender's evaluated copy: outside the graph there is none. */
  if (depsgraph == nullptr || DEG_get_evaluated(depsgraph, object) == object) {
    return false;
  }
  m_parkedCopies.push_back({object, source_uid, engine->GetRenderFrame()});
  /* The renderers draw again without it. */
  AppendToIdsToUpdate(&object->id, ID_RECALC_SHADING, false);
  return true;
}

/** The slots hold the same materials (a source's material made its own since the copy). */
static bool same_materials(blender::Object *a, blender::Object *b)
{
  if (a->totcol != b->totcol) {
    return false;
  }
  for (short slot = 1; slot <= a->totcol; ++slot) {
    if (BKE_object_material_get(a, slot) != BKE_object_material_get(b, slot)) {
      return false;
    }
  }
  return true;
}

blender::Object *KX_Scene::TakeParkedCopy(const blender::Object *source)
{
  if (m_parkedCopies.empty()) {
    return nullptr;
  }
  blender::Object *source_ob = const_cast<blender::Object *>(source);
  const int modifiers = BLI_listbase_count(&source->modifiers);
  /* The last parked first: the oldest are deleted after a while (RetireParkedCopies). */
  for (size_t index = m_parkedCopies.size(); index-- > 0;) {
    blender::Object *object = m_parkedCopies[index].object;
    if (m_parkedCopies[index].source_uid == source->id.session_uid &&
        object->data == source->data && BLI_listbase_count(&object->modifiers) == modifiers &&
        same_materials(object, source_ob))
    {
      m_parkedCopies[index] = m_parkedCopies.back();
      m_parkedCopies.pop_back();
      return object;
    }
  }
  return nullptr;
}

void KX_Scene::RetireParkedCopies(const uint64_t frame)
{
  /* As RetireDrawHiddenObjects: looked at every second, parked for two. */
  constexpr uint64_t look_every = 60;
  constexpr uint64_t parked_for = 120;
  if (m_parkedCopies.empty() || frame < m_parkedRetireFrame + look_every) {
    return;
  }
  m_parkedRetireFrame = frame;
  if (frame >= parked_for) {
    DeleteParkedCopies(frame - parked_for);
  }
}

void KX_Scene::DeleteParkedCopies(const uint64_t before)
{
  blender::Set<blender::ID *> doomed;
  for (size_t index = m_parkedCopies.size(); index-- > 0;) {
    if (m_parkedCopies[index].since <= before) {
      doomed.add(&m_parkedCopies[index].object->id);
      m_parkedCopies[index] = m_parkedCopies.back();
      m_parkedCopies.pop_back();
    }
  }
  if (doomed.is_empty()) {
    return;
  }
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  blender::Main *bmain = CTX_data_main(engine->GetContext());
  /* One rebuild for all of them. */
  BKE_id_multi_delete(bmain, doomed);
  TagForRelationsUpdate();
}

/** Why a placed object set to Auto or Merged stays drawn by its own Blender object (null: it
 * can be). */
static const char *game_draw_refusal(KX_GameObject *gameobj,
                                     const blender::Object *ob,
                                     const blender::Set<const blender::Object *> &parents,
                                     const bool shared)
{
  if (ob->type != OB_MESH || ob->data == nullptr) {
    return "only mesh objects can be Merged";
  }
  if (gameobj->IsReplica() || gameobj->IsUpbgeDupliInstance() || (ob->gameflag & OB_DUPLI_UPBGE)) {
    return "it is a copy or a Dupli Base";
  }
  if (ob->gameflag & OB_OVERLAY_COLLECTION) {
    return "it is in a menu or overlay collection";
  }
  if (!BLI_listbase_is_empty(&ob->lodlevels)) {
    return "it has levels of detail";
  }
  if (KX_BlenderSimulatesObject(ob) || BKE_modifiers_findby_type(ob, eModifierType_Armature)) {
    return "Blender simulates or deforms it (physics, particles, an armature)";
  }
  if (ob->parent != nullptr || parents.contains(ob) || !BLI_listbase_is_empty(&ob->constraints) ||
      (ob->adt != nullptr && !BLI_listbase_is_empty(&ob->adt->drivers)) ||
      (ob->transflag & OB_TRANSFLAG_OVERRIDE_GAME_PRIORITY))
  {
    return "Blender poses it or other objects from it (a parent, children, constraints, "
           "drivers)";
  }
  PHY_IPhysicsController *ctrl = gameobj->GetPhysicsController();
  if (ctrl != nullptr && ctrl->IsSoftBody()) {
    return "it is a soft body";
  }
  if (ELEM(ob->body_type, OB_BODY_TYPE_NAVMESH, OB_BODY_TYPE_OCCLUDER)) {
    return "it is a navigation mesh or an occluder";
  }
  if (shared && KX_GetActiveEngine()->UseViewportRender()) {
    return "Use Viewport Render cannot draw grouped objects";
  }
  if (shared && (ob->light_linking != nullptr || ID_IS_LINKED(ob) || ID_IS_OVERRIDE_LIBRARY(ob))) {
    return "it has light linking or comes from a library";
  }
  /* Blender's own animation (an action, drivers or NLA on the object) is evaluated on its own
   * object; the game's actions are played by the game and leave the group when they change it. */
  if (shared && ob->adt != nullptr &&
      (ob->adt->action != nullptr || !BLI_listbase_is_empty(&ob->adt->drivers) ||
       !BLI_listbase_is_empty(&ob->adt->nla_tracks)))
  {
    return "it has Blender animation data";
  }
  if (!shared && ctrl != nullptr && ctrl->IsDynamic()) {
    return "a moving physics body cannot be Merged";
  }
  return nullptr;
}

/** Whether a node tree (or a group in it) reads the drawn object's properties (an Attribute node
 * of type Object or Instancer): a grouped object would read its group's. */
static bool node_tree_reads_object_attributes(const blender::bNodeTree *tree,
                                              blender::Set<const blender::bNodeTree *> &seen)
{
  if (tree == nullptr || !seen.add(tree)) {
    return false;
  }
  for (const blender::bNode &node : tree->nodes) {
    /* A group node uses its node tree. */
    if (node.id != nullptr && GS(node.id->name) == ID_NT) {
      if (node_tree_reads_object_attributes(
              reinterpret_cast<const blender::bNodeTree *>(node.id), seen))
      {
        return true;
      }
    }
    else if (node.type_legacy == SH_NODE_ATTRIBUTE && node.storage != nullptr) {
      const int type = static_cast<const blender::NodeShaderAttribute *>(node.storage)->type;
      if (ELEM(type, SHD_ATTRIBUTE_OBJECT, SHD_ATTRIBUTE_INSTANCER)) {
        return true;
      }
    }
  }
  return false;
}

/** Two Auto objects that draw the same: one Blender object can draw both (each draws with its
 * own pose, color, name for Random, visibility and camera settings). */
static bool same_shared_drawing(blender::Main *bmain, blender::Object *a, blender::Object *b)
{
  /* Visibility settings beside the viewport's own (which the game sets). */
  const int ignored = OB_HIDE_VIEWPORT | OB_HIDE_SELECT;
  if (a->data != b->data || a->totcol != b->totcol || a->index != b->index ||
      a->dt != b->dt || a->dtx != b->dtx || a->shapeflag != b->shapeflag ||
      a->shapenr != b->shapenr ||
      (a->visibility_flag & ~ignored) != (b->visibility_flag & ~ignored) ||
      a->light_linking != nullptr || b->light_linking != nullptr ||
      BLI_listbase_count(&a->modifiers) != BLI_listbase_count(&b->modifiers))
  {
    return false;
  }
  for (short slot = 1; slot <= a->totcol; ++slot) {
    if (BKE_object_material_get(a, slot) != BKE_object_material_get(b, slot)) {
      return false;
    }
  }
  /* Every modifier setting (Blender evaluates the group's object only). */
  blender::ModifierData *md_b = static_cast<blender::ModifierData *>(b->modifiers.first);
  for (blender::ModifierData *md_a = static_cast<blender::ModifierData *>(a->modifiers.first);
       md_a != nullptr;
       md_a = md_a->next, md_b = md_b->next)
  {
    if (md_a->type != md_b->type) {
      return false;
    }
    blender::PointerRNA ptr_a = RNA_pointer_create_discrete(&a->id, RNA_Modifier, md_a);
    blender::PointerRNA ptr_b = RNA_pointer_create_discrete(&b->id, RNA_Modifier, md_b);
    if (!RNA_struct_equals(bmain, &ptr_a, &ptr_b, RNA_EQ_STRICT)) {
      return false;
    }
  }
  return true;
}

void KX_Scene::SetupGameDrawing(blender::Depsgraph *depsgraph,
                                const std::vector<KX_GameObject *> &objects)
{
  /* Blender poses the children of a Blender parent from it in its graph (a Blender parent is in
   * its children's scene, so `objects` hold it). */
  blender::Set<const blender::Object *> parents;
  for (KX_GameObject *gameobj : objects) {
    const blender::Object *ob = gameobj->GetBlenderObject();
    if (ob != nullptr && ob->parent != nullptr) {
      parents.add(ob->parent);
    }
  }
  std::vector<KX_GameObject *> automatic;
  std::vector<KX_GameObject *> merged;
  /* One line per reason: how many, and the first object's name (Merged only: Auto is every
   * object's setting, and those that cannot be grouped simply stay as they are). */
  std::map<std::string, std::pair<int, std::string>> refusals;
  for (KX_GameObject *gameobj : objects) {
    const blender::Object *ob = gameobj->GetBlenderObject();
    /* Copies (collection instances) keep their source's setting: they stay as they are. */
    if (ob == nullptr || ob->game_draw_as == OB_GAME_DRAW_OWN || gameobj->IsReplica()) {
      continue;
    }
    const bool as_auto = ob->game_draw_as == OB_GAME_DRAW_AUTO;
    if (as_auto && ob->type != OB_MESH) {
      continue;
    }
    if (const char *refusal = game_draw_refusal(gameobj, ob, parents, as_auto)) {
      if (!as_auto) {
        auto &item = refusals[refusal];
        if (item.first++ == 0) {
          item.second = ob->id.name + 2;
        }
      }
      continue;
    }
    (as_auto ? automatic : merged).push_back(gameobj);
  }
  for (const auto &item : refusals) {
    CM_Warning("\"" << item.second.second << "\""
                    << (item.second.first > 1 ?
                            " and " + std::to_string(item.second.first - 1) + " more" :
                            std::string())
                    << " drawn as own objects (Game Drawing): " << item.first);
  }
  SetupAutoDrawing(depsgraph, automatic);
  SetupMergedDrawing(depsgraph, merged);
}

void KX_Scene::SetupAutoDrawing(blender::Depsgraph *depsgraph,
                                const std::vector<KX_GameObject *> &objects)
{
  if (objects.size() < 2) {
    return;
  }
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  blender::Main *bmain = CTX_data_main(engine->GetContext());

  /* Identical objects share their mesh: compared only within a mesh's users. */
  blender::Map<const blender::ID *, std::vector<std::vector<KX_GameObject *>>> by_mesh;
  blender::Map<const blender::bNodeTree *, bool> tree_reads_object;
  const auto reads_object_attributes = [&](blender::Object *ob) {
    for (short slot = 1; slot <= ob->totcol; ++slot) {
      const blender::Material *material = BKE_object_material_get(ob, slot);
      if (material == nullptr || material->nodetree == nullptr) {
        continue;
      }
      const bool reads = tree_reads_object.lookup_or_add_cb(material->nodetree, [&]() {
        blender::Set<const blender::bNodeTree *> seen;
        return node_tree_reads_object_attributes(material->nodetree, seen);
      });
      if (reads) {
        return true;
      }
    }
    return false;
  };
  for (KX_GameObject *gameobj : objects) {
    blender::Object *ob = gameobj->GetBlenderObject();
    if (reads_object_attributes(ob)) {
      continue;
    }
    std::vector<std::vector<KX_GameObject *>> &groups = by_mesh.lookup_or_add_default(
        static_cast<const blender::ID *>(ob->data));
    auto group = std::find_if(
        groups.begin(), groups.end(), [&](const std::vector<KX_GameObject *> &members) {
          return same_shared_drawing(bmain, members.front()->GetBlenderObject(), ob);
        });
    if (group == groups.end()) {
      groups.push_back({gameobj});
    }
    else {
      group->push_back(gameobj);
    }
  }
  std::vector<std::vector<KX_GameObject *>> groups;
  for (std::vector<std::vector<KX_GameObject *>> &mesh_groups : by_mesh.values()) {
    for (std::vector<KX_GameObject *> &group : mesh_groups) {
      if (group.size() >= 2) {
        groups.push_back(std::move(group));
      }
    }
  }
  if (groups.empty()) {
    return;
  }

  /* Nothing else of Blender's may read a member (a modifier, constraint or geometry nodes of
   * another object, an instancer): its own object leaves the graph. Its collections only gather
   * it. Asked of the graph as the conversion found it: it may still hold objects the conversion
   * hid since, which can only keep more objects out of a group, never fewer. */
  const auto read_elsewhere = [&](blender::Object *ob) {
    bool read = false;
    DEG_foreach_dependent_ID_component(
        depsgraph,
        &ob->id,
        DEG_OB_COMP_ANY,
        DEG_FOREACH_COMPONENT_IGNORE_TRANSFORM_SOLVERS,
        [&](blender::ID *id, blender::eDepsObjectComponentType /*component*/) {
          if (GS(id->name) != ID_GR) {
            read = true;
          }
        });
    return read;
  };
  for (std::vector<KX_GameObject *> &group : groups) {
    group.erase(std::remove_if(group.begin(),
                               group.end(),
                               [&](KX_GameObject *member) {
                                 return read_elsewhere(member->GetBlenderObject());
                               }),
                group.end());
  }

  /* The group is evaluated at its object's own pose: a member whose modifiers or geometry nodes
   * read its position would look wrong anywhere else. Blender says so when the position changes:
   * the graph brought up to date first (an update the first frame would do anyway), then one mark
   * of every member with modifiers, one update, and those whose geometry was evaluated again stay
   * as they are. */
  std::vector<blender::Object *> probed;
  for (const std::vector<KX_GameObject *> &group : groups) {
    if (group.size() < 2) {
      continue;
    }
    for (KX_GameObject *member : group) {
      blender::Object *ob = member->GetBlenderObject();
      if (!BLI_listbase_is_empty(&ob->modifiers)) {
        probed.push_back(ob);
      }
    }
  }
  blender::Set<const blender::Object *> moves_its_geometry;
  if (!probed.empty()) {
    BKE_scene_graph_update_tagged(depsgraph, bmain);
    for (blender::Object *ob : probed) {
      DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM);
    }
    const uint64_t before_probe = DEG_get_update_count(depsgraph);
    BKE_scene_graph_update_tagged(depsgraph, bmain);
    for (blender::Object *ob : probed) {
      const blender::Object *ob_eval = DEG_get_evaluated(depsgraph, ob);
      /* Not evaluated at all (outside the graph): no answer, so not grouped. */
      if (ob_eval == ob || ob_eval->runtime->last_update_geometry > before_probe) {
        moves_its_geometry.add(ob);
      }
    }
  }

  blender::Scene *scene = GetBlenderScene();
  for (std::vector<KX_GameObject *> &group : groups) {
    std::vector<KX_GameObject *> members;
    for (KX_GameObject *member : group) {
      if (!moves_its_geometry.contains(member->GetBlenderObject())) {
        members.push_back(member);
      }
    }
    if (members.size() < 2) {
      continue;
    }
    /* The group's own Blender object: a copy of a member (any member can then leave the group),
     * in the master collection, never a game object, hidden at each camera pass. */
    blender::Object *first = members.front()->GetBlenderObject();
    blender::Object *base = nullptr;
    BKE_id_copy_ex(bmain, &first->id, reinterpret_cast<blender::ID **>(&base), 0);
    if (base == nullptr) {
      continue;
    }
    id_us_min(&base->id);
    base->visibility_flag &= ~OB_HIDE_VIEWPORT;
    BKE_collection_object_add(bmain, scene->master_collection, base);
    m_sharedDrawBases.push_back(base);
    m_sharedDrawBaseSet.insert(base);
    for (KX_GameObject *member : members) {
      m_sharedMemberOfObject[member->GetBlenderObject()] = member;
      /* Out of the graph (given back by the visibility backup at the end). */
      member->HideInGraph();
      member->MakeSharedInstance(base);
    }
  }
  if (!m_sharedDrawBases.empty()) {
    TagForCollectionRemap();
  }
}

void KX_Scene::DeleteSharedDrawBases()
{
  m_sharedMemberOfObject.clear();
  m_sharedDrawBaseSet.clear();
  if (m_sharedDrawBases.empty()) {
    return;
  }
  blender::Set<blender::ID *> doomed;
  for (blender::Object *base : m_sharedDrawBases) {
    doomed.add(&base->id);
  }
  m_sharedDrawBases.clear();
  BKE_id_multi_delete(CTX_data_main(KX_GetActiveEngine()->GetContext()), doomed);
}

void KX_Scene::SetupMergedDrawing(blender::Depsgraph *depsgraph,
                                  const std::vector<KX_GameObject *> &objects)
{
  if (objects.empty()) {
    return;
  }
  blender::Main *bmain = CTX_data_main(KX_GetActiveEngine()->GetContext());
  /* Nearby objects that draw alike: one batch per 64 m cell (off-screen batches are skipped
   * whole, and a change re-uploads only its batch). */
  constexpr float cell_size = 64.0f;
  const int ignored = OB_HIDE_VIEWPORT | OB_HIDE_SELECT;
  using Key = std::tuple<int, int, int, int, int, int, float, float, float, float>;
  std::map<Key, std::vector<KX_GameObject *>> groups;
  for (KX_GameObject *gameobj : objects) {
    const blender::Object *ob = gameobj->GetBlenderObject();
    const MT_Vector3 position = gameobj->NodeGetWorldPosition();
    const Key key = std::make_tuple(int(std::floor(position[0] / cell_size)),
                                    int(std::floor(position[1] / cell_size)),
                                    int(std::floor(position[2] / cell_size)),
                                    int(ob->visibility_flag & ~ignored),
                                    int(ob->index),
                                    int(ob->dt),
                                    ob->color[0],
                                    ob->color[1],
                                    ob->color[2],
                                    ob->color[3]);
    groups[key].push_back(gameobj);
  }

  for (const auto &item : groups) {
    const std::vector<KX_GameObject *> &group = item.second;
    if (group.size() < 2) {
      continue;
    }
    /* Each member's finished mesh with the materials its object draws, where it stands. */
    std::vector<KX_GameObject *> members;
    std::vector<uint32_t> vertex_counts;
    auto instances = std::make_unique<blender::bke::Instances>();
    for (KX_GameObject *gameobj : group) {
      blender::Object *ob = gameobj->GetBlenderObject();
      blender::Object *ob_eval = DEG_get_evaluated(depsgraph, ob);
      const blender::Mesh *mesh_eval = ob_eval != nullptr && ob_eval != ob ?
                                           BKE_object_get_evaluated_mesh(ob_eval) :
                                           nullptr;
      if (mesh_eval == nullptr || mesh_eval->verts_num == 0) {
        continue;
      }
      blender::Mesh *copy = BKE_mesh_copy_for_eval(*mesh_eval);
      for (short slot = 1; slot <= ob_eval->totcol; ++slot) {
        BKE_id_material_eval_assign(&copy->id, slot, BKE_object_material_get_eval(ob_eval, slot));
      }
      const int index = instances->instances_num();
      instances->resize(index + 1);
      instances->reference_handles_for_write()[index] = instances->add_new_reference(
          blender::bke::InstanceReference(blender::bke::GeometrySet::from_mesh(copy)));
      instances->transforms_for_write()[index] = ob_eval->object_to_world();
      members.push_back(gameobj);
      vertex_counts.push_back(uint32_t(copy->verts_num));
    }
    if (members.size() < 2) {
      continue;
    }
    blender::geometry::RealizeInstancesOptions options;
    blender::bke::GeometrySet realized =
        blender::geometry::realize_instances(
            blender::bke::GeometrySet::from_instances(std::move(instances)), options)
            .geometry;
    blender::Mesh *merged = realized.get_component_for_write<blender::bke::MeshComponent>()
                                .release();
    uint64_t total = 0;
    for (const uint32_t count : vertex_counts) {
      total += count;
    }
    if (merged == nullptr || uint64_t(merged->verts_num) != total) {
      CM_Warning("Could not merge " << members.size() << " objects (Game Drawing Merged)");
      if (merged != nullptr) {
        BKE_id_free(nullptr, &merged->id);
      }
      continue;
    }

    /* A Blender object of its own, drawn in place of the members (deleted at the end). Its
     * materials are the originals: the members' meshes held Blender's evaluated copies. */
    std::vector<blender::Material *> materials;
    for (const int slot : blender::IndexRange(merged->totcol)) {
      materials.push_back(merged->mat[slot] ? DEG_get_original(merged->mat[slot]) : nullptr);
    }
    blender::Mesh *mesh = BKE_mesh_add(bmain, "Merged Scenery");
    BKE_mesh_nomain_to_mesh(merged, mesh, nullptr);
    blender::Object *object = BKE_object_add_only_object(bmain, OB_MESH, "Merged Scenery");
    object->data = &mesh->id;
    for (size_t slot = 0; slot < materials.size(); ++slot) {
      BKE_object_material_assign(
          bmain, object, materials[slot], short(slot + 1), BKE_MAT_ASSIGN_OBDATA);
    }
    const blender::Object *first = members.front()->GetBlenderObject();
    object->visibility_flag = decltype(object->visibility_flag)(first->visibility_flag & ~ignored);
    object->index = first->index;
    object->dt = first->dt;
    copy_v4_v4(object->color, first->color);
    BKE_collection_object_add(bmain, GetBlenderScene()->master_collection, object);

    MergedBatch *batch = new MergedBatch();
    batch->object = object;
    batch->mesh = mesh;
    const blender::Span<blender::float3> positions = mesh->vert_positions();
    batch->rest_positions.resize(size_t(positions.size()) * 3);
    std::copy_n(&positions.data()->x, batch->rest_positions.size(), batch->rest_positions.data());
    const int batch_index = int(m_mergedBatches.size());
    m_mergedBatches.push_back(batch);

    uint32_t first_vertex = 0;
    for (size_t i = 0; i < members.size(); ++i) {
      KX_GameObject *member = members[i];
      member->HideInGraph();
      member->SetMergedPart(batch_index, first_vertex, vertex_counts[i]);
      if (!member->GetVisible()) {
        member->ApplyMergedVisibility();
      }
      first_vertex += vertex_counts[i];
    }
    TagForCollectionRemap();
  }
}

void KX_Scene::SetMergedPartShown(const int batch,
                                  const uint32_t first_vertex,
                                  const uint32_t vertex_count,
                                  const bool shown)
{
  /* The batches go at the end anyway. */
  if (!m_isRuntime || batch < 0 || batch >= int(m_mergedBatches.size())) {
    return;
  }
  MergedBatch &merged = *m_mergedBatches[batch];
  blender::MutableSpan<blender::float3> positions = merged.mesh->vert_positions_for_write();
  if (uint64_t(first_vertex) + vertex_count > uint64_t(positions.size())) {
    return;
  }
  /* Hidden: every vertex on the part's first one, so its faces cover nothing. */
  const float *rest = merged.rest_positions.data();
  for (uint32_t i = 0; i < vertex_count; ++i) {
    const size_t from = size_t(shown ? first_vertex + i : first_vertex) * 3;
    positions[first_vertex + i] = blender::float3(rest[from], rest[from + 1], rest[from + 2]);
  }
  merged.mesh->tag_positions_changed();
  DEG_id_tag_update(&merged.mesh->id, ID_RECALC_GEOMETRY);
}

void KX_Scene::DeleteMergedBatches()
{
  if (m_mergedBatches.empty()) {
    return;
  }
  blender::Set<blender::ID *> doomed;
  for (MergedBatch *batch : m_mergedBatches) {
    doomed.add(&batch->object->id);
    doomed.add(&batch->mesh->id);
    delete batch;
  }
  m_mergedBatches.clear();
  BKE_id_multi_delete(CTX_data_main(KX_GetActiveEngine()->GetContext()), doomed);
}

void KX_Scene::RestoreCameraHiddenObjects()
{
  /* Last changed first: an object saved twice gets its first flags back. */
  for (auto item = m_cameraHiddenRestore.rbegin(); item != m_cameraHiddenRestore.rend(); ++item)
  {
    item->first->visibility_flag = decltype(item->first->visibility_flag)(item->second);
  }
  m_cameraHiddenRestore.clear();
  m_cameraHiddenPass = nullptr;
  m_cameraHiddenPassWorkbench = false;
  m_cameraHiddenPassCollection = -1;
  m_cameraHiddenPassCollectionHide = false;
  m_cullPass.clip = false;
  m_cullPass.distances = false;
}

bool KX_Scene::IsHiddenInCameraPass(KX_GameObject *object, bool &r_keep_shadow)
{
  if (m_cameraHiddenPass == nullptr) {
    return false;
  }
  for (const CameraHiddenObject &hidden : m_cameraHiddenObjects) {
    if (hidden.object == object && camera_matches(hidden.camera,
                                                  m_cameraHiddenPass,
                                                  m_overlayCamera,
                                                  m_overlayCameraSource))
    {
      /* Workbench draws no shadows: hidden fully. */
      r_keep_shadow = hidden.keep_shadow && !m_cameraHiddenPassWorkbench;
      return true;
    }
  }
  if (m_cameraHiddenPassCollection >= 0 &&
      HiddenByCameraCollection(
          object, m_cameraHiddenPassCollection, m_cameraHiddenPassCollectionHide))
  {
    r_keep_shadow = false;
    return true;
  }
  if (CulledInCameraPass(object)) {
    r_keep_shadow = false;
    return true;
  }
  return false;
}

/** A light probe drawing the scene from another place (sphere, planar): what it sees is not what
 * the camera sees. */
static bool scene_has_reflection_probes(const blender::Main *bmain)
{
  for (const blender::LightProbe &probe : bmain->lightprobes) {
    if (probe.type != LIGHTPROBE_TYPE_VOLUME && ID_REAL_USERS(&probe.id) > 0) {
      return true;
    }
  }
  return false;
}

static const blender::bNodeSocket *node_input(const blender::bNode &node, const char *identifier)
{
  for (const blender::bNodeSocket &socket : node.inputs) {
    if (STREQ(socket.identifier, identifier)) {
      return &socket;
    }
  }
  return nullptr;
}

/** EEVEE takes a sun casting shadows from the world's light brighter than World > Sun >
 * Threshold, in a direction only the drawing knows: from a texture (an HDRI, a sky), or from a
 * background whose color or strength comes from other nodes or is that bright. */
static bool world_may_cast_sun_shadows(const blender::World *world)
{
  using namespace blender;
  if (world == nullptr || !(world->flag & WO_USE_SUN_SHADOW) || world->nodetree == nullptr) {
    return false;
  }
  const bNodeTree &tree = *world->nodetree;
  const auto linked = [&](const bNodeSocket *socket) {
    for (const bNodeLink &link : tree.links) {
      if (link.tosock == socket) {
        return true;
      }
    }
    return false;
  };
  for (const bNode &node : tree.nodes) {
    if (ELEM(node.type_legacy, SH_NODE_TEX_ENVIRONMENT, SH_NODE_TEX_IMAGE, SH_NODE_TEX_SKY)) {
      return true;
    }
    if (ELEM(node.type_legacy, SH_NODE_BACKGROUND, SH_NODE_EMISSION)) {
      const bNodeSocket *color = node_input(node, "Color");
      const bNodeSocket *strength = node_input(node, "Strength");
      if (color == nullptr || strength == nullptr || linked(color) || linked(strength)) {
        return true;
      }
      const float *rgb = static_cast<const bNodeSocketValueRGBA *>(color->default_value)->value;
      const float brightest = std::max({rgb[0], rgb[1], rgb[2]}) *
                              static_cast<const bNodeSocketValueFloat *>(strength->default_value)
                                  ->value;
      if (brightest > world->sun_threshold) {
        return true;
      }
    }
  }
  return false;
}

/** The camera's "use_draw_distances" custom property (on the camera object, else its data): 0
 * draws everything, whatever the objects' draw distances. */
static bool camera_uses_draw_distances(KX_Camera *camera)
{
  const blender::Object *ob = camera->GetBlenderObject();
  if (ob == nullptr) {
    return true;
  }
  for (const blender::ID *id : {&ob->id, static_cast<const blender::ID *>(ob->data)}) {
    if (id != nullptr && id->properties != nullptr) {
      if (const blender::IDProperty *prop = blender::IDP_GetPropertyFromGroup(
              id->properties, "use_draw_distances"))
      {
        return blender::IDP_coerce_to_double_or_zero(prop) != 0.0;
      }
    }
  }
  return true;
}

void KX_Scene::PrepareCullPass(blender::Depsgraph *depsgraph,
                               KX_Camera *camera,
                               const bool workbench,
                               const bool is_overlay_pass)
{
  using namespace blender;
  m_cullPass.clip = false;
  m_cullPass.distances = false;
  m_cullPass.lights.clear();
  m_cullPass.depsgraph = depsgraph;
  ++m_cullPass.id;
  /* UPBGE_DRAW_CULLING=0 draws everything (tests compare both). Menus and HUDs (the overlay
   * pass) have nothing far away. */
  static const bool culling_off = [] {
    const char *value = getenv("UPBGE_DRAW_CULLING");
    return value != nullptr && value[0] == '0';
  }();
  if (culling_off || camera == nullptr || depsgraph == nullptr || is_overlay_pass) {
    return;
  }
  const Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);
  if (scene_eval == nullptr) {
    return;
  }
  float camera_to_world[4][4];
  float projection[4][4];
  camera->GetCameraToWorld().getValue(&camera_to_world[0][0]);
  camera->GetProjectionMatrix().getValue(&projection[0][0]);
  m_cullPass.view = KX_DrawCulling::MakeView(camera_to_world, projection);

  /* The cameras met so far get a bit each (a removed camera's slot is reused). */
  m_cullPass.camera_bit = 0;
  auto slot = std::find(m_cullCameras.begin(), m_cullCameras.end(), camera);
  if (slot == m_cullCameras.end()) {
    slot = std::find(m_cullCameras.begin(), m_cullCameras.end(), nullptr);
    if (slot != m_cullCameras.end()) {
      *slot = camera;
    }
    else if (m_cullCameras.size() < 32) {
      slot = m_cullCameras.insert(m_cullCameras.end(), camera);
    }
  }
  m_cullPass.redraws = true;
  if (slot != m_cullCameras.end()) {
    const size_t index = size_t(slot - m_cullCameras.begin());
    m_cullPass.camera_bit = 1u << uint32_t(index);
    m_cullCameraPasses.resize(m_cullCameras.size());
    CullCameraPass &last = m_cullCameraPasses[index];
    /* Renderers draw anew on a new view or a Blender update (the last one is this pass's). */
    const uint64_t update_count = DEG_get_update_count(depsgraph);
    m_cullPass.redraws = !last.known || last.update_count != update_count ||
                         std::memcmp(&last.view, &m_cullPass.view, sizeof(m_cullPass.view)) != 0;
    last = {true, m_cullPass.view, update_count};
  }

  m_cullPass.distances = camera_uses_draw_distances(camera);

  /* Clip End culling only when nothing beyond Clip End can show: no reflection probe, and every
   * shadow it could cast comes from a light whose direction is known. */
  const Main *bmain = CTX_data_main(KX_GetActiveEngine()->GetContext());
  if (bmain == nullptr || scene_has_reflection_probes(bmain) ||
      (workbench ? (scene_eval->display.shading.flag & V3D_SHADING_SHADOW) != 0 :
                   world_may_cast_sun_shadows(scene_eval->world)))
  {
    return;
  }
  if (!workbench) {
    for (KX_LightObject *light : *m_lightlist) {
      const Light *la = light->GetLight();
      if (la == nullptr || !(la->mode & LA_SHADOW)) {
        continue;
      }
      /* A scaled light is bigger (its shadows softer). */
      const MT_Vector3 &scale = light->NodeGetWorldScaling();
      const float size_scale = std::max(
          {std::fabs(scale.x()), std::fabs(scale.y()), std::fabs(scale.z())});
      KX_DrawCulling::Light cull_light;
      if (la->type == LA_SUN) {
        cull_light.directional = true;
        /* Its light travels down its -Z axis. */
        MT_Vector3 travel = -light->NodeGetWorldOrientation().getColumn(2);
        if (travel.fuzzyZero()) {
          return;
        }
        travel.normalize();
        travel.getValue(cull_light.vector);
        cull_light.spread = la->sun_angle < float(M_PI) ? std::tan(0.5f * la->sun_angle) :
                                                          FLT_MAX;
      }
      else {
        light->NodeGetWorldPosition().getValue(cull_light.vector);
        cull_light.spread = (la->type == LA_AREA ?
                                 0.5f * std::hypot(la->area_size, la->area_sizey) :
                                 la->radius) *
                            size_scale;
      }
      m_cullPass.lights.push_back(cull_light);
    }
  }
  m_cullPass.clip = true;
}

/** The bounds of what Blender draws for the object (its own Blender object, its Auto group's or
 * its Dupli Base's), as a sphere in its own space, and which culling may leave it out. */
static void find_cull_bounds(KX_GameObject *object, blender::Depsgraph *depsgraph)
{
  using namespace blender;
  KX_GameObject::DrawCullState &state = object->GetDrawCullState();
  state.bounds_known = true;
  state.cullable = false;
  state.clip_cullable = false;
  state.world_node = nullptr;
  const Object *source = object->GetDupliDrawBase();
  /* Menus and HUDs are drawn by their own pass. */
  if (source == nullptr || source->type != OB_MESH || (source->gameflag & OB_OVERLAY_COLLECTION) ||
      (source->transflag & OB_DUPLI))
  {
    return;
  }
  const Object *source_eval = depsgraph != nullptr ? DEG_get_evaluated(depsgraph, source) :
                                                     nullptr;
  std::optional<Bounds<float3>> bounds;
  if (source_eval != nullptr && source_eval != source) {
    /* Geometry Nodes instances are not in these bounds. */
    const bke::GeometrySet *geometry = source_eval->runtime->geometry_set_eval;
    if (geometry != nullptr && geometry->has_instances()) {
      return;
    }
    bounds = BKE_object_evaluated_geometry_bounds(source_eval);
  }
  if (!bounds) {
    bounds = BKE_object_boundbox_get(source);
  }
  if (!bounds) {
    return;
  }
  const float3 center = math::midpoint(bounds->min, bounds->max);
  const float radius = 0.5f * math::distance(bounds->min, bounds->max);
  if (!std::isfinite(radius) || !std::isfinite(center.x) || !std::isfinite(center.y) ||
      !std::isfinite(center.z))
  {
    return;
  }
  copy_v3_v3(state.center, center);
  state.radius = radius;
  state.cullable = true;
  const Mesh *mesh = id_cast<const Mesh *>(source->data);
  state.clip_cullable = BLI_listbase_is_empty(&source->modifiers) &&
                        BLI_listbase_is_empty(&source->particlesystem) && mesh != nullptr &&
                        mesh->key == nullptr;
}

/** Whether the pass's view leaves the object out (KX_DrawCulling); `culled_before`: its last pass
 * did. */
static bool cull_test(KX_GameObject *object,
                      const float draw_distance,
                      const bool culled_before,
                      const KX_DrawCulling::View &view,
                      const bool clip,
                      const std::vector<KX_DrawCulling::Light> &lights)
{
  KX_GameObject::DrawCullState &state = object->GetDrawCullState();
  const bool clip_test = clip && state.clip_cullable;
  if (draw_distance <= 0.0f && !clip_test) {
    return false;
  }
  /* The sphere in the world, found again only after a write to the world transform. */
  const SG_Node *node = object->GetSGNode();
  if (state.world_node != node || state.world_stamp != node->GetWorldStamp()) {
    state.world_node = node;
    state.world_stamp = node->GetWorldStamp();
    object->NodeGetWorldTransform()(MT_Vector3(state.center)).getValue(state.world_center);
    const MT_Vector3 &scale = object->NodeGetWorldScaling();
    state.world_radius = state.radius * std::max({std::fabs(scale.x()),
                                                  std::fabs(scale.y()),
                                                  std::fabs(scale.z())});
  }
  const float *center = state.world_center;
  const float radius = state.world_radius;
  if (draw_distance > 0.0f &&
      KX_DrawCulling::BeyondDrawDistance(view, center, radius, draw_distance, culled_before))
  {
    return true;
  }
  return clip_test && KX_DrawCulling::BeyondFarPlane(view, center, radius) &&
         std::none_of(lights.begin(), lights.end(), [&](const KX_DrawCulling::Light &light) {
           return KX_DrawCulling::ShadowMayReachView(view, light, center, radius);
         });
}

bool KX_Scene::CulledInCameraPass(KX_GameObject *object)
{
  if (!m_cullPass.clip && !m_cullPass.distances) {
    return false;
  }
  KX_GameObject::DrawCullState &state = object->GetDrawCullState();
  /* A copy drawn by its own entry is asked again by the copies' provider. */
  if (state.pass == m_cullPass.id) {
    return state.pass_culled;
  }
  state.pass = m_cullPass.id;
  state.pass_culled = CullDecision(object);
  return state.pass_culled;
}

bool KX_Scene::CullDecision(KX_GameObject *object)
{
  KX_GameObject::DrawCullState &state = object->GetDrawCullState();
  if (!state.bounds_known) {
    find_cull_bounds(object, m_cullPass.depsgraph);
  }
  const bool culled_before = (state.culled_cameras & m_cullPass.camera_bit) != 0;
  bool culled = false;
  if (state.cullable) {
    float draw_distance = 0.0f;
    if (m_cullPass.distances) {
      draw_distance = object->GetOwnDrawDistance();
      if (draw_distance <= 0.0f) {
        draw_distance = CollectionDrawDistance(object);
      }
    }
    culled = cull_test(object,
                       draw_distance,
                       culled_before,
                       m_cullPass.view,
                       m_cullPass.clip,
                       m_cullPass.lights);
  }
  const uint32_t bit = m_cullPass.camera_bit;
  if (bit == 0) {
    return culled;
  }
  if (culled == culled_before) {
    state.pending_cameras &= ~bit;
    return culled;
  }
  /* A renderer drawing anew shows the change. One that finished its samples draws nothing new
   * until told, and its sync this pass would still drop a vanished object's shadow record without
   * drawing (EEVEE: the shadow then stayed). Told now, its update comes at the next pass, which
   * applies the change. */
  if (!m_cullPass.redraws && !(state.pending_cameras & bit)) {
    state.pending_cameras |= bit;
    NoteDrawChanged(object);
    return culled_before;
  }
  state.pending_cameras &= ~bit;
  state.culled_cameras ^= bit;
  return culled;
}

/** A collection's "draw_distance" custom property, else its nearest parents' (the largest of
 * them): 0 without one (a 0 there draws its objects at any distance). */
static float collection_draw_distance(const blender::Collection *collection, const int depth)
{
  using namespace blender;
  if (collection == nullptr || depth > 64) {
    return 0.0f;
  }
  if (collection->id.properties != nullptr) {
    if (const IDProperty *prop = IDP_GetPropertyFromGroup(collection->id.properties,
                                                          "draw_distance"))
    {
      const double distance = IDP_coerce_to_double_or_zero(prop);
      return (distance > 0.0 && std::isfinite(distance)) ? float(distance) : 0.0f;
    }
  }
  float distance = 0.0f;
  if (collection->runtime != nullptr) {
    for (const CollectionParent &parent : collection->runtime->parents) {
      distance = std::max(distance, collection_draw_distance(parent.collection, depth + 1));
    }
  }
  return distance;
}

float KX_Scene::CollectionDrawDistance(KX_GameObject *object)
{
  KX_GameObject::DrawCullState &state = object->GetDrawCullState();
  if (state.collection_distance_version != m_membershipVersion) {
    state.collection_distance_version = m_membershipVersion;
    float distance = 0.0f;
    for (const blender::Collection *collection : CollectionsOf(object)) {
      distance = std::max(distance, collection_draw_distance(collection, 0));
    }
    state.collection_distance = distance;
  }
  return state.collection_distance;
}

int KX_Scene::CameraCollectionIndex(const blender::Collection *collection)
{
  if (collection == nullptr) {
    return -1;
  }
  const uint32_t session_uid = collection->id.session_uid;
  CameraCollection *entry = nullptr;
  for (CameraCollection &camera_collection : m_cameraCollections) {
    if (camera_collection.collection == collection) {
      if (camera_collection.session_uid == session_uid) {
        return int(&camera_collection - m_cameraCollections.data());
      }
      /* A freed collection's address, reused: find this one's objects. */
      entry = &camera_collection;
      break;
    }
  }
  if (entry == nullptr) {
    if (m_cameraCollections.size() >= 32) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        CM_Warning("More than 32 collections in cameras' Collection settings: the others "
                   "draw everything.");
      }
      return -1;
    }
    entry = &m_cameraCollections.emplace_back();
  }
  entry->collection = collection;
  entry->session_uid = session_uid;
  /* The game objects find their bits again (once each). */
  m_cameraCollectionsVersion = next_cache_version(m_cameraCollectionsVersion);
  return int(entry - m_cameraCollections.data());
}

bool KX_Scene::HiddenByCameraCollection(KX_GameObject *object, const int index, const bool hide)
{
  const blender::Object *own = object->GetBlenderObject();
  /* Lights keep lighting the view (EEVEE), probes keep their part, cameras draw nothing. */
  if (own == nullptr || ELEM(own->type, OB_LAMP, OB_LIGHTPROBE, OB_CAMERA)) {
    return false;
  }
  uint32_t bits;
  if (!object->GetCameraCollectionBits(m_cameraCollectionsVersion, bits)) {
    bits = 0;
    for (size_t i = 0; i < m_cameraCollections.size(); i++) {
      if (IsInCollection(object, m_cameraCollections[i].collection, true)) {
        bits |= 1u << i;
      }
    }
    object->SetCameraCollectionBits(bits, m_cameraCollectionsVersion);
  }
  const bool in_collection = (bits >> index) & 1u;
  return hide ? in_collection : !in_collection;
}

void KX_Scene::NoteCollectionsChanged()
{
  m_collectionIndex = CollectionIndex();
  m_membershipVersion = next_cache_version(m_membershipVersion);
  m_cameraCollectionsVersion = next_cache_version(m_cameraCollectionsVersion);
  m_collectionMembersCache.clear();
  /* A camera's Collection may be a freed library's: the cameras add theirs again at their next
   * draw (CameraCollectionIndex), so no object's bits read a freed collection. */
  if (m_cameraHiddenPass == nullptr) {
    m_cameraCollections.clear();
  }
  /* Set Collection Member only takes this scene's collections: drop changes for those that left
   * it, so no object keeps a collection that may be freed. */
  for (EXP_ListValue<KX_GameObject> *list : {GetObjectList(), GetInactiveList()}) {
    if (list == nullptr) {
      continue;
    }
    for (KX_GameObject *object : *list) {
      const std::vector<std::pair<const blender::Collection *, bool>> &memberships =
          object->GetCollectionMemberships();
      if (std::all_of(memberships.begin(), memberships.end(), [&](const auto &entry) {
            return CollectionInScene(entry.first);
          }))
      {
        continue;
      }
      std::vector<std::pair<const blender::Collection *, bool>> kept = memberships;
      std::erase_if(kept, [&](const auto &entry) { return !CollectionInScene(entry.first); });
      object->SetCollectionMemberships(std::move(kept));
      ++m_membershipEdits;
    }
  }
  if (!m_runtimeCollectionRenderVisibility.empty()) {
    RebuildRuntimeCollectionRenderVisibility();
  }
}

void KX_Scene::EnsureCollectionIndex()
{
  if (m_collectionIndex.built) {
    return;
  }
  m_collectionIndex.built = true;
  const blender::Scene *scene = GetBlenderScene();
  if (scene == nullptr || scene->master_collection == nullptr) {
    return;
  }
  /* What the game linked into the scene's own collection to draw it: a Dupli Base kept in
   * Blender's graph, a merged scene's object (whose own scene's collections gather it). */
  std::unordered_set<const blender::Object *> game_links(m_mergedObjectLinks.begin(),
                                                         m_mergedObjectLinks.end());
  for (const DupliBaseInGraph &base : m_dupliBasesInGraph) {
    if (base.linked) {
      game_links.insert(base.object);
    }
  }
  std::unordered_set<const blender::Collection *> visited;
  std::vector<const blender::Collection *> pending = {scene->master_collection};
  m_collectionIndex.parents[scene->master_collection];
  /* Scenes merged into this one (Load Scene): their collections are the scene's too. */
  for (const blender::Scene *merged : m_mergedBlenderScenes) {
    if (merged->master_collection != nullptr) {
      pending.push_back(merged->master_collection);
      m_collectionIndex.parents[merged->master_collection];
    }
  }
  while (!pending.empty()) {
    const blender::Collection *collection = pending.back();
    pending.pop_back();
    if (!visited.insert(collection).second) {
      continue;
    }
    for (const blender::CollectionObject &entry : collection->gobject) {
      if (entry.ob == nullptr) {
        continue;
      }
      if (collection == scene->master_collection && game_links.count(entry.ob)) {
        continue;
      }
      std::vector<const blender::Collection *> &collections =
          m_collectionIndex.object_collections[entry.ob];
      if (std::find(collections.begin(), collections.end(), collection) == collections.end()) {
        collections.push_back(collection);
      }
    }
    for (const blender::CollectionChild &child : collection->children) {
      if (child.collection == nullptr) {
        continue;
      }
      m_collectionIndex.parents[child.collection].push_back(collection);
      pending.push_back(child.collection);
    }
  }
}

const std::unordered_set<const blender::Collection *> &KX_Scene::CollectionAncestors(
    const blender::Collection *collection)
{
  EnsureCollectionIndex();
  if (const auto found = m_collectionIndex.ancestors.find(collection);
      found != m_collectionIndex.ancestors.end())
  {
    return found->second;
  }
  /* Blender collections never form a cycle; the visited set only guards the walk. */
  std::unordered_set<const blender::Collection *> ancestors = {collection};
  std::vector<const blender::Collection *> pending = {collection};
  while (!pending.empty()) {
    const blender::Collection *current = pending.back();
    pending.pop_back();
    if (const auto parents = m_collectionIndex.parents.find(current);
        parents != m_collectionIndex.parents.end())
    {
      for (const blender::Collection *parent : parents->second) {
        if (ancestors.insert(parent).second) {
          pending.push_back(parent);
        }
      }
    }
  }
  return m_collectionIndex.ancestors.emplace(collection, std::move(ancestors)).first->second;
}

bool KX_Scene::CollectionInScene(const blender::Collection *collection)
{
  EnsureCollectionIndex();
  return collection != nullptr && m_collectionIndex.parents.contains(collection);
}

const blender::Collection *KX_Scene::FindSceneCollection(const std::string &name,
                                                         const std::string &library_path)
{
  EnsureCollectionIndex();
  for (const auto &item : m_collectionIndex.parents) {
    const blender::Collection *collection = item.first;
    /* The master collection is not a data-block of its own (no node can pick it). */
    if ((collection->flag & blender::COLLECTION_IS_MASTER) != 0 ||
        name != collection->id.name + 2)
    {
      continue;
    }
    const std::string path = collection->id.lib ? collection->id.lib->filepath : "";
    if (path == library_path) {
      return collection;
    }
  }
  return nullptr;
}

std::pair<const blender::Object *, const blender::Object *> KX_Scene::CollectionSourcesOf(
    KX_GameObject *object) const
{
  /* A menu copy keeps its authored source (as for Set Collection Render Visibility before). */
  const auto overlay_source = m_overlayMemberSources.find(object);
  const blender::Object *source = overlay_source != m_overlayMemberSources.end() ?
                                      overlay_source->second :
                                      object->GetCollectionSource();
  /* A copy made for a collection instance (DupliGroupRecurse) is also in the collections of the
   * Empty that instances it. */
  KX_GameObject *group = object->GetDupliGroupObject();
  return {source, group ? group->GetCollectionSource() : nullptr};
}

const std::vector<const blender::Collection *> &KX_Scene::CollectionsOf(KX_GameObject *object)
{
  static const std::vector<const blender::Collection *> none;
  if (object == nullptr) {
    return none;
  }
  if (const std::vector<const blender::Collection *> *collections =
          object->GetEffectiveCollections(m_membershipVersion))
  {
    return *collections;
  }
  EnsureCollectionIndex();
  std::vector<const blender::Collection *> collections;
  const auto add_authored = [&](const blender::Object *source) {
    if (source == nullptr) {
      return;
    }
    const auto found = m_collectionIndex.object_collections.find(source);
    if (found == m_collectionIndex.object_collections.end()) {
      return;
    }
    for (const blender::Collection *collection : found->second) {
      if (std::find(collections.begin(), collections.end(), collection) == collections.end()) {
        collections.push_back(collection);
      }
    }
  };
  add_authored(CollectionSourcesOf(object).first);
  /* A collection instance's copy is also in the collections the Empty instancing it is in, Set
   * Collection Member's changes to the Empty included (NoteObjectCollectionsChanged). */
  if (KX_GameObject *group = object->GetDupliGroupObject()) {
    for (const blender::Collection *collection : CollectionsOf(group)) {
      if (std::find(collections.begin(), collections.end(), collection) == collections.end()) {
        collections.push_back(collection);
      }
    }
  }
  for (const auto &[collection, member] : object->GetCollectionMemberships()) {
    if (member) {
      if (std::find(collections.begin(), collections.end(), collection) == collections.end()) {
        collections.push_back(collection);
      }
      continue;
    }
    std::erase_if(collections, [&](const blender::Collection *current) {
      return CollectionAncestors(current).contains(collection);
    });
  }
  return object->SetEffectiveCollections(std::move(collections), m_membershipVersion);
}

bool KX_Scene::IsInCollection(KX_GameObject *object,
                              const blender::Collection *collection,
                              const bool include_children)
{
  if (collection == nullptr || object == nullptr) {
    return false;
  }
  if (!CollectionInScene(collection)) {
    /* Not linked in the scene: what the file links to it. */
    blender::Collection *outside = const_cast<blender::Collection *>(collection);
    const auto [source, group_source] = CollectionSourcesOf(object);
    for (const blender::Object *item : {source, group_source}) {
      blender::Object *authored = const_cast<blender::Object *>(item);
      if (authored != nullptr &&
          (include_children ? BKE_collection_has_object_recursive(outside, authored) :
                              BKE_collection_has_object(outside, authored)))
      {
        return true;
      }
    }
    return false;
  }
  for (const blender::Collection *current : CollectionsOf(object)) {
    if (current == collection ||
        (include_children && CollectionAncestors(current).contains(collection)))
    {
      return true;
    }
  }
  return false;
}

const std::vector<KX_GameObject *> &KX_Scene::CollectionMembers(
    const blender::Collection *collection, const bool include_children)
{
  CollectionMembersCache &cache = m_collectionMembersCache[{collection, include_children}];
  if (cache.object_list_generation == m_objectListGeneration &&
      cache.membership_version == m_membershipVersion &&
      cache.membership_edits == m_membershipEdits && cache.membership_version != 0)
  {
    return cache.members;
  }
  cache.object_list_generation = m_objectListGeneration;
  cache.membership_version = m_membershipVersion;
  cache.membership_edits = m_membershipEdits;
  cache.members.clear();
  EXP_ListValue<KX_GameObject> *objects = GetObjectList();
  if (collection == nullptr || objects == nullptr) {
    return cache.members;
  }
  std::unordered_set<KX_GameObject *> live;
  live.reserve(objects->GetCount());
  for (KX_GameObject *object : *objects) {
    live.insert(object);
  }
  /* A collection the scene doesn't link (see IsInCollection) holds its file's objects, found
   * here once for all objects. */
  const bool in_scene = CollectionInScene(collection);
  std::unordered_set<const blender::Object *> outside_objects;
  if (!in_scene) {
    std::unordered_set<const blender::Collection *> seen;
    const auto collect = [&](const auto &self, const blender::Collection *current) -> void {
      if (current == nullptr || !seen.insert(current).second) {
        return;
      }
      for (const blender::CollectionObject &entry : current->gobject) {
        if (entry.ob != nullptr) {
          outside_objects.insert(entry.ob);
        }
      }
      if (include_children) {
        for (const blender::CollectionChild &child : current->children) {
          self(self, child.collection);
        }
      }
    };
    collect(collect, collection);
  }
  const auto is_member = [&](KX_GameObject *object) {
    if (in_scene) {
      return IsInCollection(object, collection, include_children);
    }
    const auto [source, group_source] = CollectionSourcesOf(object);
    return (source != nullptr && outside_objects.contains(source)) ||
           (group_source != nullptr && outside_objects.contains(group_source));
  };
  std::unordered_set<KX_GameObject *> added;
  /* First the objects linked to the collection in the file, in its order (Get Collection
   * Objects' order before 2026-10-15), then its child collections' when they count. */
  std::unordered_set<const blender::Collection *> visited;
  const auto add_linked = [&](const auto &self, const blender::Collection *current) -> void {
    if (current == nullptr || !visited.insert(current).second) {
      return;
    }
    for (const blender::CollectionObject &entry : current->gobject) {
      KX_GameObject *object = entry.ob ? GetGameObjectFromObject(entry.ob) : nullptr;
      if (object != nullptr && live.contains(object) && is_member(object) &&
          added.insert(object).second)
      {
        cache.members.push_back(object);
      }
    }
    if (include_children) {
      for (const blender::CollectionChild &child : current->children) {
        self(self, child.collection);
      }
    }
  };
  add_linked(add_linked, collection);
  /* Then the others: Add Object copies and objects Set Collection Member put in. */
  for (KX_GameObject *object : *objects) {
    if (!added.contains(object) && is_member(object)) {
      added.insert(object);
      cache.members.push_back(object);
    }
  }
  return cache.members;
}

bool KX_Scene::SetCollectionMember(KX_GameObject *object,
                                   blender::Collection *collection,
                                   const bool member)
{
  if (object == nullptr || !CollectionInScene(collection)) {
    return false;
  }
  object->SetCollectionMembership(collection, member);
  NoteObjectCollectionsChanged(object);
  return true;
}

void KX_Scene::RestoreCollectionMemberships(
    KX_GameObject *object, std::vector<std::pair<const blender::Collection *, bool>> memberships)
{
  if (object == nullptr) {
    return;
  }
  object->SetCollectionMemberships(std::move(memberships));
  NoteObjectCollectionsChanged(object);
}

void KX_Scene::NoteObjectCollectionsChanged(KX_GameObject *object)
{
  ++m_membershipEdits;
  std::vector<KX_GameObject *> changed = {object};
  for (size_t index = 0; index < changed.size(); index++) {
    changed[index]->InvalidateCollectionMembership();
    if (EXP_ListValue<KX_GameObject> *instances = changed[index]->GetInstanceObjects()) {
      for (KX_GameObject *instance : *instances) {
        changed.push_back(instance);
      }
    }
  }
  if (!m_runtimeCollectionRenderVisibility.empty()) {
    for (KX_GameObject *item : changed) {
      ApplyRuntimeCollectionRenderVisibility(item);
    }
  }
  /* A camera that draws only a collection's objects, or hides them, may draw these differently
   * now (HideCameraHiddenObjects). */
  bool cameras_use_collections = false;
  for (KX_Camera *camera : *GetCameraList()) {
    const blender::Camera *data = camera_data(camera);
    cameras_use_collections = cameras_use_collections || (data && data->game_collection);
  }
  if (cameras_use_collections) {
    for (KX_GameObject *item : changed) {
      NoteDrawChanged(item);
    }
  }
}

void KX_Scene::SetCurrentGPUViewport(blender::GPUViewport *viewport)
{
  m_currentGPUViewport = viewport;
}

blender::GPUViewport *KX_Scene::GetCurrentGPUViewport()
{
  return m_currentGPUViewport;
}

void KX_Scene::SetOverlayCamera(KX_Camera *cam)
{
  m_overlayCamera = cam;
}

KX_Camera *KX_Scene::GetOverlayCamera()
{
  return m_overlayCamera;
}

bool KX_Scene::OverlayCameraMatches(const KX_Camera *camera) const
{
  return camera != nullptr && (camera == m_overlayCamera || camera == m_overlayCameraSource);
}

KX_Camera *KX_Scene::GetOverlayCameraSource()
{
  return m_overlayCameraSource;
}

KX_Camera *KX_Scene::ResolveOverlayRequestCamera(KX_Camera *requested) const
{
  if (requested != nullptr) {
    return requested;
  }
  return m_overlayCameraSource;
}

void KX_Scene::ApplyOverlayResourceVisibility(blender::Collection *collection)
{
  if (collection == nullptr || !m_overlayResources.Has(collection)) {
    return;
  }
  const bool presentation_visible = m_overlayResources.PresentationRequested(collection);
  const bool independent = m_overlayResources.IndependentEnabled(collection);
  /* All of the collection's objects: replicas of hidden templates, objects shown from Disable in
   * Renders and plain visible objects. Menu coverage uses its own layer, so hiding a covered menu
   * and showing it again never undoes a Set Visible that game logic applied to one of them. */
  std::vector<KX_GameObject *> objects;
  AppendOverlayRuntimeObjectsInCollection(collection, objects);
  for (KX_GameObject *game_object : objects) {
    ApplyRuntimeCollectionRenderVisibility(game_object);
    /* A player's copy of a menu shows and hides with that player's menu. Set Overlay Collection
     * shows copy 0 (the overlay's own) whatever covers that menu. */
    bool visible = presentation_visible;
    const int32_t instance = GetOverlayInstanceOf(game_object);
    const auto lease = m_overlayInstanceLease.find({collection, instance});
    if (lease != m_overlayInstanceLease.end() &&
        m_overlayResources.HasLease(collection, lease->second))
    {
      visible = (independent && instance == 0) ||
                m_overlayResources.LeaseVisible(collection, lease->second);
    }
    game_object->SetPresentationVisible(visible);
  }
}

void KX_Scene::ApplyAllOverlayResourceVisibility()
{
  for (blender::Collection *collection : m_overlay_collections) {
    ApplyOverlayResourceVisibility(collection);
  }
}

KX_OverlayCollectionResult KX_Scene::AcquireOverlayMenuLease(
    KX_Camera *overlay_cam,
    blender::Collection *collection,
    const int32_t menu_handle,
    const bool visible)
{
  if (menu_handle == 0) {
    return KX_OverlayCollectionResult::RuntimeUnavailable;
  }
  if (m_overlayResources.HasLease(collection, menu_handle)) {
    m_overlayResources.SetLeaseVisible(collection, menu_handle, visible);
    ApplyOverlayResourceVisibility(collection);
    return KX_OverlayCollectionResult::NoChange;
  }
  overlay_cam = ResolveOverlayRequestCamera(overlay_cam);
  if (!m_overlayResources.Has(collection)) {
    const KX_OverlayCollectionResult created = AddOverlayCollection(overlay_cam, collection, false);
    if (!KX_OverlayCollectionResultSucceeded(created)) {
      return created;
    }
    m_overlayResources.AddLease(collection, menu_handle, visible);
    /* AddOverlayCollection enables the independent request. A menu lease must not inherit that. */
    m_overlayResources.SetIndependentEnabled(collection, false);
    ApplyOverlayResourceVisibility(collection);
    return KX_OverlayCollectionResult::Success;
  }
  else {
    const KX_OverlayCollectionResult validation = ValidateAddOverlayCollection(overlay_cam,
                                                                               collection);
    if (validation != KX_OverlayCollectionResult::NoChange &&
        !KX_OverlayCollectionResultSucceeded(validation))
    {
      return validation;
    }
  }
  if (!m_overlayResources.Has(collection)) {
    return KX_OverlayCollectionResult::RuntimeUnavailable;
  }
  m_overlayResources.AddLease(collection, menu_handle, visible);
  ApplyOverlayResourceVisibility(collection);
  return KX_OverlayCollectionResult::Success;
}

KX_OverlayCollectionResult KX_Scene::ReleaseOverlayMenuLease(
    blender::Collection *collection, const int32_t menu_handle)
{
  if (!m_overlayResources.HasLease(collection, menu_handle)) {
    return KX_OverlayCollectionResult::NoChange;
  }
  m_overlayResources.RemoveLease(collection, menu_handle);
  if (m_overlayResources.ShouldExist(collection)) {
    ApplyOverlayResourceVisibility(collection);
    return KX_OverlayCollectionResult::Success;
  }
  return RemoveOverlayCollection(collection, false, false);
}

void KX_Scene::SetOverlayMenuLeaseVisible(
    blender::Collection *collection, const int32_t menu_handle, const bool visible)
{
  if (!m_overlayResources.HasLease(collection, menu_handle)) {
    return;
  }
  m_overlayResources.SetLeaseVisible(collection, menu_handle, visible);
  ApplyOverlayResourceVisibility(collection);
}

uint64_t KX_Scene::GetOverlayMembershipGeneration(const blender::Collection *collection) const
{
  return m_overlayResources.MembershipGeneration(collection);
}

void KX_Scene::NoteOverlayMembershipChanged(blender::Collection *collection)
{
  m_overlayResources.NoteMembershipChanged(collection);
}

bool KX_Scene::OverlayWouldExistWithoutLease(const blender::Collection *collection,
                                             const int32_t menu_handle) const
{
  return m_overlayResources.WouldExistWithoutLease(collection, menu_handle);
}

void KX_Scene::AddImageRenderCamera(KX_Camera *cam)
{
  /* Keep duplicate entries: multiple ImageRender owners may legitimately share one camera, and
   * each destructor removes exactly one registration. */
  if (cam != nullptr) {
    m_imageRenderCameraList.push_back(cam);
    /* Its renderer is then never shared nor given back (KX_Camera::m_usedForImageRender). */
    cam->SetUsedForImageRender();
  }
}

void KX_Scene::RemoveImageRenderCamera(KX_Camera *cam)
{
  const auto camera_iter = std::find(
      m_imageRenderCameraList.begin(), m_imageRenderCameraList.end(), cam);
  if (camera_iter != m_imageRenderCameraList.end()) {
    m_imageRenderCameraList.erase(camera_iter);
  }
}

bool KX_Scene::CameraIsInactive(KX_Camera *cam)
{
  if (cam == nullptr) {
    return false;
  }
  if (cam->GetViewport()) {
    return false;
  }
  if (cam == GetActiveCamera()) {
    return false;
  }
  if (std::find(m_imageRenderCameraList.begin(), m_imageRenderCameraList.end(), cam) !=
      m_imageRenderCameraList.end())
  {
    return false;
  }
  return true;
}

static RAS_Rasterizer::FrameBufferType r = RAS_Rasterizer::RAS_FRAMEBUFFER_FILTER0;
static RAS_Rasterizer::FrameBufferType s = RAS_Rasterizer::RAS_FRAMEBUFFER_EYE_LEFT0;

void KX_Scene::PrepareGPUViewport(KX_Camera *cam,
                                  const KX_CameraRenderMemory::PassSettings &settings)
{
  bool useViewportRender = KX_GetActiveEngine()->UseViewportRender();
  if (useViewportRender) {  // Custom bge render loop only
    return;
  }
  const double now = BLI_time_now_seconds();
  /* A pass that keeps no history draws through a renderer shared with the passes of equal
   * settings (two minimaps of the same size): one EEVEE world lighting, one set of buffers. */
  if (KX_CameraRenderMemory::CanShareRenderer(settings)) {
    for (SharedGPUViewport &shared : m_sharedGPUViewports) {
      if (shared.settings == settings) {
        shared.last_use = now;
        SetCurrentGPUViewport(shared.viewport);
        return;
      }
    }
    m_sharedGPUViewports.push_back({settings, GPU_viewport_create(), now});
    SetCurrentGPUViewport(m_sharedGPUViewports.back().viewport);
    return;
  }
  cam->MarkGPUViewportUsed(now);
  SetCurrentGPUViewport(cam->GetGPUViewport());
}

void KX_Scene::FreeIdleGPUViewports(const double now,
                                    const std::set<blender::GPUViewport *> &in_use)
{
  const auto idle = [&](const double last_use) {
    return now - last_use > KX_CameraRenderMemory::IdleSeconds;
  };
  for (KX_Camera *cam : m_cameralist) {
    blender::GPUViewport *viewport = cam->GetExistingGPUViewport();
    /* A texture can point at a render-to-texture camera's color buffer for as long as it
     * lives. */
    if (viewport == nullptr || cam->IsUsedForImageRender() || in_use.count(viewport) ||
        !idle(cam->GetGPUViewportLastUse()))
    {
      continue;
    }
    GPU_viewport_free(viewport);
    cam->m_gpuViewport = nullptr;
  }
  std::erase_if(m_sharedGPUViewports, [&](const SharedGPUViewport &shared) {
    if (in_use.count(shared.viewport) || !idle(shared.last_use)) {
      return false;
    }
    GPU_viewport_free(shared.viewport);
    return true;
  });
}

void KX_Scene::FreeSharedGPUViewports()
{
  for (const SharedGPUViewport &shared : m_sharedGPUViewports) {
    if (m_currentGPUViewport == shared.viewport) {
      m_currentGPUViewport = nullptr;
    }
    GPU_viewport_free(shared.viewport);
  }
  m_sharedGPUViewports.clear();
}

void KX_Scene::UpdateDepsgraph(blender::Main *bmain,
                               blender::Scene *scene,
                               bool is_overlay_pass,
                               KX_Camera *cam)
{
  RetireDrawHiddenObjects(KX_GetActiveEngine()->GetRenderFrame());
  RetireParkedCopies(KX_GetActiveEngine()->GetRenderFrame());

  const bool relations_changed = m_collectionRemap || m_relationsUpdatePending;
  if (m_collectionRemap) {
    /* check 68589a31ebfb79165f99a979357d237e5413e904 for potential issue or improvement? */
    /* If problem with ReplicateBlenderObject, see other occurrences of
     * BKE_collection_object_add_from*/
    BKE_main_collection_sync_remap(bmain);
    m_collectionRemap = false;
  }

  if (m_relationsUpdatePending) {
    DEG_relations_tag_update(bmain);
    m_relationsUpdatePending = false;
  }
  if (relations_changed) {
    ++m_renderSyncRelations;
  }

  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  blender::bContext *C = engine->GetContext();
  blender::Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  const uint64_t pass_start_update = DEG_get_update_count(depsgraph);

  /* Looked up once a pass: per object, Blender's context lookups cost more than the compare. */
  const KX_GameObject::RenderSyncPass sync = {bmain,
                                              depsgraph,
                                              CTX_data_scene(C),
                                              engine->GetRenderFrame(),
                                              ++m_renderSyncPasses,
                                              pass_start_update,
                                              m_renderSyncRelations};
  m_directPoseObjects.clear();
  m_poseCheckObjects.clear();
  const bool blender_physics = (scene->gm.flag & (GAME_USE_INTERACTIVE_DYNAPAINT |
                                                  GAME_USE_INTERACTIVE_RIGIDBODY)) != 0;
  for (KX_GameObject *gameobj : GetObjectList()) {
    if (blender_physics) {
      TagBlenderPhysicsObject(scene, gameobj->GetBlenderObject());
    }
    gameobj->TagForTransformUpdate(sync, is_overlay_pass);
  }

  /* Direct moves are written after the update, but the renderers only look again when Blender's
   * update count moves on: one shading mark on one of them does it (its shading stamp; nothing is
   * evaluated again). An object outside Blender's graph (hidden) would not. */
  for (KX_GameObject *gameobj : m_directPoseObjects) {
    blender::Object *ob = gameobj->GetBlenderObject();
    if (DEG_get_evaluated(depsgraph, ob) != ob) {
      DEG_id_tag_update(&ob->id, ID_RECALC_SHADING);
      break;
    }
  }

  TagForExtraIdsUpdate(bmain, cam);

  /* Every update that starts tasks wakes all of Blender's worker threads, which spin about a
   * millisecond after their work: a small update runs on this thread alone (Blender's own
   * single-thread evaluation), a slow one goes back to the workers (KX_DepsgraphThreads). */
  const bool use_workers = m_depsgraphThreads.UseWorkers();
  const bool single_thread = !use_workers && (G.debug & G_DEBUG_DEPSGRAPH_NO_THREADS) == 0;
  if (single_thread) {
    G.debug |= G_DEBUG_DEPSGRAPH_NO_THREADS;
  }
  const uint64_t tagged_update = DEG_get_update_count(depsgraph);
  const double start = BLI_time_now_seconds();
  BKE_scene_graph_update_tagged(depsgraph, bmain);
  const double milliseconds = (BLI_time_now_seconds() - start) * 1000.0;
  if (single_thread) {
    G.debug &= ~G_DEBUG_DEPSGRAPH_NO_THREADS;
  }
  if (DEG_get_update_count(depsgraph) != tagged_update) {
    m_depsgraphThreads.Record(use_workers, milliseconds);
  }

  for (KX_GameObject *gameobj : m_directPoseObjects) {
    gameobj->WriteDirectPose(depsgraph);
  }

  /* The drawn poses go into the objects Blender evaluated in this pass, or whose pose changed;
   * the others keep the pose an earlier pass wrote. Objects that take their pose from Blender
   * still give it to the game: logic or physics may have moved them since. Blender evaluates an
   * object nothing poses but its original from that original, which holds the drawn pose, so
   * only the pass's moved objects and those Blender poses from other things are looked at; all of
   * them after a relations rebuild (it re-evaluates everything; looking up every object cost a
   * quarter of the update among 10,000). */
  const bool evaluated = DEG_get_update_count(depsgraph) != pass_start_update;
  if (relations_changed) {
    for (KX_GameObject *gameobj : GetObjectList()) {
      gameobj->TagForTransformUpdateEvaluated(depsgraph, evaluated, pass_start_update);
    }
  }
  else {
    for (KX_GameObject *gameobj : m_poseCheckObjects) {
      gameobj->TagForTransformUpdateEvaluated(depsgraph, evaluated, pass_start_update);
    }
  }
}

bool KX_Scene::ViewportRender(KX_Camera *cam,
                              const RAS_Rect &viewport,
                              const blender::rcti &window,
                              RAS_ICanvas *canvas,
                              blender::Scene *scene,
                              blender::bContext *C)
{
  bool useViewportRender = KX_GetActiveEngine()->UseViewportRender();
  if (useViewportRender) {
    /* Viewport render mode doesn't support several render passes then exit here
     * if we are trying to use not supported features. */
    const std::vector<KX_Camera *> rendering_cameras = KX_GetActiveEngine()->GetRenderingCameras();
    if (cam && !rendering_cameras.empty() && cam != rendering_cameras.front()) {
      std::cout << "Warning: Viewport Render mode doesn't support multiple render passes"
                << std::endl;
      return true;
    }

    /* Don't need any background framebuffer as everything will be redrawn */
    GPU_framebuffer_restore();

    if (cam) {
      if (G.is_blenderplayer) {
        blender::ARegion *region = CTX_wm_region(C);
        scene->flag |= SCE_IS_BLENDERPLAYER;
        region->winrct = window;
        region->winx = canvas->GetWidth();
        region->winy = canvas->GetHeight();
        /* Force camera projection matrix to be the same as viewport one (for mouse events) */
        cam->SetProjectionMatrix(MT_Matrix4x4(&CTX_wm_region_view3d(C)->winmat[0][0]));
      }

      CTX_wm_view3d(C)->camera = cam->GetBlenderObject();

#ifdef WITH_XR_OPENXR
      blender::wmWindowManager *wm = CTX_wm_manager(C);
      if (WM_xr_session_exists(&wm->xr)) {
        if (WM_xr_session_is_ready(&wm->xr)) {
          wm_xr_events_handle(CTX_wm_manager(C));
          // wm_event_do_handlers(C);   // TODO: Find more specific XR code
          wm_event_do_notifiers(C);  // TODO: Find more specific XR code
        }
      }
#endif
      CTX_wm_region(C)->runtime->visible = true;
      ED_region_tag_redraw(CTX_wm_region(C));
      wm_draw_update(C);

      /* We need to do that before and after wm_draw_update
       * because wm_draw_update unset context variables.
       * We might need these variables at next logic step
       */
      ReinitBlenderContextVariables();

      if (G.is_blenderplayer) {
        scene->flag &= ~SCE_IS_BLENDERPLAYER;
      }

      return true;
    }
  }
  return false;
}

void KX_Scene::RenderAfterCameraSetup(KX_Camera *cam,
                                      RAS_FrameBuffer *background_fb,
                                      const RAS_Rect &viewport,
                                      bool is_overlay_pass,
                                      bool /*is_last_render_pass*/)
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  RAS_Rasterizer *rasty = engine->GetRasterizer();
  RAS_ICanvas *canvas = engine->GetCanvas();
  blender::bContext *C = engine->GetContext();
  blender::Main *bmain = CTX_data_main(C);
  blender::Scene *scene = GetBlenderScene();
  /* This ensures a depsgraph is allocated and activates it */
  blender::Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);

  engine->CountDepsgraphTime();
  UpdateDepsgraph(bmain, scene, is_overlay_pass, cam);
  engine->EndCountDepsgraphTime();

  blender::rcti window;
  int v[4];
  /* Custom BGE viewports*/
  if (cam && cam->GetViewport() && cam != GetOverlayCamera()) {
    v[0] = viewport.GetLeft();
    v[1] = viewport.GetBottom();
    v[2] = viewport.GetWidth() + 1;
    v[3] = viewport.GetHeight() + 1;
    window = {0, viewport.GetWidth(), 0, viewport.GetHeight()};
  }
  /* blender::Main cam (when it has no custom viewport), overlay cam */
  else {
    v[0] = 0;
    v[1] = 0;
    v[2] = canvas->GetWidth() + 1;
    v[3] = canvas->GetHeight() + 1;
    window = {0, canvas->GetWidth(), 0, canvas->GetHeight()};
  }

  /* What the camera draws this pass with (its switches, Use Workbench, the shadow pool for its
   * share of the window), and the renderer it draws through. */
  const int64_t window_pixels = int64_t(canvas->GetWidth() + 1) * int64_t(canvas->GetHeight() + 1);
  const KX_CameraRenderMemory::PassSettings pass_settings = CameraPassSettings(
      cam, is_overlay_pass, v[2], v[3], window_pixels);
  PrepareGPUViewport(cam, pass_settings);

  /* When we call wm_draw_update, blender::bContext variables are unset,
   * then we need to set it again correctly to render the next frame.
   * wm_draw_update can also be called when playing dragging or resizing
   * blender window */
  ReinitBlenderContextVariables();

  /* Viewport Render draws the first camera only (ViewportRender skips the others, such as the
   * menu camera): a flush for a skipped camera starts a debug buffer that is never drawn, so
   * never freed (one leak per frame). */
  const std::vector<KX_Camera *> rendering_cameras = engine->UseViewportRender() ?
                                                         engine->GetRenderingCameras() :
                                                         std::vector<KX_Camera *>();
  if (engine->UseViewportRender() &&
      (cam == nullptr || rendering_cameras.empty() || cam == rendering_cameras.front()))
  {
    /* Blender viewport overlays are drawn inside wm_draw_update. Flush BGE debug geometry before
     * that call so current-frame logic debug lines do not appear one viewport draw late. Fixed
     * simulation geometry is retained per scene and sampled by every world-camera render; the
     * dedicated overlay pass stays isolated from gameplay debug geometry. */
    rasty->FlushDebugDraw(canvas,
                          is_overlay_pass ? RAS_DebugDraw::FixedSnapshotMode::None :
                                            RAS_DebugDraw::FixedSnapshotMode::Context,
                          is_overlay_pass ? nullptr : this);
  }

  /* Here we'll render directly the scene with viewport code. */
  if (ViewportRender(cam, viewport, window, canvas, scene, C)) {
    return;
  }

  /* Custom bge render loop only from here */
  if (cam) {
    float winmat[4][4];
    cam->GetProjectionMatrix().getValue(&winmat[0][0]);
    /* Only a 3D view in camera view looks through v3d->camera; otherwise it draws its own view.
     * Give it this camera's view, or an overlay (menu) camera would draw the editor's view when
     * the game plays from the viewport. */
    float viewmat[4][4];
    cam->GetModelviewMatrix().getValue(&viewmat[0][0]);
    const bool camera_view = CTX_wm_region_view3d(C)->persp == RV3D_CAMOB;
    CTX_wm_view3d(C)->camera = cam->GetBlenderObject();
    ED_view3d_draw_setup_view(CTX_wm_manager(C),
                              CTX_wm_window(C),
                              CTX_data_expect_evaluated_depsgraph(C),
                              CTX_data_scene(C),
                              CTX_wm_region(C),
                              CTX_wm_view3d(C),
                              camera_view ? NULL : viewmat,
                              winmat,
                              NULL);

    UpdateObjectLods(cam);
  }

  /* The effects this camera (or Split Screen Quality) turns off, Use Workbench, the shadow pool. */
  const bool one_sample = DisableCameraEffects(depsgraph, cam, is_overlay_pass, pass_settings);
  /* The objects this camera does not draw (Set Camera Visibility, its Collection setting, draw
   * culling). */
  HideCameraHiddenObjects(depsgraph, cam, pass_settings.workbench, is_overlay_pass);
  /* The copies drawn as groups: their own draw entries leave them out until the draw ends. */
  KX_Scene *copies_scene = dupli_copies_scene(KX_GetActiveEngine(), depsgraph);
  blender::Span<blender::DRWGameInstanceGroup> instance_groups;
  if (copies_scene != nullptr) {
    copies_scene->BeginDupliDrawGroups(depsgraph, is_overlay_pass, pass_settings.workbench);
    instance_groups = copies_scene->GetDupliDrawGroups().groups.as_span();
  }

  short samples_per_frame = min_ii(scene->gm.samples_per_frame, scene->eevee.taa_samples);
  samples_per_frame = one_sample ? 1 : max_ii(samples_per_frame, 1);

  if (KX_GetActiveEngine()->UseViewportRender() &&
      KX_GetActiveEngine()->IsPhysicsInterpolationEnabled())
  {
    samples_per_frame = 1;
  }

  for (short i = 0; i < samples_per_frame; i++) {
    if (background_fb) {
      GPU_framebuffer_bind(background_fb->GetFrameBuffer());
      GPU_framebuffer_clear_depth(background_fb->GetFrameBuffer(), 1.0f);
      GPU_framebuffer_restore();
    }
    /* Draw custom viewport render loop into its own blender::GPUViewport */
    DRW_game_render_loop(C,
                         m_currentGPUViewport,
                         depsgraph,
                         &window,
                         is_overlay_pass,
                         pass_settings.workbench,
                         instance_groups);
  }
  if (copies_scene != nullptr) {
    copies_scene->EndDupliDrawGroups();
  }
  /* The camera's overrides end with its draw: a render to texture drawn later gets the scene's
   * own settings. */
  restore_camera_effects(*DEG_get_evaluated_scene(depsgraph), *scene);
  RestoreCameraHiddenObjects();

  /* FILTER1 borrows the view's own color and depth (it has no textures of its own). */
  RAS_FrameBuffer *input = rasty->GetFrameBuffer(rasty->NextFilterFrameBuffer(r));

  blender::gpu::Texture *color = GPU_viewport_color_texture(m_currentGPUViewport, 0);
  GPUAttachment config[] = {
      GPU_ATTACHMENT_TEXTURE(GPU_viewport_depth_texture(m_currentGPUViewport)),
      GPU_ATTACHMENT_TEXTURE(color)};

  GPU_framebuffer_config_array(
      input->GetFrameBuffer(), config, sizeof(config) / sizeof(GPUAttachment));

  /* A view pass without 2D filters used to copy its picture into the "output" buffer (cleared to
   * opaque black), then that into the background. Drawn straight into the background, it gives
   * the same pixels without the output buffer: a window-size color and depth pair that was also
   * re-created for every camera of another size, every frame. With no blending the copies were
   * exact; with the premultiplied blending of the other cameras' views (KX_KetsjiEngine::
   * RenderCamera) they gave the picture's color with alpha 1, which FillOpaqueAlpha gives. */
  const GPUBlend blend = GPU_blend_get();
  const bool view_pass = !is_overlay_pass && background_fb;
  const bool draw_directly = view_pass && !m_filterManager->HasFilters() &&
                             (blend == GPU_BLEND_NONE || blend == GPU_BLEND_ALPHA_PREMULT);

  RAS_FrameBuffer *f = input;
  if (view_pass && !draw_directly) {
    RAS_FrameBuffer *output = rasty->GetFrameBuffer(rasty->NextRenderFrameBuffer(s));
    output->UpdateSize(GPU_texture_width(color), GPU_texture_height(color));

    /* Clear output framebuffer to ensure it has no color from previous pass.
     * (was causing troubles in Vulkan + custom bge viewports) */
    GPU_framebuffer_bind(output->GetFrameBuffer());
    GPU_framebuffer_clear_color_depth(output->GetFrameBuffer(), {0.0, 0.0, 0.0, 1.0}, 1.0f);
    GPU_framebuffer_restore();

    /* Draw 2D filters */
    f = Render2DFilters(rasty, canvas, input, output);
  }

  GPU_framebuffer_restore();

  if (background_fb) {
    /* Draw this camera render into background framebuffer */
    GPU_framebuffer_bind(background_fb->GetFrameBuffer());
    GPU_viewport(v[0], v[1], v[2], v[3]);
    GPU_scissor_test(true);
    GPU_scissor(v[0], v[1], v[2], v[3]);
    /* A round view (Viewport Shape: Circle) keeps only the circle filling its viewport, with the
     * same pixels inside as the rectangle's paste; its corners show what is behind. Without its
     * shader it is drawn whole. */
    const bool round_view = view_pass && cam && cam->HasCircleViewport() &&
                            cam != GetOverlayCamera();
    if (round_view && rasty->DrawFrameBufferCircle(f)) {
      /* Drawn. */
    }
    else if (draw_directly && blend == GPU_BLEND_ALPHA_PREMULT) {
      GPU_blend(GPU_BLEND_NONE);
      rasty->DrawFrameBuffer(input, background_fb);
      rasty->FillOpaqueAlpha();
      GPU_blend(blend);
    }
    else {
      rasty->DrawFrameBuffer(f, background_fb);
    }
  }

  GPU_framebuffer_restore();

  GPU_blend(GPU_BLEND_NONE);
}

void KX_Scene::RenderAfterCameraSetupImageRender(KX_Camera *cam, const blender::rcti *window)
{
  blender::bContext *C = KX_GetActiveEngine()->GetContext();
  blender::Depsgraph *depsgraph = CTX_data_depsgraph_on_load(C);

  if (!depsgraph) {
    return;
  }

  float winmat[4][4];
  cam->GetProjectionMatrix().getValue(&winmat[0][0]);
  CTX_wm_view3d(C)->camera = cam->GetBlenderObject();
  ED_view3d_draw_setup_view(CTX_wm_manager(C),
                            CTX_wm_window(C),
                            CTX_data_expect_evaluated_depsgraph(C),
                            CTX_data_scene(C),
                            CTX_wm_region(C),
                            CTX_wm_view3d(C),
                            NULL,
                            winmat,
                            NULL);

  /* The camera's own effect switches (a mirror or security camera texture), and Use Workbench.
   * A texture is no share of the window: the scene's whole shadow pool. */
  const KX_CameraRenderMemory::PassSettings settings = CameraPassSettings(cam, false, 0, 0, 0);
  DisableCameraEffects(depsgraph, cam, false, settings);
  HideCameraHiddenObjects(depsgraph, cam, settings.workbench);
  KX_Scene *copies_scene = dupli_copies_scene(KX_GetActiveEngine(), depsgraph);
  blender::Span<blender::DRWGameInstanceGroup> instance_groups;
  if (copies_scene != nullptr) {
    copies_scene->BeginDupliDrawGroups(depsgraph, false, settings.workbench);
    instance_groups = copies_scene->GetDupliDrawGroups().groups.as_span();
  }
  DRW_game_render_loop(
      C, cam->GetGPUViewport(), depsgraph, window, false, settings.workbench, instance_groups);
  if (copies_scene != nullptr) {
    copies_scene->EndDupliDrawGroups();
  }
  restore_camera_effects(*DEG_get_evaluated_scene(depsgraph), *GetBlenderScene());
  RestoreCameraHiddenObjects();
}

void KX_Scene::SetBlenderSceneConverter(BL_SceneConverter *sc_converter)
{
  m_sceneConverter = sc_converter;
}

BL_SceneConverter *KX_Scene::GetBlenderSceneConverter()
{
  return m_sceneConverter;
}

void KX_Scene::ConvertBlenderObject(blender::Object *ob)
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  e_PhysicsEngine physics_engine = static_cast<e_PhysicsEngine>(
      GetBlenderScene()->gm.physicsEngine);
  RAS_Rasterizer *rasty = engine->GetRasterizer();
  RAS_ICanvas *canvas = engine->GetCanvas();
  blender::bContext *C = engine->GetContext();
  blender::Depsgraph *depsgraph = CTX_data_expect_evaluated_depsgraph(C);
  blender::Main *bmain = CTX_data_main(C);
  BL_ConvertBlenderObjects(bmain,
                           depsgraph,
                           this,
                           engine,
                           physics_engine,
                           rasty,
                           canvas,
                           m_sceneConverter,
                           ob,
                           false,
                           false);
}

void KX_Scene::convert_blender_objects_list_synchronous(std::vector<blender::Object *> objectslist)
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  e_PhysicsEngine physics_engine = static_cast<e_PhysicsEngine>(
      GetBlenderScene()->gm.physicsEngine);
  RAS_Rasterizer *rasty = engine->GetRasterizer();
  RAS_ICanvas *canvas = engine->GetCanvas();
  blender::bContext *C = engine->GetContext();
  blender::Depsgraph *depsgraph = CTX_data_expect_evaluated_depsgraph(C);
  blender::Main *bmain = CTX_data_main(C);

  for (blender::Object *obj : objectslist) {
    BL_ConvertBlenderObjects(bmain,
                             depsgraph,
                             this,
                             engine,
                             physics_engine,
                             rasty,
                             canvas,
                             m_sceneConverter,
                             obj,
                             false,
                             false);
  }
}

// Task data for convertBlenderCollection in a different thread.
struct ConvertBlenderObjectsListTaskData {
  std::vector<blender::Object *> objectslist;
  KX_KetsjiEngine *engine;
  e_PhysicsEngine physics_engine;
  KX_Scene *kxscene;
  BL_SceneConverter *converter;
  RAS_Rasterizer *rasty;
  RAS_ICanvas *canvas;
  blender::Depsgraph *depsgraph;
  blender::Main *bmain;
};

static void convert_blender_objects_list_thread_func(blender::TaskPool *__restrict pool,
                                                     void *taskdata)
{
  ConvertBlenderObjectsListTaskData *task = static_cast<ConvertBlenderObjectsListTaskData *>(
      taskdata);

  for (blender::Object *obj : task->objectslist) {
    BL_ConvertBlenderObjects(task->bmain,
                             task->depsgraph,
                             task->kxscene,
                             task->engine,
                             task->physics_engine,
                             task->rasty,
                             task->canvas,
                             task->converter,
                             obj,
                             false,
                             false);
  }
}

void KX_Scene::ConvertBlenderObjectsList(std::vector<blender::Object *> objectslist,
                                         bool asynchronous)
{
#ifdef WITH_GAMEENGINE_LOGICNODES
  if (asynchronous) {
    CM_Warning("Logic Nodes require main-thread object conversion; running synchronously.");
    asynchronous = false;
  }
#endif

  if (asynchronous) {
    /* Convert the Blender Objects list in a different thread, so that the
     * game engine can keep running at full speed. */
    ConvertBlenderObjectsListTaskData task;
    task.engine = KX_GetActiveEngine();
    task.physics_engine = static_cast<e_PhysicsEngine>(GetBlenderScene()->gm.physicsEngine);
    task.kxscene = this;
    task.converter = m_sceneConverter;
    task.rasty = task.engine->GetRasterizer();
    task.canvas = task.engine->GetCanvas();
    blender::bContext *C = task.engine->GetContext();
    task.depsgraph = CTX_data_expect_evaluated_depsgraph(C);
    task.bmain = CTX_data_main(C);
    task.objectslist = objectslist;

    blender::TaskPool *taskpool = BLI_task_pool_create(&task, TASK_PRIORITY_LOW);

    BLI_task_pool_push(
        taskpool,
        convert_blender_objects_list_thread_func,
        &task,
        false,  // We will clean the objectslist std::vector of pointers ourself later
        NULL);
    BLI_task_pool_work_and_wait(taskpool);

    /* delete the objectslist ourself as it gives error if the work
     * has to do it the BLI_task_pool_work_and_wait function */
    while (!task.objectslist.size() != 0) {
      blender::Object *temp = task.objectslist.back();
      task.objectslist.pop_back();
      delete temp;
    }

    BLI_task_pool_free(taskpool);
    taskpool = nullptr;
  }
  else {
    convert_blender_objects_list_synchronous(objectslist);
  }
  /* Objects converted while playing may come with collections the index doesn't know. */
  NoteCollectionsChanged();
}

void KX_Scene::convert_blender_collection_synchronous(blender::Collection *co)
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  e_PhysicsEngine physics_engine = static_cast<e_PhysicsEngine>(
      GetBlenderScene()->gm.physicsEngine);
  RAS_Rasterizer *rasty = engine->GetRasterizer();
  RAS_ICanvas *canvas = engine->GetCanvas();
  blender::bContext *C = engine->GetContext();
  blender::Depsgraph *depsgraph = CTX_data_expect_evaluated_depsgraph(C);
  blender::Main *bmain = CTX_data_main(C);

  FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (co, obj) {
    BL_ConvertBlenderObjects(bmain,
                             depsgraph,
                             this,
                             engine,
                             physics_engine,
                             rasty,
                             canvas,
                             m_sceneConverter,
                             obj,
                             false,
                             false);
  }
  FOREACH_COLLECTION_OBJECT_RECURSIVE_END;
}

// Task data for convertBlenderCollection in a different thread.
struct ConvertBlenderCollectionTaskData {
  blender::Collection *co;
  KX_KetsjiEngine *engine;
  e_PhysicsEngine physics_engine;
  KX_Scene *kxscene;
  BL_SceneConverter *converter;
  RAS_Rasterizer *rasty;
  RAS_ICanvas *canvas;
  blender::Depsgraph *depsgraph;
  blender::Main *bmain;
};

static void convert_blender_collection_thread_func(blender::TaskPool *__restrict pool,
                                                   void *taskdata)
{
  ConvertBlenderCollectionTaskData *task = static_cast<ConvertBlenderCollectionTaskData *>(
      taskdata);

  FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (task->co, obj) {
    BL_ConvertBlenderObjects(task->bmain,
                             task->depsgraph,
                             task->kxscene,
                             task->engine,
                             task->physics_engine,
                             task->rasty,
                             task->canvas,
                             task->converter,
                             obj,
                             false,
                             false);
  }
  FOREACH_COLLECTION_OBJECT_RECURSIVE_END;
}

void KX_Scene::ConvertBlenderCollection(blender::Collection *co, bool asynchronous)
{
#ifdef WITH_GAMEENGINE_LOGICNODES
  if (asynchronous) {
    CM_Warning("Logic Nodes require main-thread collection conversion; running synchronously.");
    asynchronous = false;
  }
#endif

  if (asynchronous) {

    /* Convert the Blender collection in a different thread, so that the
     * game engine can keep running at full speed. */
    ConvertBlenderCollectionTaskData task;

    task.engine = KX_GetActiveEngine();
    task.physics_engine = static_cast<e_PhysicsEngine>(GetBlenderScene()->gm.physicsEngine);
    task.co = co;
    task.kxscene = this;
    task.converter = m_sceneConverter;
    task.rasty = task.engine->GetRasterizer();
    task.canvas = task.engine->GetCanvas();
    blender::bContext *C = task.engine->GetContext();
    task.depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
    task.bmain = CTX_data_main(C);

    blender::TaskPool *taskpool = BLI_task_pool_create(&task, TASK_PRIORITY_LOW);

    BLI_task_pool_push(taskpool, convert_blender_collection_thread_func, &task, false, NULL);
    BLI_task_pool_work_and_wait(taskpool);
    BLI_task_pool_free(taskpool);
    taskpool = nullptr;
  }
  else {
    convert_blender_collection_synchronous(co);
  }
  /* Objects converted while playing may come with collections the index doesn't know. */
  NoteCollectionsChanged();
}

void KX_Scene::ConvertBlenderAction(blender::bAction *action)
{
  SCA_LogicManager *logicMgr = GetLogicManager();
  if (logicMgr) {
    if (!logicMgr->GetActionByName(action->id.name + 2)) {
      logicMgr->RegisterActionName(action->id.name + 2, (void *)action);
    }
  }
}

void KX_Scene::SetIsPythonMainLoop(bool isPythonMainLoop)
{
  m_isPythonMainLoop = isPythonMainLoop;
}

void KX_Scene::AddObjToLodObjList(KX_GameObject *gameobj)
{
  std::vector<KX_GameObject *>::iterator it = std::find(
      m_kxobWithLod.begin(), m_kxobWithLod.end(), gameobj);
  if (it == m_kxobWithLod.end()) {
    m_kxobWithLod.push_back(gameobj);
  }
}

void KX_Scene::RemoveObjFromLodObjList(KX_GameObject *gameobj)
{
  std::vector<KX_GameObject *>::iterator it = std::find(
      m_kxobWithLod.begin(), m_kxobWithLod.end(), gameobj);
  if (it != m_kxobWithLod.end()) {
    m_kxobWithLod.erase(it);
  }
}

void KX_Scene::BackupVisibilityFlag(blender::Object *ob, short visibilityFlag)
{
  m_obVisibilityFlag.insert({ob, visibilityFlag});
}

void KX_Scene::ForgetVisibilityFlag(blender::Object *ob)
{
  m_obVisibilityFlag.erase(ob);
}

bool KX_Scene::KeepDupliBaseInGraph(blender::Object *ob)
{
  using namespace blender;
  Main *bmain = CTX_data_main(KX_GetActiveEngine()->GetContext());
  Scene *scene = GetBlenderScene();
  ViewLayer *view_layer = scene ? BKE_view_layer_default_view(scene) : nullptr;
  if (bmain == nullptr || view_layer == nullptr) {
    return false;
  }
  const auto enabled_base = [&]() -> Base * {
    BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
    Base *base = BKE_view_layer_base_find(view_layer, ob);
    return (base != nullptr && (base->flag & BASE_ENABLED_VIEWPORT)) ? base : nullptr;
  };
  DupliBaseInGraph entry;
  entry.object = ob;
  /* Its own Disable in Viewports (its flags saved at its conversion come back after the game). */
  if (ob->visibility_flag & OB_HIDE_VIEWPORT) {
    ob->visibility_flag &= ~OB_HIDE_VIEWPORT;
    BKE_view_layer_need_resync_tag(view_layer);
  }
  Base *base = enabled_base();
  if (base == nullptr) {
    /* All its collections are disabled: it goes in the scene's own collection too, never
     * disabled (left out of the game's collection index, EnsureCollectionIndex). */
    entry.linked = BKE_collection_object_add(bmain, scene->master_collection, ob);
    BKE_main_collection_sync_remap(bmain);
    base = enabled_base();
    if (base == nullptr) {
      if (entry.linked) {
        BKE_collection_object_remove(bmain, scene->master_collection, ob, false);
        BKE_main_collection_sync_remap(bmain);
      }
      return false;
    }
  }
  if (!(base->flag & BASE_HIDDEN)) {
    base->flag |= BASE_HIDDEN;
    entry.eye_closed = true;
    BKE_view_layer_need_resync_tag(view_layer);
  }
  m_dupliBasesInGraph.push_back(entry);
  DEG_relations_tag_update(bmain);
  return true;
}

void KX_Scene::RestoreVisibilityFlag()
{
  blender::bContext *C = KX_GetActiveEngine()->GetContext();
  blender::Main *bmain = CTX_data_main(C);
  /* Merged scenes' objects leave this scene's collection (their eye here goes with them). */
  if (!m_mergedObjectLinks.empty()) {
    blender::Scene *scene = GetBlenderScene();
    for (blender::Object *ob : m_mergedObjectLinks) {
      BKE_collection_object_remove(bmain, scene->master_collection, ob, false);
    }
    m_mergedObjectLinks.clear();
    BKE_main_collection_sync_remap(bmain);
  }
  /* The Dupli Bases kept in Blender's graph get their collections and eye back. */
  if (!m_dupliBasesInGraph.empty()) {
    blender::Scene *scene = GetBlenderScene();
    blender::ViewLayer *view_layer = BKE_view_layer_default_view(scene);
    for (const DupliBaseInGraph &entry : m_dupliBasesInGraph) {
      if (entry.linked) {
        BKE_collection_object_remove(bmain, scene->master_collection, entry.object, false);
      }
    }
    BKE_main_collection_sync_remap(bmain);
    BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
    for (const DupliBaseInGraph &entry : m_dupliBasesInGraph) {
      blender::Base *base = BKE_view_layer_base_find(view_layer, entry.object);
      if (entry.eye_closed && base != nullptr) {
        base->flag &= ~BASE_HIDDEN;
      }
    }
    BKE_view_layer_need_resync_tag(view_layer);
    m_dupliBasesInGraph.clear();
  }
  for (std::map<blender::Object *, short>::iterator it = m_obVisibilityFlag.begin();
       it != m_obVisibilityFlag.end();
       it++)
  {
    blender::Object *ob = it->first;
    ob->visibility_flag = eObject_VisibilityFlag(it->second);
    DEG_id_tag_update(&ob->id, ID_RECALC_SYNC_TO_EVAL);
  }
  DEG_relations_tag_update(bmain);
  BKE_main_collection_sync_remap(bmain);
  m_obVisibilityFlag.clear();
}

void KX_Scene::TagForCollectionRemap()
{
  m_collectionRemap = true;
  m_relationsUpdatePending = true;
}

void KX_Scene::TagForRelationsUpdate()
{
  m_relationsUpdatePending = true;
}

KX_GameObject *KX_Scene::GetGameObjectFromObject(blender::Object *ob)
{
  return m_sceneConverter->FindGameObject(ob);
}

void KX_Scene::BackupObjectsMatToWorld(BackupObj *backup)
{
  m_backupObList.push_back(backup);
}

void KX_Scene::RestoreObjectsMatToWorld()
{
  for (BackupObj *backup : m_backupObList) {
    BKE_object_tfm_restore(backup->ob, backup->obtfm);
  }
}

bool KX_Scene::OrigObCanBeTransformedInRealtime(blender::Object *ob)
{
  FluidModifierData *fluidModifierData = (FluidModifierData *)BKE_modifiers_findby_type(
      ob, eModifierType_Fluid);
  if (fluidModifierData) {
    return false;
  }
  return true;
}

/* Look at object_transform for original function */
void KX_Scene::IgnoreParentTxBGE(blender::Main *bmain,
                                 blender::Depsgraph *depsgraph,
                                 blender::Scene *scene,
                                 blender::Object *ob,
                                 std::vector<blender::Object *> children)
{
  blender::Object *ob_child;

  blender::Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);

  /* a change was made, adjust the children to compensate */
  for (blender::Object *child : children) {
    if (child->parent == ob) {
      ob_child = child;
      blender::Object *ob_child_eval = DEG_get_evaluated(depsgraph, ob_child);
      BKE_object_apply_mat4(ob_child_eval, ob_child_eval->object_to_world().ptr(), true, false);
      invert_m4_m4(ob_child->parentinv,
                   BKE_object_calc_parent(depsgraph, scene, ob_child_eval).ptr());
      /* Copy result of BKE_object_apply_mat4(). */
      BKE_object_transform_copy(ob_child, ob_child_eval);
      /* Make sure evaluated object is in a consistent state with the original one.
       * It might be needed for applying transform on its children. */
      copy_m4_m4(ob_child_eval->parentinv, ob_child->parentinv);
      BKE_object_eval_transform_all(depsgraph, scene_eval, ob_child_eval);
      /* Tag for update.
       * This is because parent matrix did change, so in theory the child object might now be
       * evaluated to a different location in another editing context. */
      if (!OrigObCanBeTransformedInRealtime(ob_child)) {
        continue;
      }
      DEG_id_tag_update(&ob_child->id, ID_RECALC_TRANSFORM);
    }
  }
}

void KX_Scene::TagForObjectsMatToWorldRestore()
{
  for (BackupObj *backup : m_backupObList) {

    blender::Object *ob_orig = backup->ob;

    if (ob_orig) {

      bool applyTransformToOrig = OrigObCanBeTransformedInRealtime(ob_orig);

      if (applyTransformToOrig) {
        BKE_object_apply_mat4(ob_orig,
                              ob_orig->object_to_world().ptr(),
                              false,
                              ob_orig->parent && ob_orig->partype != PARVERT1);
      }

      if (applyTransformToOrig) {
        /* NORMAL CASE */
        if (ob_orig->type != OB_MBALL) {
          DEG_id_tag_update(&ob_orig->id, ID_RECALC_TRANSFORM);
        }
        /* SPECIAL CASE: EXPERIMENTAL -> TEST METABALLS (incomplete) (TODO restore elems position
         * at ge exit) */
        else if (ob_orig->type == OB_MBALL) {
          if (!BKE_mball_is_basis(ob_orig)) {
            DEG_id_tag_update(&ob_orig->id, ID_RECALC_GEOMETRY);
          }
          else {
            DEG_id_tag_update(&ob_orig->id, ID_RECALC_TRANSFORM);
          }
        }
      }
    }
    /* Free what was allocated in BlenderDataConversion */
    MEM_delete_void(backup->obtfm);
    delete backup;
  }
}

void KX_Scene::ReserveIdsToUpdate(const size_t additional_all_render_passes,
                                  const size_t additional_overlay_pass)
{
  if (additional_all_render_passes >
          m_idsToUpdateInAllRenderPasses.max_size() - m_idsToUpdateInAllRenderPasses.size() ||
      additional_overlay_pass >
          m_idsToUpdateInOverlayPass.max_size() - m_idsToUpdateInOverlayPass.size())
  {
    throw std::length_error("ID update reservation exceeds the scene limit");
  }
  m_idsToUpdateInAllRenderPasses.reserve(m_idsToUpdateInAllRenderPasses.size() +
                                         additional_all_render_passes);
  m_idsToUpdateInOverlayPass.reserve(m_idsToUpdateInOverlayPass.size() + additional_overlay_pass);
}

void KX_Scene::AppendToIdsToUpdate(blender::ID *id,
                                   IDRecalcFlag flag,
                                   bool in_overlay_collection_only)
{
  /* A grouped (Auto) object's own Blender object is out of the graph: a change of its own data
   * (geometry, materials, modifiers...) makes it leave its group, so its own object draws the
   * change; a redraw of its pose or shading redraws its group (a changed shared material, GPU
   * shape keys on the shared mesh). Every path that changes it this way is caught here, also
   * those that do not say so themselves. */
  if (!m_sharedMemberOfObject.empty() && GS(id->name) == ID_OB) {
    auto member = m_sharedMemberOfObject.find(reinterpret_cast<blender::Object *>(id));
    if (member != m_sharedMemberOfObject.end()) {
      KX_GameObject *object = member->second;
      if ((flag & ~(ID_RECALC_TRANSFORM | ID_RECALC_SHADING)) == 0) {
        object->TagUpbgeDupliInstanceForTaaReset();
        return;
      }
      object->NoteOwnDrawingNeeded();
    }
  }
  std::pair<blender::ID *, IDRecalcFlag> it = {id, flag};
  if (in_overlay_collection_only) {
    if (std::find(m_idsToUpdateInOverlayPass.begin(), m_idsToUpdateInOverlayPass.end(), it) ==
        m_idsToUpdateInOverlayPass.end())
    {
      m_idsToUpdateInOverlayPass.push_back(it);
    }
  }
  else {
    if (std::find(m_idsToUpdateInAllRenderPasses.begin(),
                  m_idsToUpdateInAllRenderPasses.end(),
                  it) == m_idsToUpdateInAllRenderPasses.end())
    {
      m_idsToUpdateInAllRenderPasses.push_back(it);
    }
  }
}

void KX_Scene::TagForExtraIdsUpdate(blender::Main *bmain, KX_Camera *cam)
{
  for (std::vector<std::pair<blender::ID *, IDRecalcFlag>>::iterator it =
           m_idsToUpdateInAllRenderPasses.begin();
       it != m_idsToUpdateInAllRenderPasses.end();
       it++)
  {
    DEG_id_tag_update(it->first, it->second);
  }
  /* Once: the evaluation stays for the frame's later passes. Tagged again at each pass, they
   * were evaluated again at each one (and woke Blender's worker threads each time). */
  m_idsToUpdateInAllRenderPasses.clear();

  if (cam && cam == GetOverlayCamera()) {
    for (std::vector<std::pair<blender::ID *, IDRecalcFlag>>::iterator it =
             m_idsToUpdateInOverlayPass.begin();
         it != m_idsToUpdateInOverlayPass.end();
         it++)
    {
      DEG_id_tag_update(it->first, it->second);
    }
    m_idsToUpdateInOverlayPass.clear();
  }
}

void KX_Scene::TagBlenderPhysicsObject(blender::Scene *scene, blender::Object *ob)
{
  if (ob && (ob->gameflag & OB_DUPLI_UPBGE)) {
    return;
  }
  /* Optionally handle Blender Physics simulation at bge runtime when supported */
  bool use_interactive_dynapaint = scene->gm.flag & GAME_USE_INTERACTIVE_DYNAPAINT;
  if (use_interactive_dynapaint) {
    /* Option to leave dynamic paint work during bge session :
     * Tag the brush for transform to update simulation */
    if (BKE_modifiers_findby_type(ob, eModifierType_DynamicPaint)) {
      ModifierData *md = BKE_modifiers_findby_type(ob, eModifierType_DynamicPaint);
      if (md && md->mode & (eModifierMode_Realtime | eModifierMode_Render)) {
        DynamicPaintModifierData *pmd2 = (DynamicPaintModifierData *)md;
        if (pmd2->brush) {
          DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM);
        }
      }
    }
  }
  bool use_interactive_rb = scene->gm.flag & GAME_USE_INTERACTIVE_RIGIDBODY;
  if (use_interactive_rb) {
    if (ob->rigidbody_object && ob->rigidbody_object->type == RBO_TYPE_ACTIVE) {
      DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM);
      ob->transflag |= OB_TRANSFLAG_OVERRIDE_GAME_PRIORITY;
    }
  }
}

/** The data `add_duplicate` made for a Full Copy: what its new object uses, directly or through
 * other new data, that was made after the object (IDs get increasing session ids as they are
 * made, and the duplicate copies the object first). */
static std::vector<blender::ID *> full_copy_owned_data(blender::Main *bmain, blender::Object *copy)
{
  std::vector<blender::ID *> owned;
  blender::Set<blender::ID *> seen = {&copy->id};
  std::vector<blender::ID *> to_walk = {&copy->id};
  while (!to_walk.empty()) {
    blender::ID *id = to_walk.back();
    to_walk.pop_back();
    blender::BKE_library_foreach_ID_link(
        bmain,
        id,
        [&](blender::LibraryIDLinkCallbackData *cb_data) {
          blender::ID *used = *cb_data->id_pointer;
          const int skip = blender::IDWALK_CB_EMBEDDED | blender::IDWALK_CB_EMBEDDED_NOT_OWNING |
                           blender::IDWALK_CB_LOOPBACK;
          if (used != nullptr && (cb_data->cb_flag & skip) == 0 &&
              used->session_uid > copy->id.session_uid && seen.add(used))
          {
            owned.push_back(used);
            to_walk.push_back(used);
          }
          return int(blender::IDWALK_RET_NOP);
        },
        nullptr,
        blender::IDWALK_READONLY);
  }
  return owned;
}

KX_GameObject *KX_Scene::AddFullCopyObject(KX_GameObject *gameobj,
                                           KX_GameObject *reference,
                                           float lifespan)
{
  blender::Object *ob = gameobj->GetBlenderObject();
  if (ob && (ob->gameflag & OB_DUPLI_UPBGE)) {
    CM_Warning("Full Copy of an UPBGE dupli base is not supported");
    return nullptr;
  }
  if (ob) {
    if (ob->instance_collection) {
      CM_Warning("Full Copy of an instance collection is not supported: " << ob->id.name + 2);
      return nullptr;
    }
    blender::bContext *C = KX_GetActiveEngine()->GetContext();
    blender::Main *bmain = CTX_data_main(C);
    blender::Scene *scene = GetBlenderScene();
    ViewLayer *view_layer = BKE_view_layer_default_view(scene);
    BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
    Base *base = BKE_view_layer_base_find(view_layer, ob);
    if (base) {
      Base *basen = blender::ed::object::add_duplicate(
          bmain, scene, view_layer, base, USER_DUP_OBDATA);
      std::vector<blender::ID *> owned_data = full_copy_owned_data(bmain, basen->object);
      /* Drawn from the master collection (the duplicate is also in its source's collections,
       * which may be disabled in viewports), as KX_GameObject::ReplicateBlenderObject does. */
      if (!BKE_collection_has_object(scene->master_collection, basen->object)) {
        BKE_collection_object_add(bmain, scene->master_collection, basen->object);
      }

      basen->flag |= (BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT |
                      BASE_ENABLED_AND_VISIBLE_IN_DEFAULT_VIEWPORT);
      basen->object->visibility_flag &= ~OB_HIDE_VIEWPORT;
      BKE_main_collection_sync_remap(bmain);

      DEG_relations_tag_update(bmain);
      ++m_renderSyncRelations;
      // BKE_scene_graph_update_tagged(depsgraph, bmain);
      ConvertBlenderObject(basen->object);

      KX_GameObject *replica = m_sceneConverter->FindGameObject(basen->object);
      /* Linked to the master collection, it still counts as in its source's for a camera's
       * Collection setting. */
      if (replica) {
        replica->SetCollectionSource(gameobj->GetCollectionSource());
        /* As a normal copy, it keeps Set Collection Member's changes of its source. */
        replica->SetCollectionMemberships(gameobj->GetCollectionMemberships());
        /* Its duplicated data goes with it (it stayed in the file before, one set per copy). */
        replica->SetOwnedData(std::move(owned_data));
      }

      // add a timebomb to this object
      // lifespan of zero means 'this object lives forever'
      if (lifespan > 0.0f) {
        // for now, convert between so called frames and realtime
        m_tempObjectList.push_back(replica);
        // this convert the life from frames to sort-of seconds, hard coded 0.02 that assumes we
        // have 50 frames per second if you change this value, make sure you change it in
        // KX_GameObject::pyattr_get_life property too
        EXP_Value *fval = new EXP_FloatValue(lifespan * 0.02f);
        replica->SetProperty("::timebomb", fval);
        fval->Release();
      }

      if (reference) {
        MT_Vector3 oldpos = replica->NodeGetWorldPosition();
        MT_Vector3 newpos = reference->NodeGetWorldPosition();
        replica->NodeSetLocalPosition(newpos);

        MT_Matrix3x3 newori = reference->NodeGetWorldOrientation();
        replica->NodeSetLocalOrientation(newori);

        // get the rootnode's scale
        MT_Vector3 newscale = reference->GetSGNode()->GetRootSGParent()->GetLocalScale();
        // set the replica's relative scale with the rootnode's scale
        replica->NodeSetRelativeScale(newscale);

        PHY_IPhysicsController *ctrl = replica->GetPhysicsController();

        /* Hack to fix softbody transform after conversion */
        if (ctrl) {
          ctrl->SetSoftBodyTransform(newpos - oldpos, newori);
        }
      }

      replica->GetSGNode()->UpdateWorldData(0);
      rebase_physics_interpolation_hierarchy(replica->GetSGNode());

      // Sync physics body to the updated transform.
      // This is needed for Jolt which creates bodies with fixed positions at creation time.
      if (replica->GetPhysicsController()) {
        replica->GetPhysicsController()->SetTransform();
        replica->GetPhysicsController()->RebaseKinematicMotionState();
      }

      if (!m_runtimeCollectionRenderVisibility.empty()) {
        RebuildRuntimeCollectionRenderVisibility();
      }

      return replica;
    }
  }
  return nullptr;
}

/******************End of EEVEE INTEGRATION****************************/

std::string KX_Scene::GetName()
{
  return m_sceneName;
}

/// Set the name of the value
void KX_Scene::SetName(const std::string &name)
{
  m_sceneName = name;
}

RAS_BucketManager *KX_Scene::GetBucketManager() const
{
  return m_bucketmanager;
}

EXP_ListValue<KX_GameObject> *KX_Scene::GetObjectList() const
{
  return m_objectlist;
}

EXP_ListValue<KX_GameObject> *KX_Scene::GetRootParentList() const
{
  return m_parentlist;
}

EXP_ListValue<KX_GameObject> *KX_Scene::GetInactiveList() const
{
  return m_inactivelist;
}

EXP_ListValue<KX_LightObject> *KX_Scene::GetLightList() const
{
  return m_lightlist;
}

SCA_LogicManager *KX_Scene::GetLogicManager() const
{
  return m_logicmgr;
}

SCA_TimeEventManager *KX_Scene::GetTimeEventManager() const
{
  return m_timemgr;
}

KX_PythonProxyManager &KX_Scene::GetPythonProxyManager()
{
  return m_proxyManager;
}

EXP_ListValue<KX_Camera> *KX_Scene::GetCameraList() const
{
  return m_cameralist;
}

void KX_Scene::SetCameraList(EXP_ListValue<KX_Camera> *camList)
{
  m_cameralist = camList;
}

EXP_ListValue<KX_FontObject> *KX_Scene::GetFontList() const
{
  return m_fontlist;
}

void KX_Scene::SetFramingType(RAS_FrameSettings &frame_settings)
{
  m_frame_settings = frame_settings;
};

/**
 * Return a const reference to the framing
 * type set by the above call.
 * The contents are not guaranteed to be sensible
 * if you don't call the above function.
 */
const RAS_FrameSettings &KX_Scene::GetFramingType() const
{
  return m_frame_settings;
}

void KX_Scene::SetActivityCulling(bool b)
{
  m_activityCulling = b;
}

void KX_Scene::AddObjectDebugProperties(class KX_GameObject *gameobj)
{
  blender::Object *blenderobject = gameobj->GetBlenderObject();
  if (!blenderobject) {
    return;
  }

  bProperty *prop = (bProperty *)blenderobject->prop.first;

  while (prop) {
    if (prop->flag & PROP_DEBUG)
      AddDebugProperty(gameobj, prop->name);
    prop = prop->next;
  }

  if (blenderobject->scaflag & OB_DEBUGSTATE)
    AddDebugProperty(gameobj, "__state__");
}

void KX_Scene::RemoveNodeDestructObject(SG_Node *node, KX_GameObject *gameobj)
{
  if (NewRemoveObject(gameobj, m_activeRemovalReason)) {
    // object is not yet deleted because a reference is hanging somewhere.
    // This should not happen anymore since we use proxy object for Python.
    CM_Error("zombie object! name=" << gameobj->GetName());
    BLI_assert(false);
  }
  if (node)
    delete node;
}

KX_GameObject *KX_Scene::AddNodeReplicaObject(SG_Node *node, KX_GameObject *gameobj)
{
  // for group duplication, limit the duplication of the hierarchy to the
  // objects that are part of the group.
  if (!IsObjectInGroup(gameobj))
    return nullptr;

  KX_GameObject *newobj = (KX_GameObject *)gameobj->GetReplica();
  m_map_gameobject_to_replica[gameobj] = newobj;
  for (std::vector<KX_GameObject *> *capture : m_activeReplicaCaptures) {
    if (capture != nullptr) {
      capture->push_back(newobj);
    }
  }
  for (std::unordered_map<KX_GameObject *, KX_GameObject *> *capture :
       m_activeReplicaMappingCaptures)
  {
    if (capture != nullptr) {
      capture->insert_or_assign(gameobj, newobj);
    }
  }

  // also register 'timers' (time properties) of the replica
  int numprops = newobj->GetPropertyCount();

  for (int i = 0; i < numprops; i++) {
    EXP_Value *prop = newobj->GetProperty(i);

    if (prop->GetProperty("timer"))
      this->m_timemgr->AddTimeProperty(prop);
  }

  if (node) {
    newobj->SetSGNode(node);
  }
  else {
    m_rootnode = new SG_Node(newobj, this, KX_Scene::m_callbacks);
    newobj->ClearParentIntent();

    // this fixes part of the scaling-added object bug
    SG_Node *orgnode = gameobj->GetSGNode();
    m_rootnode->SetLocalScale(orgnode->GetLocalScale());
    m_rootnode->SetLocalPosition(orgnode->GetLocalPosition());
    m_rootnode->SetLocalOrientation(orgnode->GetLocalOrientation());

    // define the relationship between this node and it's parent.
    KX_NormalParentRelation *parent_relation = new KX_NormalParentRelation();
    m_rootnode->SetParentRelation(parent_relation);

    newobj->SetSGNode(m_rootnode);
  }

  SG_Node *replicanode = newobj->GetSGNode();
  //	SG_Node* rootnode = (replicanode == m_rootnode ? nullptr : m_rootnode);

  // Add the object in the obstacle simulation if needed.
  if (m_obstacleSimulation && gameobj->GetBlenderObject()->gameflag & OB_HASOBSTACLE) {
    m_obstacleSimulation->AddObstacleForObj(newobj);
  }

  // Register object for component update.
  if (gameobj->GetPrototype() || gameobj->GetComponents()) {
    m_proxyManager.Register(newobj);
  }

  replicanode->SetSGClientObject(newobj);

  // this is the list of object that are send to the graphics pipeline
  m_objectlist->Add(CM_AddRef(newobj));
  NoteObjectListChanged();
  if (IsOverlayRuntimeObject(gameobj)) {
    m_overlayRuntimeObjects.insert(newobj);
    newobj->NoteBlenderObjectChanged();
    if (const auto source = m_overlayMemberSources.find(gameobj);
        source != m_overlayMemberSources.end())
    {
      m_overlayMemberSources[newobj] = source->second;
    }
    if (const auto instance = m_overlayMemberInstance.find(gameobj);
        instance != m_overlayMemberInstance.end())
    {
      m_overlayMemberInstance[newobj] = instance->second;
    }
  }
  ApplyRuntimeCollectionRenderVisibility(newobj);
  switch (newobj->GetGameObjectType()) {
    case SCA_IObject::OBJ_LIGHT: {
      m_lightlist->Add(CM_AddRef(static_cast<KX_LightObject *>(newobj)));
      break;
    }
    case SCA_IObject::OBJ_TEXT: {
      m_fontlist->Add(CM_AddRef(static_cast<KX_FontObject *>(newobj)));
      break;
    }
    case SCA_IObject::OBJ_CAMERA: {
      m_cameralist->Add(CM_AddRef(static_cast<KX_Camera *>(newobj)));
      break;
    }
    case SCA_IObject::OBJ_ARMATURE: {
      AddAnimatedObject(newobj);
      break;
    }
  }

  // logic cannot be replicated, until the whole hierarchy is replicated.
  m_logicHierarchicalGameObjects.push_back(newobj);
  // replicate controllers of this node
  SGControllerList scenegraphcontrollers = gameobj->GetSGNode()->GetSGControllerList();
  replicanode->RemoveAllControllers();

  for (SG_Controller *controller : scenegraphcontrollers) {
    // controller replication is quite complicated
    // only replicate ipo controller for now

    SG_Controller *replicacontroller = controller->GetReplica(replicanode);
    if (replicacontroller) {
      replicacontroller->SetNode(replicanode);
      replicanode->AddSGController(replicacontroller);
    }
  }

  // replicate physics controller
  if (gameobj->GetPhysicsController()) {
    PHY_IMotionState *motionstate = new KX_MotionState(newobj->GetSGNode());
    PHY_IPhysicsController *newctrl = gameobj->GetPhysicsController()->GetReplica();

    KX_GameObject *parent = newobj->GetParent();
    PHY_IPhysicsController *parentctrl = (parent) ? parent->GetPhysicsController() : nullptr;

    newctrl->SetNewClientInfo(newobj->getClientInfo());
    newobj->SetPhysicsController(newctrl);
    newctrl->PostProcessReplica(motionstate, parentctrl);

    /* Dynamic children must be suspended because the scene graph owns their
     * transform while parented. Kinematic children are already transform-owned
     * and must stay in the world to push Dynamic bodies. */
    if (parent && !newctrl->IsKinematic())
      newctrl->SuspendDynamics();
  }

  return newobj;
}

// before calling this method KX_Scene::ReplicateLogic(), make sure to
// have called 'GameObject::ReParentLogic' for each object this
// hierarchy that's because first ALL bricks must exist in the new
// replica of the hierarchy in order to make cross-links work properly
// !
// It is VERY important that the order of sensors and actuators in
// the replicated object is preserved: it is used to reconnect the logic.
// This method is more robust then using the bricks name in case of complex
// group replication. The replication of logic bricks is done in
// SCA_IObject::ReParentLogic(), make sure it preserves the order of the bricks.
void KX_Scene::ReplicateLogic(KX_GameObject *newobj)
{
  /* add properties to debug list, for added objects and DupliGroups */
  if (KX_GetActiveEngine()->GetFlag(KX_KetsjiEngine::AUTO_ADD_DEBUG_PROPERTIES)) {
    AddObjectDebugProperties(newobj);
  }
  // also relink the controller to sensors/actuators
  const SCA_ControllerList controllers = newobj->GetControllers();
  // SCA_SensorList&     sensors     = newobj->GetSensors();
  // SCA_ActuatorList&   actuators   = newobj->GetActuators();

  for (SCA_IController *cont : controllers) {
    cont->SetUeberExecutePriority(m_ueberExecutionPriority);
    const SCA_SensorList linkedsensors = cont->GetLinkedSensors();
    const SCA_ActuatorList linkedactuators = cont->GetLinkedActuators();

    // disconnect the sensors and actuators
    // do it directly on the list at this controller is not connected to anything at this stage
    cont->GetLinkedSensors().clear();
    cont->GetLinkedActuators().clear();

    // now relink each sensor
    for (SCA_ISensor *oldsensor : linkedsensors) {
      SCA_IObject *oldsensorobj = oldsensor->GetParent();
      // the original owner of the sensor has been replicated?
      SCA_IObject *newsensorobj = m_map_gameobject_to_replica[oldsensorobj];

      if (!newsensorobj) {
        // no, then the sensor points outside the hierarchy, keep it the same
        if (m_objectlist->SearchValue(static_cast<KX_GameObject *>(oldsensorobj)))
          // only replicate links that points to active objects
          m_logicmgr->RegisterToSensor(cont, oldsensor);
      }
      else {
        // yes, then the new sensor has the same position
        SCA_SensorList &sensorlist = oldsensorobj->GetSensors();
        SCA_SensorList::iterator sit;
        SCA_ISensor *newsensor = nullptr;
        int sensorpos;

        for (sensorpos = 0, sit = sensorlist.begin(); sit != sensorlist.end(); sit++, sensorpos++)
        {
          if ((*sit) == oldsensor) {
            newsensor = newsensorobj->GetSensors().at(sensorpos);
            break;
          }
        }
        BLI_assert(newsensor != nullptr);
        m_logicmgr->RegisterToSensor(cont, newsensor);
      }
    }

    // now relink each actuator
    for (SCA_IActuator *oldactuator : linkedactuators) {
      SCA_IObject *oldactuatorobj = oldactuator->GetParent();
      SCA_IObject *newactuatorobj = m_map_gameobject_to_replica[oldactuatorobj];

      if (!newactuatorobj) {
        // no, then the sensor points outside the hierarchy, keep it the same
        if (m_objectlist->SearchValue(static_cast<KX_GameObject *>(oldactuatorobj)))
          // only replicate links that points to active objects
          m_logicmgr->RegisterToActuator(cont, oldactuator);
      }
      else {
        // yes, then the new sensor has the same position
        SCA_ActuatorList &actuatorlist = oldactuatorobj->GetActuators();
        SCA_ActuatorList::iterator ait;
        SCA_IActuator *newactuator = nullptr;
        int actuatorpos;

        for (actuatorpos = 0, ait = actuatorlist.begin(); ait != actuatorlist.end();
             ait++, actuatorpos++)
        {
          if ((*ait) == oldactuator) {
            newactuator = newactuatorobj->GetActuators().at(actuatorpos);
            break;
          }
        }
        BLI_assert(newactuator != nullptr);
        m_logicmgr->RegisterToActuator(cont, newactuator);
        newactuator->SetUeberExecutePriority(m_ueberExecutionPriority);
      }
    }
  }
  // ready to set initial state
  newobj->ResetState();
}

static void remap_parents_recursive(KX_GameObject *parent)
{
  std::vector<KX_GameObject *> children = parent->GetChildren();
  for (KX_GameObject *child : children) {
    child->GetBlenderObject()->parent = parent->GetBlenderObject();

    remap_parents_recursive(child);
  }
}

void KX_Scene::DupliGroupRecurse(KX_GameObject *groupobj, int level)
{
  blender::Object *blgroupobj = groupobj->GetBlenderObject();
  std::vector<KX_GameObject *> duplilist;

  if (!groupobj->GetSGNode() || !groupobj->IsDupliGroup() || level > MAX_DUPLI_RECUR)
    return;

  // we will add one group at a time
  m_logicHierarchicalGameObjects.clear();
  m_map_gameobject_to_replica.clear();
  m_ueberExecutionPriority++;
  // for groups will do something special:
  // we will force the creation of objects to those in the group only
  // Again, this is match what Blender is doing (it doesn't care of parent relationship)
  m_groupGameObjects.clear();

  blender::Collection *group = blgroupobj->instance_collection;
  FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (group, blenderobj) {
    if (blgroupobj == blenderobj)
      // this check is also in group_duplilist()
      continue;

    KX_GameObject *gameobj = (KX_GameObject *)m_logicmgr->FindGameObjByBlendObj(blenderobj);
    if (gameobj == nullptr) {
      // this object has not been converted!!!
      // Should not happen as dupli group are created automatically
      continue;
    }

    if (group->flag & COLLECTION_IS_SPAWNED) {
      if (!BKE_collection_has_object(group, blenderobj)) {
        // old method to spawn in an empty + all group members
        continue;
      }
    }
    else {
      if ((blenderobj->lay & groupobj->GetLayer()) == 0) {
        // blender::Object is not visible in the 3D view, will not be instantiated. ??
        /* 3 remarks:
         * - The comment shouldn't be: "if blenderobj not in same layer than groupobj, don't
         * convert it as gameobj"?
         * - The code was using blenderobj->lay & group->layer but group->layer is deprecated.
         * - Maybe it shouldn't be in an else statement. */
        continue;
      }
    }

    gameobj->SetBlenderGroupObject(blgroupobj);
    m_groupGameObjects.insert(gameobj);
  }
  FOREACH_COLLECTION_OBJECT_RECURSIVE_END;

  for (KX_GameObject *gameobj : m_groupGameObjects) {
    KX_GameObject *parent = gameobj->GetParent();
    if (parent != nullptr) {
      // this object is not a top parent. Either it is the child of another
      // object in the group and it will be added automatically when the parent
      // is added. Or it is the child of an object outside the group and the group
      // is inconsistent, skip it anyway
      continue;
    }
    KX_GameObject *replica = (KX_GameObject *)AddNodeReplicaObject(nullptr, gameobj);
    // add to 'rootparent' list (this is the list of top hierarchy objects, updated each frame)
    m_parentlist->Add(CM_AddRef(replica));

    // recurse replication into children nodes
    const NodeList children = gameobj->GetSGNode()->GetSGChildren();

    replica->GetSGNode()->ClearSGChildren();
    for (SG_Node *orgnode : children) {
      SG_Node *childreplicanode = orgnode->GetSGReplica();
      if (childreplicanode)
        replica->GetSGNode()->AddChild(childreplicanode);
    }
    // don't replicate logic now: we assume that the objects in the group can have
    // logic relationship, even outside parent relationship
    // In order to match 3D view, the position of groupobj is used as a
    // transformation matrix instead of the new position. This means that
    // the group reference point is 0,0,0

    // get the rootnode's scale
    MT_Vector3 newscale = groupobj->NodeGetWorldScaling();
    // set the replica's relative scale with the rootnode's scale
    replica->NodeSetRelativeScale(newscale);

    MT_Vector3 offset(group->instance_offset);
    MT_Vector3 newpos = groupobj->NodeGetWorldPosition() +
                        newscale * (groupobj->NodeGetWorldOrientation() *
                                    (gameobj->NodeGetWorldPosition() - offset));
    replica->NodeSetLocalPosition(newpos);
    // set the orientation after position for softbody!
    MT_Matrix3x3 newori = groupobj->NodeGetWorldOrientation() * gameobj->NodeGetWorldOrientation();
    replica->NodeSetLocalOrientation(newori);
    // update scenegraph for entire tree of children
    replica->GetSGNode()->UpdateWorldData(0);
    rebase_physics_interpolation_hierarchy(replica->GetSGNode());

    // Sync physics body to the updated transform.
    // This is needed for Jolt which creates bodies with fixed positions at creation time.
    if (replica->GetPhysicsController()) {
      replica->GetPhysicsController()->SetTransform();
      replica->GetPhysicsController()->RebaseKinematicMotionState();
    }

    remap_parents_recursive(replica);

    // done with replica
    replica->Release();
  }

  // the logic must be replicated first because we need
  // the new logic bricks before relinking
  for (KX_GameObject *gameobj : m_logicHierarchicalGameObjects) {
    gameobj->ReParentLogic();
  }

  //	relink any pointers as necessary, sort of a temporary solution
  for (KX_GameObject *gameobj : m_logicHierarchicalGameObjects) {
    // this will also relink the actuator to objects within the hierarchy
    gameobj->Relink(m_map_gameobject_to_replica);
    // add the object in the layer of the parent
    gameobj->SetLayer(groupobj->GetLayer());
  }

  // replicate crosslinks etc. between logic bricks
  for (KX_GameObject *gameobj : m_logicHierarchicalGameObjects) {
    ReplicateLogic(gameobj);
  }

#ifdef WITH_GAMEENGINE_LOGICNODES
  ReplicateLogicNodeRuntimeTreesForReplicaMap();
#endif

  // Build lookup map for fast name resolution during constraint replication.
  std::unordered_map<std::string, KX_GameObject *> hierarchyLookup;
  hierarchyLookup.reserve(m_logicHierarchicalGameObjects.size());
  for (KX_GameObject *obj : m_logicHierarchicalGameObjects) {
    hierarchyLookup.emplace(obj->GetName(), obj);
  }

  // now look if object in the hierarchy have dupli group and recurse
  for (KX_GameObject *gameobj : m_logicHierarchicalGameObjects) {
    /* Replicate all constraints. */
    if (gameobj->GetPhysicsController()) {
      gameobj->GetPhysicsController()->ReplicateConstraints(gameobj,
                                                            m_logicHierarchicalGameObjects);
      gameobj->ClearConstraints();
    }

    if (gameobj->HasRigidBodyConstraints()) {
      gameobj->ReplicateRigidBodyConstraints(hierarchyLookup);
      // Don't clear - keep constraint IDs for cleanup when object is deleted
    }

    if (gameobj != groupobj && gameobj->IsDupliGroup())
      // can't instantiate group immediately as it destroys m_logicHierarchicalGameObjects
      duplilist.push_back(gameobj);

    if (gameobj->GetBlenderGroupObject() == blgroupobj) {
      // set references for dupli-group
      // groupobj holds a list of all objects, that belongs to this group
      groupobj->AddInstanceObjects(gameobj);

      /* If the groupobj itself is parented, parent the top parent instance spawned to invisibled
       * groupobj to have the same behaviour than in viewport.
       */
      if (groupobj->GetParent()) {
        gameobj->SetParent(groupobj, false, false);
      }

      // every object gets the reference to its dupli-group object
      gameobj->SetDupliGroupObject(groupobj);
      /* Drawn or hidden by its collections again, now with the Empty's (it was copied before). */
      ApplyRuntimeCollectionRenderVisibility(gameobj);
    }
  }

  for (KX_GameObject *gameobj : duplilist) {
    DupliGroupRecurse(gameobj, level + 1);
  }
}

KX_GameObject *KX_Scene::AddReplicaObject(KX_GameObject *originalobject,
                                          KX_GameObject *referenceobject,
                                          float lifespan,
                                          std::vector<KX_GameObject *> *created_members,
                                          const std::set<KX_GameObject *> *replica_filter,
                                          std::unordered_map<KX_GameObject *, KX_GameObject *> *
                                              created_mapping)
{
  m_logicHierarchicalGameObjects.clear();
  m_map_gameobject_to_replica.clear();
  m_groupGameObjects.clear();
  if (replica_filter != nullptr) {
    m_groupGameObjects = *replica_filter;
  }
  if (created_members != nullptr) {
    created_members->clear();
    m_activeReplicaCaptures.push_back(created_members);
  }
  if (created_mapping != nullptr) {
    created_mapping->clear();
    m_activeReplicaMappingCaptures.push_back(created_mapping);
  }
  BLI_SCOPED_DEFER([&]() {
    if (created_members != nullptr) {
      BLI_assert(!m_activeReplicaCaptures.empty() &&
                 m_activeReplicaCaptures.back() == created_members);
      m_activeReplicaCaptures.pop_back();
    }
    if (created_mapping != nullptr) {
      BLI_assert(!m_activeReplicaMappingCaptures.empty() &&
                 m_activeReplicaMappingCaptures.back() == created_mapping);
      m_activeReplicaMappingCaptures.pop_back();
    }
    m_groupGameObjects.clear();
  });

  KX_GameObject *originalobj = (KX_GameObject *)originalobject;
  KX_GameObject *referenceobj = (KX_GameObject *)referenceobject;

  m_ueberExecutionPriority++;

  // lets create a replica
  KX_GameObject *replica = (KX_GameObject *)AddNodeReplicaObject(nullptr, originalobj);
  if (replica == nullptr) {
    return nullptr;
  }

  // add a timebomb to this object
  // lifespan of zero means 'this object lives forever'
  if (lifespan > 0.0f) {
    // for now, convert between so called frames and realtime
    m_tempObjectList.push_back(replica);
    // this convert the life from frames to sort-of seconds, hard coded 0.016666667 that assumes we
    // have 60 frames per second if you change this value, make sure you change it in
    // KX_GameObject::pyattr_get_life property too
    EXP_Value *fval = new EXP_FloatValue(lifespan * 0.016666667f);
    replica->SetProperty("::timebomb", fval);
    fval->Release();
  }

  // add to 'rootparent' list (this is the list of top hierarchy objects, updated each frame)
  m_parentlist->Add(CM_AddRef(replica));

  // recurse replication into children nodes

  const NodeList children = originalobj->GetSGNode()->GetSGChildren();

  replica->GetSGNode()->ClearSGChildren();
  for (SG_Node *orgnode : children) {
    SG_Node *childreplicanode = orgnode->GetSGReplica();
    if (childreplicanode)
      replica->GetSGNode()->AddChild(childreplicanode);
  }

  if (referenceobj) {
    // At this stage all the objects in the hierarchy have been duplicated,
    // we can update the scenegraph, we need it for the duplication of logic
    MT_Vector3 newpos = referenceobj->NodeGetWorldPosition();
    replica->NodeSetLocalPosition(newpos);

    MT_Matrix3x3 newori = referenceobj->NodeGetWorldOrientation();
    replica->NodeSetLocalOrientation(newori);

    // get the rootnode's scale
    MT_Vector3 newscale = referenceobj->GetSGNode()->GetRootSGParent()->GetLocalScale();
    // set the replica's relative scale with the rootnode's scale
    replica->NodeSetRelativeScale(newscale);
  }

  replica->GetSGNode()->UpdateWorldData(0);
  rebase_physics_interpolation_hierarchy(replica->GetSGNode());

  // Sync physics bodies to the updated transforms.
  // This is needed for Jolt which creates bodies with fixed positions at creation time,
  // unlike Bullet which uses a motion state callback system.
  for (KX_GameObject *gameobj : m_logicHierarchicalGameObjects) {
    if (gameobj->GetPhysicsController()) {
      gameobj->GetPhysicsController()->SetTransform();
      gameobj->GetPhysicsController()->RebaseKinematicMotionState();
    }
  }

  // now replicate logic
  for (KX_GameObject *gameobj : m_logicHierarchicalGameObjects) {
    gameobj->ReParentLogic();
  }

  //	relink any pointers as necessary, sort of a temporary solution
  for (KX_GameObject *gameobj : m_logicHierarchicalGameObjects) {
    // this will also relink the actuators in the hierarchy
    gameobj->Relink(m_map_gameobject_to_replica);
    if (referenceobj) {
      // add the object in the layer of the reference object
      gameobj->SetLayer(referenceobj->GetLayer());
    }
    else {
      // We don't know what layer set, so we set all visible layers in the blender scene.
      gameobj->SetLayer(m_blenderScene->lay);
    }
  }

  // replicate crosslinks etc. between logic bricks
  for (KX_GameObject *gameobj : m_logicHierarchicalGameObjects) {
    ReplicateLogic(gameobj);
  }

#ifdef WITH_GAMEENGINE_LOGICNODES
  ReplicateLogicNodeRuntimeTreesForReplicaMap();
#endif

  // check if there are objects with dupligroup in the hierarchy
  std::vector<KX_GameObject *> duplilist;
  for (KX_GameObject *gameobj : m_logicHierarchicalGameObjects) {
    if (gameobj->IsDupliGroup()) {
      // separate list as m_logicHierarchicalGameObjects is also used by DupliGroupRecurse()
      duplilist.push_back(gameobj);
    }
  }
  for (KX_GameObject *gameobj : duplilist) {
    DupliGroupRecurse(gameobj, 0);
  }

  remap_parents_recursive(replica);

  //	don't release replica here because we are returning it, not done with it...
  return replica;
}

void KX_Scene::RemoveObject(KX_GameObject *gameobj, const KX_ObjectRemovalReason reason)
{
  // disconnect child from parent
  SG_Node *node = gameobj->GetSGNode();

  if (node) {
    const KX_ObjectRemovalReason previous_reason = m_activeRemovalReason;
    m_activeRemovalReason = reason;
    node->DisconnectFromParent();

    // recursively destruct
    node->Destruct();
    m_activeRemovalReason = previous_reason;
  }
}

void KX_Scene::RemoveDupliGroup(KX_GameObject *gameobj, const KX_ObjectRemovalReason reason)
{
  if (gameobj->IsDupliGroup()) {
    for (KX_GameObject *instance : gameobj->GetInstanceObjects()) {
      DelayedRemoveObject(instance, reason);
    }
  }
}

void KX_Scene::DelayedRemoveObject(KX_GameObject *gameobj, const KX_ObjectRemovalReason reason)
{
  RemoveDupliGroup(gameobj, reason);

  const bool newly_queued = std::find(m_euthanasyobjects.begin(),
                                      m_euthanasyobjects.end(),
                                      gameobj) == m_euthanasyobjects.end();
  if (newly_queued) {
    m_euthanasyRemovalReasons.emplace(gameobj, reason);
  }

#ifdef WITH_GAMEENGINE_LOGICNODES
  if (m_logicNodeManager) {
    KX_ObjectRemovalReason effective_reason = reason;
    if (!newly_queued) {
      const auto found = m_euthanasyRemovalReasons.find(gameobj);
      if (found != m_euthanasyRemovalReasons.end()) {
        effective_reason = found->second;
      }
    }
    m_logicNodeManager->NotifyGameObjectQueuedForRemoval(gameobj, effective_reason);
  }
#endif

  CM_ListAddIfNotFound(m_euthanasyobjects, gameobj);

  for (const auto &item : m_overlayCollectionMembers) {
    if (item.second.contains(gameobj)) {
      m_overlayResources.NoteMembershipChanged(item.first);
    }
  }

  /* Unregister asap (don't wait next frame) to avoid issue
   * when objects are added/removed the same frame
   * https://github.com/UPBGE/upbge/issues/1600
   */
  GetBlenderSceneConverter()->UnregisterGameObject(gameobj);
}

bool KX_Scene::NewRemoveObject(KX_GameObject *gameobj, const KX_ObjectRemovalReason reason)
{
  if (gameobj != nullptr) {
    /* Object properties referring to it become empty. */
    gameobj->ClearReferenceValues();
  }
  /* A replica owns its temporary Blender Object ID, which can be deleted when the final scene
   * reference is released below. Remove pending render-pass tags for that exact ID first; shared
   * mesh/material IDs remain valid and other KX instances are unaffected. */
  blender::Object *render_object = gameobj ? gameobj->GetBlenderObject() : nullptr;
  if (gameobj != nullptr && gameobj->IsReplica() && render_object != nullptr) {
    const auto references_render_object = [render_object](const auto &update) {
      return update.first == &render_object->id;
    };
    std::erase_if(m_idsToUpdateInAllRenderPasses, references_render_object);
    std::erase_if(m_idsToUpdateInOverlayPass, references_render_object);
  }

  if (gameobj == m_overlayCamera) {
    /* An externally queued removal of the live runtime camera invalidates the complete overlay
     * view. Detach ownership first so collection teardown cannot queue the same root twice. */
    DestroyOverlayCameraRuntimeInstance(false);
    const std::vector<blender::Collection *> collections = m_overlay_collections;
    for (blender::Collection *collection : collections) {
      RemoveOverlayCollection(collection);
    }
  }
  for (auto item = m_overlayCollectionReplicas.begin(); item != m_overlayCollectionReplicas.end();)
  {
    item->second.erase(gameobj);
    if (item->second.empty()) {
      item = m_overlayCollectionReplicas.erase(item);
    }
    else {
      ++item;
    }
  }
  blender::Object *removed_overlay_object = nullptr;
  for (auto item = m_overlayCollectionMembers.begin(); item != m_overlayCollectionMembers.end();) {
    if (item->second.erase(gameobj) != 0 && gameobj != nullptr) {
      removed_overlay_object = gameobj->GetBlenderObject();
    }
    if (item->second.empty()) {
      item = m_overlayCollectionMembers.erase(item);
    }
    else {
      ++item;
    }
  }
  for (auto item = m_overlayCollectionShownObjects.begin();
       item != m_overlayCollectionShownObjects.end();)
  {
    item->second.erase(gameobj);
    if (item->second.empty()) {
      item = m_overlayCollectionShownObjects.erase(item);
    }
    else {
      ++item;
    }
  }
  m_overlayRuntimeObjects.erase(gameobj);
  ForgetOverlayCopy(gameobj);
  m_overlayMemberSources.erase(gameobj);
  m_overlayMemberInstance.erase(gameobj);
  m_overlayCameraReplicaMembers.erase(gameobj);
  if (removed_overlay_object != nullptr) {
    if (AnyRuntimeOverlayContainsObject(removed_overlay_object)) {
      removed_overlay_object->gameflag |= OB_OVERLAY_COLLECTION;
    }
    else {
      removed_overlay_object->gameflag &= ~OB_OVERLAY_COLLECTION;
    }
  }

  /* Both interpolation lists are non-owning and may outlive a render-frame command flush. Remove
   * the object before any Release below can destroy it. */
  UnregisterInterpolatedObject(gameobj);

#ifdef WITH_GAMEENGINE_LOGICNODES
  if (m_logicNodeManager) {
    m_logicNodeManager->UnregisterGameObject(gameobj, reason);
  }
#endif

  /* Replicas are normally absent from the name map, while reconstructed authored objects are
   * present. Only erase an exact match so deleting one object cannot unregister a same-named
   * replacement that was published later. */
  m_logicmgr->UnregisterGameObjectName(gameobj->GetName(), gameobj);

  gameobj->Dispose();

  /* remove property from debug list */
  RemoveObjectDebugProperties(gameobj);

  /* Invalidate the python reference, since the object may exist in script lists
   * its possible that it wont be automatically invalidated, so do it manually here,
   *
   * if for some reason the object is added back into the scene python can always get a new Proxy
   */
  gameobj->InvalidateProxy();

  // keep the blender->game object association up to date
  // note that all the replicas of an object will have the same
  // blender object, that's why we need to check the game object
  // as only the deletion of the original object must be recorded
  if (gameobj->GetBlenderObject()) {
    // In some case the game object can contains a nullptr blender object e.g default camera.
    m_logicmgr->UnregisterGameObj(gameobj->GetBlenderObject(), gameobj);
  }

  // remove all sensors/controllers/actuators from logicsystem...

  SCA_SensorList &sensors = gameobj->GetSensors();
  for (SCA_ISensor *sensor : sensors) {
    m_logicmgr->RemoveSensor(sensor);
  }

  SCA_ControllerList &controllers = gameobj->GetControllers();
  for (SCA_IController *controller : controllers) {
    m_logicmgr->RemoveController(controller);
    controller->ReParent(nullptr);
  }

  SCA_ActuatorList &actuators = gameobj->GetActuators();
  for (SCA_IActuator *actuator : actuators) {
    m_logicmgr->RemoveActuator(actuator);
  }
  // the sensors/controllers/actuators must also be released, this is done in ~SCA_IObject

  // now remove the timer properties from the time manager
  int numprops = gameobj->GetPropertyCount();

  for (int i = 0; i < numprops; i++) {
    EXP_Value *propval = gameobj->GetProperty(i);
    if (propval->GetProperty("timer")) {
      m_timemgr->RemoveTimeProperty(propval);
    }
  }

  // if the object is the dupligroup proxy, you have to cleanup all m_pDupliGroupObject's in all
  // instances referring to this group
  if (gameobj->GetInstanceObjects()) {
    for (KX_GameObject *instance : gameobj->GetInstanceObjects()) {
      instance->RemoveDupliGroupObject();
      /* Out of the Empty's collections. */
      NoteObjectCollectionsChanged(instance);
    }
  }

  // if this object was part of a group, make sure to remove it from that group's instance list
  KX_GameObject *group = gameobj->GetDupliGroupObject();
  if (group)
    group->RemoveInstanceObject(gameobj);

  if (m_obstacleSimulation) {
    m_obstacleSimulation->DestroyObstacleForObj(gameobj);
  }

  m_proxyManager.Unregister(gameobj);

#ifdef WITH_GAMEENGINE_LOGICNODES
  m_pauseAnimatedObjects.erase(gameobj);
#endif

  gameobj->RemoveMeshes();

  bool ret = true;
  if (m_lightlist->RemoveValue(gameobj)) {
    ret = (gameobj->Release() != nullptr);
  }
  if (m_objectlist->RemoveValue(gameobj)) {
    NoteObjectListChanged();
    ret = (gameobj->Release() != nullptr);
  }
  if (m_parentlist->RemoveValue(gameobj)) {
    ret = (gameobj->Release() != nullptr);
  }
  if (m_inactivelist->RemoveValue(gameobj)) {
    ret = (gameobj->Release() != nullptr);
  }
  if (m_fontlist->RemoveValue(gameobj)) {
    ret = (gameobj->Release() != nullptr);
  }
  if (m_cameralist->RemoveValue(gameobj)) {
    ret = (gameobj->Release() != nullptr);
  }

  // WARNING: 'gameobj' maybe be freed now, only compare, don't access.
  CM_ListRemoveIfFound(m_animatedlist, gameobj);
  CM_ListRemoveIfFound(m_euthanasyobjects, gameobj);
  m_euthanasyRemovalReasons.erase(gameobj);
  CM_ListRemoveIfFound(m_tempObjectList, gameobj);
  /* A removed camera's culling bit goes to the next new camera. */
  for (size_t i = 0; i < m_cullCameras.size(); i++) {
    if (m_cullCameras[i] == gameobj) {
      m_cullCameras[i] = nullptr;
      if (i < m_cullCameraPasses.size()) {
        m_cullCameraPasses[i].known = false;
      }
    }
  }

  if (gameobj == m_active_camera) {
    // no AddRef done on m_active_camera so no Release
    // m_active_camera->Release();
    m_active_camera = nullptr;
  }

  if (gameobj == m_overrideCullingCamera) {
    m_overrideCullingCamera = nullptr;
  }
  m_playerViews.RemoveObject(gameobj);
  std::erase_if(m_cameraHiddenObjects, [gameobj](const CameraHiddenObject &hidden) {
    return hidden.object == gameobj || hidden.camera == gameobj;
  });

  // return value will be 0 if the object is actually deleted (all reference gone)

  return ret;
}

void KX_Scene::ReplaceMesh(KX_GameObject *gameobj,
                           RAS_MeshObject *mesh,
                           bool use_gfx,
                           bool use_phys)
{
  if (!gameobj) {
    CM_FunctionWarning("invalid object, doing nothing");
    return;
  }

  if (!mesh) {
    return;
  }

  if (use_gfx) {
    /* Another mesh: a Merged object needs its own object, a Shared one cannot (warned). */
    gameobj->NoteOwnDrawingNeeded();
    gameobj->RemoveMeshes();
    gameobj->AddMesh(mesh);

    /* Here we are in the case where we use ReplaceMesh not for levels of details
     * but for other purposes. We'll add a dummy LodManager with only 1 KX_LodLevel
     * because we need it to update the rendered mesh.
     */
    if (!gameobj->GetLodManager() || gameobj->GetLodManager()->GetLevelCount() < 2) {
      if (gameobj->GetLodManager()) {
        gameobj->GetLodManager()->Release();
      }
      gameobj->AddDummyLodManager(mesh, mesh->GetOriginalObject());  // tmp
    }
  }

  if (use_phys) { /* update the new assigned mesh with the physics mesh */
    if (gameobj->GetPhysicsController())
      gameobj->GetPhysicsController()->ReinstancePhysicsShape(gameobj, mesh);
  }
}

blender::Mesh *KX_Scene::GetReplaceMeshEvalMesh(RAS_MeshObject *mesh)
{
  if (mesh == nullptr || mesh->GetOrigMesh() == nullptr) {
    return nullptr;
  }
  auto [item, inserted] = m_replaceMeshEvalMeshes.try_emplace(mesh, nullptr);
  if (inserted) {
    /* A copy outside Main shares the original's data, so it is cheap. */
    item->second = BKE_mesh_copy_for_eval(*mesh->GetOrigMesh());
  }
  return item->second;
}

KX_Camera *KX_Scene::GetActiveCamera()
{
  // nullptr if not defined
  return m_active_camera;
}

void KX_Scene::SetActiveCamera(KX_Camera *cam)
{
  if (cam != nullptr && cam == m_overlayCamera) {
    CM_Warning("The dedicated overlay camera cannot also be the active scene camera");
    return;
  }
  m_active_camera = cam;
}

KX_Camera *KX_Scene::GetOverrideCullingCamera() const
{
  return m_overrideCullingCamera;
}

void KX_Scene::SetOverrideCullingCamera(KX_Camera *cam)
{
  m_overrideCullingCamera = cam;
}

void KX_Scene::SetCameraOnTop(KX_Camera *cam)
{
  // no release and addref just change camera place
  m_cameralist->RemoveValue(cam);
  m_cameralist->Add(cam);
}

void KX_Scene::PhysicsCullingCallback(KX_ClientObjectInfo *objectInfo, void *cullingInfo)
{
  KX_GameObject *gameobj = objectInfo->m_gameobject;
  if (!gameobj->GetVisible() || !gameobj->UseCulling()) {
    // ideally, invisible objects should be removed from the culling tree temporarily
    return;
  }
}

void KX_Scene::RenderDebugProperties(RAS_DebugDraw &debugDraw,
                                     int xindent,
                                     int ysize,
                                     int &xcoord,
                                     int &ycoord,
                                     unsigned short propsMax)
{
  static const MT_Vector4 white(1.0f, 1.0f, 1.0f, 1.0f);

  // The 'normal' debug props.
  const std::vector<SCA_DebugProp> &debugproplist = GetDebugProperties();

  unsigned short numprop = debugproplist.size();
  if (numprop > propsMax) {
    numprop = propsMax;
  }

  for (unsigned i = 0; i < numprop; ++i) {
    const SCA_DebugProp &debugProp = debugproplist[i];
    SCA_IObject *gameobj = debugProp.m_obj;
    const std::string objname = gameobj->GetName();
    const std::string &propname = debugProp.m_name;
    if (propname == "__state__") {
      // reserve name for object state
      unsigned int state = gameobj->GetState();
      std::string debugtxt = objname + "." + propname + " = ";
      bool first = true;
      for (int statenum = 1; state; state >>= 1, statenum++) {
        if (state & 1) {
          if (!first) {
            debugtxt += ",";
          }
          debugtxt += std::to_string(statenum);
          first = false;
        }
      }
      debugDraw.RenderText2D(debugtxt, MT_Vector2(xcoord + xindent, ycoord), white);
      ycoord += ysize;
    }
    else {
      EXP_Value *propval = gameobj->GetProperty(propname);
      if (propval) {
        const std::string text = propval->GetText();
        const std::string debugtxt = objname + ": '" + propname + "' = " + text;
        debugDraw.RenderText2D(debugtxt, MT_Vector2(xcoord + xindent, ycoord), white);
        ycoord += ysize;
      }
    }
  }
}

// logic stuff
void KX_Scene::LogicBeginFrame(double curtime, double framestep)
{
  // have a look at temp objects ...
  for (KX_GameObject *gameobj : m_tempObjectList) {
    EXP_FloatValue *propval = (EXP_FloatValue *)gameobj->GetProperty("::timebomb");

    if (propval) {
      const float timeleft = propval->GetNumber() - framestep;

      if (timeleft > 0) {
        propval->SetFloat(timeleft);
      }
      else {
        // remove obj, remove the object from tempObjectList in NewRemoveObject only.
        DelayedRemoveObject(gameobj, KX_ObjectRemovalReason::Lifetime);
      }
    }
    else {
      // all object is the tempObjectList should have a clock
      BLI_assert(false);
    }
  }
  m_logicmgr->BeginFrame(curtime, framestep);
}

#ifdef WITH_GAMEENGINE_LOGICNODES
void KX_Scene::LogicNodeCaptureInput()
{
  if (m_logicNodeManager) {
    m_logicNodeManager->CaptureInput();
  }
}

void KX_Scene::LogicNodeBeginFrame()
{
  if (m_logicNodeManager) {
    m_logicNodeManager->BeginFrame();
  }
}

void KX_Scene::LogicNodeUpdateFrame(const double curtime,
                                    const double scaledDeltaTime,
                                    const double unscaledDeltaTime,
                                    const bool useFixedPhysicsTimestep,
                                    const bool gamePaused)
{
  if (m_logicNodeManager) {
    m_logicNodeManager->Update(
        curtime, scaledDeltaTime, unscaledDeltaTime, useFixedPhysicsTimestep, gamePaused);
  }
  LogicNodeEndFrame();
  /* Text set by frame logic (UI routes, a paused menu) and {Action} names show this frame: logic
   * steps, which also update text objects, do not run while paused. */
  for (KX_FontObject *font : m_fontlist) {
    font->UpdateTextFromProperty();
  }
}
#endif

void KX_Scene::AddAnimatedObject(KX_GameObject *gameobj)
{
  CM_ListAddIfNotFound(m_animatedlist, gameobj);
}

void KX_Scene::UpdateAnimations(double curtime)
{
  m_animationUpdate = true;
  m_animationUpdateCount++;
  for (KX_GameObject *gameobj : m_animatedlist) {
    if (!gameobj->IsActionsSuspended()) {
      gameobj->UpdateActionManager(curtime, true);
    }
  }
  m_animationUpdate = false;
  m_animationDepsgraph = nullptr;
}

blender::Depsgraph *KX_Scene::GetAnimationDepsgraph()
{
  if (m_animationUpdate && m_animationDepsgraph == nullptr) {
    blender::bContext *C = KX_GetActiveEngine()->GetContext();
    m_animationDepsgraph = C ? CTX_data_depsgraph_pointer(C) : nullptr;
  }
  return m_animationDepsgraph;
}

#ifdef WITH_GAMEENGINE_LOGICNODES
void KX_Scene::UpdatePausedAnimations(const double pauseTime, const double normalTime)
{
  if (m_logicNodeManager == nullptr) {
    return;
  }

  std::unordered_set<KX_GameObject *> current_pause_animated_objects;
  current_pause_animated_objects.reserve(m_pauseAnimatedObjects.size());
  m_animationUpdate = true;
  m_animationUpdateCount++;
  for (KX_GameObject *gameobj : m_animatedlist) {
    if (gameobj == nullptr || !m_logicNodeManager->IsObjectPauseProcessed(gameobj)) {
      continue;
    }
    current_pause_animated_objects.insert(gameobj);
    gameobj->RebaseActionTime(float(normalTime), float(pauseTime), true);
    if (!gameobj->IsActionsSuspended()) {
      gameobj->UpdateActionManager(float(pauseTime), true);
    }
  }
  m_animationUpdate = false;
  m_animationDepsgraph = nullptr;

  for (KX_GameObject *gameobj : m_pauseAnimatedObjects) {
    if (gameobj != nullptr && !current_pause_animated_objects.contains(gameobj)) {
      gameobj->RebaseActionTime(float(pauseTime), float(normalTime), false);
    }
  }
  m_pauseAnimatedObjects.swap(current_pause_animated_objects);
}

void KX_Scene::RebasePausedAnimations(const double pauseTime, const double normalTime)
{
  for (KX_GameObject *gameobj : m_pauseAnimatedObjects) {
    if (gameobj != nullptr) {
      gameobj->RebaseActionTime(float(pauseTime), float(normalTime), false);
    }
  }
  m_pauseAnimatedObjects.clear();
}
#endif

void KX_Scene::LogicUpdateFrame(const double curtime,
                                const bool useFixedPhysicsTimestep,
                                const double fixedDt,
                                const double unscaledDt)
{
  m_proxyManager.Update();

#ifdef WITH_GAMEENGINE_LOGICNODES
  if (m_logicNodeManager) {
    if (useFixedPhysicsTimestep) {
      m_logicNodeManager->FixedUpdate(curtime, fixedDt, unscaledDt);
    }
  }
#else
  (void)useFixedPhysicsTimestep;
  (void)fixedDt;
  (void)unscaledDt;
#endif

  m_logicmgr->UpdateFrame(curtime);
}

#ifdef WITH_GAMEENGINE_LOGICNODES
LN_Manager *KX_Scene::GetLogicNodeManager() const
{
  return m_logicNodeManager.get();
}

void KX_Scene::ReplicateLogicNodeRuntimeTreesForReplicaMap()
{
  if (!m_logicNodeManager) {
    return;
  }

  const int object_count = m_objectlist ? m_objectlist->GetCount() : 0;
  const int appended_count = int(m_logicHierarchicalGameObjects.size());
  const int appended_begin = object_count >= appended_count ? object_count - appended_count : -1;

  static constexpr size_t inline_appended_scene_index_count = 8;
  std::array<std::pair<KX_GameObject *, uint32_t>, inline_appended_scene_index_count>
      inline_appended_scene_indices{};
  size_t inline_appended_scene_indices_count = 0;
  std::vector<std::pair<KX_GameObject *, uint32_t>> overflow_appended_scene_indices;
  if (appended_begin >= 0 && m_objectlist != nullptr) {
    for (int index = 0; index < appended_count; index++) {
      KX_GameObject *gameobj = m_logicHierarchicalGameObjects[size_t(index)];
      const int candidate = appended_begin + index;
      if (gameobj != nullptr && candidate < object_count &&
          m_objectlist->GetValue(candidate) == gameobj)
      {
        const std::pair<KX_GameObject *, uint32_t> entry{gameobj, uint32_t(candidate)};
        if (inline_appended_scene_indices_count < inline_appended_scene_indices.size()) {
          inline_appended_scene_indices[inline_appended_scene_indices_count++] = entry;
        }
        else {
          overflow_appended_scene_indices.push_back(entry);
        }
      }
    }
  }

  auto find_appended_scene_index = [&](KX_GameObject *gameobj) -> int {
    for (size_t index = 0; index < inline_appended_scene_indices_count; index++) {
      if (inline_appended_scene_indices[index].first == gameobj) {
        return int(inline_appended_scene_indices[index].second);
      }
    }
    for (const std::pair<KX_GameObject *, uint32_t> &entry : overflow_appended_scene_indices) {
      if (entry.first == gameobj) {
        return int(entry.second);
      }
    }
    return -1;
  };

  for (const std::pair<SCA_IObject *const, SCA_IObject *> &replica_pair :
       m_map_gameobject_to_replica)
  {
    KX_GameObject *source_gameobj = static_cast<KX_GameObject *>(replica_pair.first);
    KX_GameObject *replica_gameobj = static_cast<KX_GameObject *>(replica_pair.second);

    int scene_object_index = -1;
    scene_object_index = find_appended_scene_index(replica_gameobj);
    if (scene_object_index < 0 && m_objectlist != nullptr) {
      for (int i = 0; i < object_count; i++) {
        if (m_objectlist->GetValue(i) == replica_gameobj) {
          scene_object_index = i;
          break;
        }
      }
    }

    if (scene_object_index >= 0) {
      m_logicNodeManager->ReplicateRuntimeTrees(
          source_gameobj, replica_gameobj, static_cast<uint32_t>(scene_object_index));
    }
  }
}
#endif

void KX_Scene::LogicEndFrame()
{
  m_logicmgr->EndFrame();

#ifdef WITH_GAMEENGINE_LOGICNODES
  LogicNodeEndFrame();
#else
  /* Don't remove the objects from the euthanasy list here as the child objects of a deleted
   * parent object are destructed directly from the sgnode in the same time the parent
   * object is destructed. These child objects must be removed automatically from the
   * euthanasy list to avoid double deletion in case the user ask to delete the child object
   * explicitly. NewRemoveObject is the place to do it.
   */
  while (!m_euthanasyobjects.empty()) {
    KX_GameObject *gameobj = m_euthanasyobjects.front();
    const auto reason = m_euthanasyRemovalReasons.find(gameobj);
    RemoveObject(gameobj,
                 reason != m_euthanasyRemovalReasons.end() ? reason->second :
                                                             KX_ObjectRemovalReason::Gameplay);
  }
#endif

  // prepare obstacle simulation for new frame
  if (m_obstacleSimulation)
    m_obstacleSimulation->UpdateObstacles();

  for (KX_FontObject *font : m_fontlist) {
    font->UpdateTextFromProperty();
  }
}

#ifdef WITH_GAMEENGINE_LOGICNODES
void KX_Scene::LogicNodeEndFrame()
{
  if (m_logicNodeManager && !m_euthanasyobjects.empty()) {
    m_logicNodeManager->NotifyGameObjectsQueuedForBulkRemoval(m_euthanasyobjects);
  }

  /* Don't remove the objects from the euthanasy list here as the child objects of a deleted
   * parent object are destructed directly from the sgnode in the same time the parent
   * object is destructed. These child objects must be removed automatically from the
   * euthanasy list to avoid double deletion in case the user ask to delete the child object
   * explicitly. NewRemoveObject is the place to do it.
   */
  while (!m_euthanasyobjects.empty()) {
    KX_GameObject *gameobj = m_euthanasyobjects.front();
    const auto reason = m_euthanasyRemovalReasons.find(gameobj);
    RemoveObject(gameobj,
                 reason != m_euthanasyRemovalReasons.end() ? reason->second :
                                                             KX_ObjectRemovalReason::Gameplay);
  }
}
#endif

/**
 * UpdateParents: SceneGraph transformation update.
 */
void KX_Scene::UpdateParents(double curtime)
{
  // we use the SG dynamic list
  SG_Node *node;

  while ((node = SG_Node::GetNextScheduled(m_sghead)) != nullptr) {
    node->UpdateWorldData(curtime);
  }

  // the list must be empty here
  BLI_assert(m_sghead.Empty());
  // some nodes may be ready for reschedule, move them to schedule list for next time
  while ((node = SG_Node::GetNextRescheduled(m_sghead)) != nullptr) {
    node->Schedule(m_sghead);
  }
}

RAS_MaterialBucket *KX_Scene::FindBucket(class RAS_IPolyMaterial *polymat, bool &bucketCreated)
{
  return m_bucketmanager->FindBucket(polymat, bucketCreated);
}

void KX_Scene::UpdateObjectLods(KX_Camera *cam)
{
  /* Once a frame. Decided at each camera's pass, an object near one view and far from another (a
   * second player, a minimap, the menu camera) swapped its mesh, and with LOD Physics rebuilt its
   * physics shape, at every pass. The views are the cameras this frame draws (as
   * KX_KetsjiEngine::GetFrameRenderData picks them), not the menu camera. Due once a frame
   * (SetLodUpdateDue): stereo draws through copies of the cameras, which are in no list. */
  if (!m_lodUpdateDue || m_kxobWithLod.empty() || cam == nullptr || cam == GetOverlayCamera()) {
    return;
  }
  m_lodUpdateDue = false;
  KX_Camera *active = GetActiveCamera();
  std::vector<std::pair<MT_Vector3, float>> views;
  /* Split Screen Quality: in a view that shows objects half as big, half the distance counts. */
  const bool by_view_size = GetBlenderScene()->gm.split_flag & GAME_SPLIT_LOD_BY_VIEW_SIZE;
  for (KX_Camera *camera : *GetCameraList()) {
    if (camera == nullptr || camera == GetOverlayCamera() ||
        (camera != active && !camera->GetViewport()))
    {
      continue;
    }
    float factor = camera->GetLodDistanceFactor();
    if (by_view_size && camera->GetViewSizeScale() > 0.0f) {
      factor /= camera->GetViewSizeScale();
    }
    views.emplace_back(camera->NodeGetWorldPosition(), factor * factor);
  }
  if (views.empty()) {
    const float factor = cam->GetLodDistanceFactor();
    views.emplace_back(cam->NodeGetWorldPosition(), factor * factor);
  }

  for (KX_GameObject *gameobj : m_kxobWithLod) {
    const MT_Vector3 position = gameobj->NodeGetWorldPosition();
    float distance2 = std::numeric_limits<float>::max();
    for (const auto &[view_position, factor2] : views) {
      distance2 = std::min(distance2, position.distance2(view_position) * factor2);
    }
    gameobj->UpdateLod(distance2);
  }
}

void KX_Scene::SetLodHysteresis(bool active)
{
  m_isActivedHysteresis = active;
}

bool KX_Scene::IsActivedLodHysteresis(void)
{
  return m_isActivedHysteresis;
}

void KX_Scene::SetLodHysteresisValue(int hysteresisvalue)
{
  m_lodHysteresisValue = hysteresisvalue;
}

int KX_Scene::GetLodHysteresisValue(void)
{
  return m_lodHysteresisValue;
}

void KX_Scene::UpdateObjectActivity(void)
{
  if (!m_activityCulling) {
    return;
  }

  std::vector<MT_Vector3> camPositions;

  for (KX_Camera *cam : m_cameralist) {
    if (cam->GetActivityCulling()) {
      camPositions.push_back(cam->NodeGetWorldPosition());
    }
  }

  // None cameras are using object activity culling?
  if (camPositions.size() == 0) {
    return;
  }

  for (KX_GameObject *gameobj : m_objectlist) {
    // If the object doesn't manage activity culling we don't compute distance.
    if (gameobj->GetActivityCullingInfo().m_flags ==
        KX_GameObject::ActivityCullingInfo::ACTIVITY_NONE)
    {
      continue;
    }

    // For each camera compute the distance to objects and keep the minimum distance.
    const MT_Vector3 &obpos = gameobj->NodeGetWorldPosition();
    float dist = FLT_MAX;
    for (const MT_Vector3 &campos : camPositions) {
      // Keep the minimum distance.
      dist = min_ff((obpos - campos).length2(), dist);
    }
    gameobj->UpdateActivity(dist);
  }
}

KX_NetworkMessageScene *KX_Scene::GetNetworkMessageScene()
{
  return m_networkMessageScene;
}

void KX_Scene::SetNetworkMessageScene(KX_NetworkMessageScene *newScene)
{
  m_networkMessageScene = newScene;
}

void KX_Scene::SetGravity(const MT_Vector3 &gravity)
{
  GetPhysicsEnvironment()->SetGravity(gravity[0], gravity[1], gravity[2]);
}

MT_Vector3 KX_Scene::GetGravity()
{
  MT_Vector3 gravity;

  GetPhysicsEnvironment()->GetGravity(gravity);

  return gravity;
}

void KX_Scene::SetPhysicsEnvironment(class PHY_IPhysicsEnvironment *physEnv)
{
  m_physicsEnvironment = physEnv;
  if (m_physicsEnvironment) {
    KX_CollisionEventManager *collisionmgr = new KX_CollisionEventManager(m_logicmgr, physEnv);
    m_logicmgr->RegisterEventManager(collisionmgr);
  }
}

short KX_Scene::GetAnimationFPS()
{
  return m_blenderScene->r.frs_sec;
}

static void MergeScene_LogicBrick(SCA_ILogicBrick *brick, KX_Scene *from, KX_Scene *to)
{
  SCA_LogicManager *logicmgr = to->GetLogicManager();

  brick->Replace_IScene(to);
  brick->Replace_NetworkMessageScene(to->GetNetworkMessageScene());
  brick->SetLogicManager(to->GetLogicManager());

  // If we end up replacing a KX_CollisionEventManager, we need to make sure
  // physics controllers are properly in place. In other words, do this
  // after merging physics controllers!
  SCA_ISensor *sensor = dynamic_cast<class SCA_ISensor *>(brick);
  if (sensor) {
    sensor->Replace_EventManager(logicmgr);
  }

  SCA_2DFilterActuator *filter_actuator = dynamic_cast<class SCA_2DFilterActuator *>(brick);
  if (filter_actuator) {
    filter_actuator->SetScene(to, to->Get2DFilterManager());
  }
}

static void MergeScene_GameObject(KX_GameObject *gameobj, KX_Scene *to, KX_Scene *from)
{
  SCA_ActuatorList &actuators = gameobj->GetActuators();
  for (SCA_IActuator *actuator : actuators) {
    MergeScene_LogicBrick(actuator, from, to);
  }

  SCA_SensorList &sensors = gameobj->GetSensors();
  for (SCA_ISensor *sensor : sensors) {
    MergeScene_LogicBrick(sensor, from, to);
  }

  SCA_ControllerList &controllers = gameobj->GetControllers();
  for (SCA_IController *controller : controllers) {
    MergeScene_LogicBrick(controller, from, to);
  }

  /* graphics controller */
  PHY_IController *ctrl = gameobj->GetPhysicsController();
  if (ctrl) {
    ctrl->SetPhysicsEnvironment(to->GetPhysicsEnvironment());
  }

  /* SG_Node can hold a scene reference */
  SG_Node *sg = gameobj->GetSGNode();
  if (sg) {
    if (sg->GetSGClientInfo() == from) {
      sg->SetSGClientInfo(to);

      /* Make sure to grab the children too since they might not be tied to a game object */
      const NodeList &children = sg->GetSGChildren();
      for (SG_Node *child : children) {
        child->SetSGClientInfo(to);
      }
    }
  }

  // All armatures should be in the animated object list to be umpdated.
  if (gameobj->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE)
    to->AddAnimatedObject(gameobj);

  /* Add the object to the scene's logic manager */
  to->GetLogicManager()->RegisterGameObjectName(gameobj->GetName(), gameobj);
  to->GetLogicManager()->RegisterGameObj(gameobj->GetBlenderObject(), gameobj);

  for (int i = 0; i < gameobj->GetMeshCount(); ++i) {
    RAS_MeshObject *meshobj = gameobj->GetMesh(i);
    // Register the mesh object by name and blender object.
    to->GetLogicManager()->RegisterGameMeshName(meshobj->GetName(), gameobj->GetBlenderObject());
    to->GetLogicManager()->RegisterMeshName(meshobj->GetName(), meshobj);
  }
}

void KX_Scene::AdoptMergedScene(KX_Scene *other,
                                const std::vector<KX_GameObject *> &merged,
                                const std::vector<KX_GameObject *> &active)
{
  using namespace blender;
  bContext *C = KX_GetActiveEngine()->GetContext();
  Main *bmain = CTX_data_main(C);
  Scene *scene = GetBlenderScene();
  Scene *other_scene = other->GetBlenderScene();
  /* LibLoad's scenes live in a Main of their own, which no collection of this one may hold: they
   * stay as before (the other scene gives its data back when it is deleted). */
  bool in_main = false;
  for (Scene &candidate : bmain->scenes) {
    in_main |= &candidate == other_scene;
  }
  if (!in_main || other_scene == scene) {
    return;
  }

  /* What the game changed in Blender's data for those objects comes back at this scene's end, as
   * for its own objects (other's end would have given it back at once). */
  for (const auto &[ob, flag] : other->m_obVisibilityFlag) {
    m_obVisibilityFlag.insert({ob, flag});
  }
  other->m_obVisibilityFlag.clear();
  /* Start poses given back at the end follow this scene's Undo at Exit, as for its own objects
   * (the other scene's setting made them): kept, or made now (nothing moved since the
   * conversion), or freed. */
  const bool keep_poses = (scene->gm.flag & GAME_USE_UNDO) != 0;
  std::unordered_set<const Object *> backed_up;
  for (BackupObj *backup : other->m_backupObList) {
    if (keep_poses) {
      m_backupObList.push_back(backup);
      backed_up.insert(backup->ob);
    }
    else {
      MEM_delete_void(backup->obtfm);
      delete backup;
    }
  }
  other->m_backupObList.clear();
  if (keep_poses) {
    for (KX_GameObject *gameobj : merged) {
      Object *ob = gameobj->GetBlenderObject();
      if (ob == nullptr || gameobj->IsReplica() || gameobj->IsUpbgeDupliInstance() ||
          !backed_up.insert(ob).second)
      {
        continue;
      }
      BackupObj *backup = new BackupObj();
      backup->ob = ob;
      backup->obtfm = BKE_object_tfm_backup(ob);
      m_backupObList.push_back(backup);
    }
  }
  /* The lists this scene runs its objects' drawing from. */
  for (KX_GameObject *gameobj : other->m_kxobWithLod) {
    AddObjToLodObjList(gameobj);
  }
  other->m_kxobWithLod.clear();
  for (KX_GameObject *gameobj : other->m_upbgeDupliInstancesList) {
    gameobj->SetDupliListIndex(-1);
    AddUpbgeDupliInstanceToList(gameobj);
  }
  other->m_upbgeDupliInstancesList.clear();
  m_drawHiddenObjects.insert(
      m_drawHiddenObjects.end(), other->m_drawHiddenObjects.begin(), other->m_drawHiddenObjects.end());
  other->m_drawHiddenObjects.clear();
  m_replaceMeshEvalMeshes.insert(other->m_replaceMeshEvalMeshes.begin(),
                                 other->m_replaceMeshEvalMeshes.end());
  other->m_replaceMeshEvalMeshes.clear();
  /* Blender object -> game object: object pickers of its node trees, collection queries. */
  if (m_sceneConverter != nullptr && other->m_sceneConverter != nullptr) {
    m_sceneConverter->AdoptLookups(*other->m_sceneConverter);
  }

  /* Which objects this scene's graph draws as their own scene's did: those its view layer drew or
   * evaluated, with its eye. Decided before Game Drawing takes members out of the graph (their
   * own objects stay linked: a member leaving its group is drawn again). */
  ViewLayer *view_layer = BKE_view_layer_default_view(scene);
  ViewLayer *other_layer = BKE_view_layer_default_view(other_scene);
  BKE_view_layer_synced_ensure(*bmain, other_scene, other_layer);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  const std::unordered_set<KX_GameObject *> placed(active.begin(), active.end());
  struct Link {
    Object *object;
    bool hidden;
  };
  std::vector<Link> links;
  std::vector<Object *> dupli_bases;
  Set<const Object *> seen;
  for (KX_GameObject *gameobj : merged) {
    Object *ob = gameobj->GetBlenderObject();
    /* A Dupli copy draws from its base, which is one of the objects. */
    if (ob == nullptr || gameobj->IsUpbgeDupliInstance() || !seen.add(ob)) {
      continue;
    }
    /* In both scenes: this one draws it already. */
    if (BKE_view_layer_base_find(view_layer, ob) != nullptr) {
      continue;
    }
    const Base *base = BKE_view_layer_base_find(other_layer, ob);
    /* Not in its scene's view layer (an excluded collection, a collection instance's source). */
    if (base == nullptr) {
      continue;
    }
    if ((ob->gameflag & OB_DUPLI_UPBGE) != 0 && !placed.count(gameobj)) {
      dupli_bases.push_back(ob);
      continue;
    }
    /* Outside its scene's graph (a hidden object the conversion disabled, a disabled collection). */
    if ((base->flag & BASE_ENABLED_VIEWPORT) == 0) {
      continue;
    }
    links.push_back({ob, (base->flag & BASE_ENABLED_AND_VISIBLE_IN_DEFAULT_VIEWPORT) == 0});
  }

  /* Draw As Auto / Merged, as at the game's start: asked of the graph that holds them now, the
   * merged scene's (built by its conversion). */
  Depsgraph *other_graph = BKE_scene_get_depsgraph(other_scene, other_layer);
  if (other_graph == nullptr && CTX_data_scene(C) == other_scene) {
    other_graph = CTX_data_depsgraph_on_load(C);
  }
  if (other_graph != nullptr) {
    /* A grouped or merged object's own Blender object leaves the graph, so nothing may read it:
     * that graph answers for its own scene, this scene's for this one (a constraint or driver
     * aimed at an object of the other scene). */
    Depsgraph *graph = BKE_scene_get_depsgraph(scene, view_layer);
    std::vector<KX_GameObject *> groupable;
    for (KX_GameObject *gameobj : active) {
      bool read = false;
      if (graph != nullptr && gameobj->GetBlenderObject() != nullptr) {
        DEG_foreach_dependent_ID_component(
            graph,
            &gameobj->GetBlenderObject()->id,
            DEG_OB_COMP_ANY,
            DEG_FOREACH_COMPONENT_IGNORE_TRANSFORM_SOLVERS,
            [&](ID *id, eDepsObjectComponentType /*component*/) {
              if (GS(id->name) != ID_GR) {
                read = true;
              }
            });
      }
      if (!read) {
        groupable.push_back(gameobj);
      }
    }
    SetupGameDrawing(other_graph, groupable);
  }

  for (const Link &link : links) {
    if (BKE_collection_object_add(bmain, scene->master_collection, link.object)) {
      m_mergedObjectLinks.insert(link.object);
    }
  }
  if (!links.empty()) {
    BKE_main_collection_sync_remap(bmain);
    BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
    for (const Link &link : links) {
      if (!link.hidden || !m_mergedObjectLinks.count(link.object)) {
        continue;
      }
      if (Base *base = BKE_view_layer_base_find(view_layer, link.object)) {
        base->flag |= BASE_HIDDEN;
      }
    }
    BKE_view_layer_need_resync_tag(view_layer);
  }
  /* Evaluated for their copies, eye-hidden (KeepDupliBaseInGraph links them too). */
  for (Object *ob : dupli_bases) {
    if (!KeepDupliBaseInGraph(ob)) {
      CM_Warning("Dupli Base \"" << ob->id.name + 2 << "\" of merged scene \""
                                 << other_scene->id.name + 2
                                 << "\" could not be put in Blender's graph: its copies will not "
                                    "be drawn");
    }
  }
  if (std::find(m_mergedBlenderScenes.begin(), m_mergedBlenderScenes.end(), other_scene) ==
      m_mergedBlenderScenes.end())
  {
    m_mergedBlenderScenes.push_back(other_scene);
  }
  TagForCollectionRemap();
}

void KX_Scene::ForgetMergedLink(blender::Object *ob)
{
  m_mergedObjectLinks.erase(ob);
}

bool KX_Scene::MergeScene(KX_Scene *other)
{
  PHY_IPhysicsEnvironment *env = this->GetPhysicsEnvironment();
  PHY_IPhysicsEnvironment *env_other = other->GetPhysicsEnvironment();

  if ((env == nullptr) !=
      (env_other == nullptr)) /* TODO - even when both scenes have NONE physics, the other is
                                 loaded with bullet enabled, ??? */
  {
    CM_FunctionError("physics scenes type differ, aborting\n\tsource "
                     << (int)(env != nullptr) << ", target " << (int)(env_other != nullptr));
    return false;
  }

  GetBucketManager()->MergeBucketManager(other->GetBucketManager());

  /* For the drawing (AdoptMergedScene): all of its objects, and its placed ones. */
  std::vector<KX_GameObject *> merged_objects;
  std::vector<KX_GameObject *> placed_objects;
  for (KX_GameObject *gameobj : *other->GetObjectList()) {
    merged_objects.push_back(gameobj);
    placed_objects.push_back(gameobj);
  }
  for (KX_GameObject *gameobj : *other->GetInactiveList()) {
    merged_objects.push_back(gameobj);
  }

  /* active + inactive == all ??? - lets hope so */
  for (KX_GameObject *gameobj : *other->GetObjectList()) {
    MergeScene_GameObject(gameobj, this, other);

    /* add properties to debug list for LibLoad objects */
    if (KX_GetActiveEngine()->GetFlag(KX_KetsjiEngine::AUTO_ADD_DEBUG_PROPERTIES)) {
      AddObjectDebugProperties(gameobj);
    }
  }

  for (KX_GameObject *gameobj : *other->GetInactiveList()) {
    MergeScene_GameObject(gameobj, this, other);
  }

  if (env) {
    env->MergeEnvironment(env_other);
    EXP_ListValue<KX_GameObject> *otherObjects = other->GetObjectList();

    // List of all physics objects to merge (needed by ReplicateConstraints).
    std::vector<KX_GameObject *> physicsObjects;
    for (KX_GameObject *gameobj : *otherObjects) {
      if (gameobj->GetPhysicsController()) {
        physicsObjects.push_back(gameobj);
      }
    }

    for (unsigned int i = 0; i < physicsObjects.size(); ++i) {
      KX_GameObject *gameobj = physicsObjects[i];
      // Replicate all constraints in the right physics environment.
      gameobj->GetPhysicsController()->ReplicateConstraints(gameobj, physicsObjects);
      gameobj->ClearConstraints();
    }

    // Build lookup map for fast name resolution during rigid body constraint replication.
    std::unordered_map<std::string, KX_GameObject *> objectLookup;
    objectLookup.reserve(otherObjects->GetCount());
    for (KX_GameObject *gameobj : *otherObjects) {
      objectLookup.emplace(gameobj->GetName(), gameobj);
    }

    for (KX_GameObject *gameobj : *otherObjects) {
      if (!gameobj->HasRigidBodyConstraints()) {
        continue;
      }
      gameobj->ReplicateRigidBodyConstraints(objectLookup);
      // Don't clear - keep constraint IDs for cleanup when object is deleted
    }
  }

  /* The objects know this scene now (MergeScene_GameObject): their drawing becomes its own. */
  AdoptMergedScene(other, merged_objects, placed_objects);

  /* Their cached collections are stamped with the other scene's versions, which may equal this
   * scene's. */
  for (EXP_ListValue<KX_GameObject> *list : {other->GetObjectList(), other->GetInactiveList()}) {
    for (KX_GameObject *gameobj : *list) {
      gameobj->InvalidateCollectionMembership();
    }
  }
  GetObjectList()->MergeList(other->GetObjectList());
  other->GetObjectList()->ReleaseAndRemoveAll();
  NoteObjectListChanged();
  other->NoteObjectListChanged();
  if (other->HasSmoothMotion()) {
    NoteSmoothMotion();
  }

  GetInactiveList()->MergeList(other->GetInactiveList());
  other->GetInactiveList()->ReleaseAndRemoveAll();
  /* Merged objects keep their own scene's collections, which this scene's index now walks too
   * (m_mergedBlenderScenes). */
  NoteCollectionsChanged();

  GetRootParentList()->MergeList(other->GetRootParentList());
  other->GetRootParentList()->ReleaseAndRemoveAll();

  GetLightList()->MergeList(other->GetLightList());
  other->GetLightList()->ReleaseAndRemoveAll();

  GetCameraList()->MergeList(other->GetCameraList());
  other->GetCameraList()->ReleaseAndRemoveAll();

  GetFontList()->MergeList(other->GetFontList());
  other->GetFontList()->ReleaseAndRemoveAll();

#ifdef WITH_GAMEENGINE_LOGICNODES
  if (m_logicNodeManager && other->GetLogicNodeManager()) {
    m_logicNodeManager->AbsorbRuntimeTreesFrom(*other->GetLogicNodeManager());
  }
#endif

  /* move materials across, assume they both use the same scene-converters
   * Do this after lights are merged so materials can use the lights in shaders
   */
  KX_GetActiveEngine()->GetConverter()->MergeScene(this, other);

  /* merge logic */
  {
    SCA_LogicManager *logicmgr = GetLogicManager();
    SCA_LogicManager *logicmgr_other = other->GetLogicManager();

    std::vector<class SCA_EventManager *> evtmgrs = logicmgr->GetEventManagers();
    // vector<class SCA_EventManager*>evtmgrs_others= logicmgr_other->GetEventManagers();

    // SCA_EventManager *evtmgr;
    SCA_EventManager *evtmgr_other;

    for (unsigned int i = 0; i < evtmgrs.size(); i++) {
      evtmgr_other = logicmgr_other->FindEventManager(evtmgrs[i]->GetType());

      if (evtmgr_other) /* unlikely but possible one scene has a joystick and not the other */
        evtmgr_other->Replace_LogicManager(logicmgr);

      /* when merging objects sensors are moved across into the new manager, don't need to do this
       * here */
    }

    /* grab any timer properties from the other scene */
    SCA_TimeEventManager *timemgr = GetTimeEventManager();
    SCA_TimeEventManager *timemgr_other = other->GetTimeEventManager();
    std::vector<EXP_Value *> times = timemgr_other->GetTimeValues();

    for (unsigned int i = 0; i < times.size(); i++) {
      timemgr->AddTimeProperty(times[i]);
    }
  }
  return true;
}

RAS_2DFilterManager *KX_Scene::Get2DFilterManager() const
{
  return m_filterManager;
}

RAS_FrameBuffer *KX_Scene::Render2DFilters(RAS_Rasterizer *rasty,
                                           RAS_ICanvas *canvas,
                                           RAS_FrameBuffer *inputfb,
                                           RAS_FrameBuffer *targetfb)
{
  return m_filterManager->RenderFilters(rasty, canvas, inputfb, targetfb, this);
}

#ifdef WITH_PYTHON

void KX_Scene::RunDrawingCallbacks(DrawingCallbackType callbackType, KX_Camera *camera)
{
  PyObject *list = m_drawCallbacks[callbackType];
  if (!list || PyList_GET_SIZE(list) == 0) {
    return;
  }

  if (camera) {
    PyObject *args[1] = {camera->GetProxy()};
    EXP_RunPythonCallBackList(list, args, 0, 1);
  }
  else {
    EXP_RunPythonCallBackList(list, nullptr, 0, 0);
  }
}

void KX_Scene::RunOnRemoveCallbacks()
{
  PyObject *list = m_removeCallbacks;
  if (!list || PyList_GET_SIZE(list) == 0) {
    return;
  }

  PyObject *args[1] = {GetProxy()};
  EXP_RunPythonCallBackList(list, args, 0, 1);
}
#endif

#ifdef WITH_GAMEENGINE_LOGICNODES
void KX_Scene::ProcessReplica()
{
  KX_PythonProxy::ProcessReplica();
  /* Scene replicas are created with the default copy constructor before ProcessReplica() runs.
   * Do not call BeginShutdown() on the copied shared pointer here: it can still be the source
   * scene's manager. Resetting only detaches this replica; a uniquely owned stale manager will
   * still shut down through LN_Manager::~LN_Manager(). */
  m_logicNodeManager.reset();
  m_logicNodeManager = std::make_shared<LN_Manager>(*this);
}
#endif

KX_Scene *KX_Scene::NewInstance()
{
  KX_Scene *replica = new KX_Scene(*this);
  /* Its end counts like any scene's. */
  g_dupli_provider_scenes++;
  /* Runtime render state contains non-owning pointers owned by the source scene. A scene replica
   * establishes its own overlays, ImageRender registrations, viewport, and interpolation state. */
  replica->m_overlay_collections.clear();
  /* The source gives back what the game changed in Blender's data for it. */
  replica->m_dupliBasesInGraph.clear();
  replica->m_mergedObjectLinks.clear();
  replica->m_mergedBlenderScenes.clear();
  /* The source frees its retired Dupli draw identities. */
  replica->m_retiredDupliDrawOriginals.clear();
  /* Its own copies' groups. */
  replica->m_dupliDrawGroups.reset();
  replica->m_dupliGroupPass = 0;
  replica->m_directPoseObjects.clear();
  replica->m_poseCheckObjects.clear();
  replica->m_drawHiddenObjects.clear();
  replica->m_parkedCopies.clear();
  replica->m_sharedDrawBases.clear();
  replica->m_sharedDrawBaseSet.clear();
  replica->m_sharedMemberOfObject.clear();
  replica->m_mergedBatches.clear();
  replica->m_overlayCollectionReplicas.clear();
  replica->m_overlayCollectionMembers.clear();
  replica->m_overlayCollectionShownObjects.clear();
  replica->m_overlayMemberSources.clear();
  replica->m_overlayMemberInstance.clear();
  replica->m_overlayCopyOfSource.clear();
  replica->m_overlayCopyZeroRemoved.clear();
  replica->m_overlayCopyRefusedWarned.clear();
  replica->m_overlayInstanceLease.clear();
  replica->m_nextOverlayInstance = 1;
  replica->m_overlayRuntimeObjects.clear();
  replica->m_runtimeCollectionRenderVisibility.clear();
  replica->m_runtimeVisibleCollections.clear();
  replica->m_collectionIndex = CollectionIndex();
  replica->m_membershipVersion = next_cache_version(m_membershipVersion);
  replica->m_collectionMembersCache.clear();
  replica->m_overlayCamera = nullptr;
  replica->m_overlayCameraSource = nullptr;
  replica->m_overlayCameraReplicaRoot = nullptr;
  replica->m_overlayCameraReplicaMembers.clear();
  replica->m_imageRenderCameraList.clear();
  replica->m_playerViews = KX_PlayerViews();
  replica->m_cameraHiddenObjects.clear();
  replica->m_cameraHiddenRestore.clear();
  replica->m_cameraHiddenPass = nullptr;
  replica->m_cullPass = CullPass();
  replica->m_cullCameras.clear();
  replica->m_cullCameraPasses.clear();
  replica->m_cameraCollections.clear();
  replica->m_cameraCollectionsVersion = next_cache_version(m_cameraCollectionsVersion);
  replica->m_currentGPUViewport = nullptr;
  replica->m_sharedGPUViewports.clear();
  replica->m_idsToUpdateInAllRenderPasses.clear();
  replica->m_idsToUpdateInOverlayPass.clear();
  replica->m_interpolatedObjects.clear();
  replica->m_activeReplicaCaptures.clear();
  replica->m_activeReplicaMappingCaptures.clear();
  replica->m_euthanasyRemovalReasons.clear();
  replica->m_activeRemovalReason = KX_ObjectRemovalReason::SystemCleanup;
#ifdef WITH_GAMEENGINE_LOGICNODES
  replica->m_pauseAnimatedObjects.clear();
  /* The implicit scene copy constructor copies shared pointers. Drop the copied manager before
   * the replica can be observed; ProcessReplica() installs the replica-owned manager. */
  replica->m_logicNodeManager.reset();
#endif
  return replica;
}

#ifdef WITH_PYTHON
//----------------------------------------------------------------------------
// Python

PyTypeObject KX_Scene::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "KX_Scene",
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
                               0,
                               0,
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
                               &EXP_Value::Type,
                               0,
                               0,
                               0,
                               0,
                               0,
                               0,
                               py_base_new};

PyMethodDef KX_Scene::Methods[] = {
    EXP_PYMETHODTABLE(KX_Scene, addObject),
    EXP_PYMETHODTABLE(KX_Scene, end),
    EXP_PYMETHODTABLE(KX_Scene, restart),
    EXP_PYMETHODTABLE(KX_Scene, replace),
    EXP_PYMETHODTABLE(KX_Scene, drawObstacleSimulation),
    EXP_PYMETHODTABLE(KX_Scene, convertBlenderObject),
    EXP_PYMETHODTABLE(KX_Scene, convertBlenderObjectsList),
    EXP_PYMETHODTABLE(KX_Scene, convertBlenderCollection),
    EXP_PYMETHODTABLE(KX_Scene, convertBlenderAction),
    EXP_PYMETHODTABLE(KX_Scene, unregisterBlenderAction),
    EXP_PYMETHODTABLE(KX_Scene, addOverlayCollection),
    EXP_PYMETHODTABLE(KX_Scene, removeOverlayCollection),
    EXP_PYMETHODTABLE(KX_Scene, getGameObjectFromObject),

    /* dict style access */
    EXP_PYMETHODTABLE(KX_Scene, get),

    {nullptr, nullptr}  // Sentinel
};
static PyObject *Map_GetItem(PyObject *self_v, PyObject *item)
{
  KX_Scene *self = static_cast<KX_Scene *> EXP_PROXY_REF(self_v);
  const char *attr_str = _PyUnicode_AsString(item);
  PyObject *pyconvert;

  if (self == nullptr) {
    PyErr_SetString(PyExc_SystemError, "val = scene[key]: KX_Scene, " EXP_PROXY_ERROR_MSG);
    return nullptr;
  }

  if (!self->m_attr_dict)
    self->m_attr_dict = PyDict_New();

  if (self->m_attr_dict && (pyconvert = PyDict_GetItem(self->m_attr_dict, item))) {

    if (attr_str)
      PyErr_Clear();
    Py_INCREF(pyconvert);
    return pyconvert;
  }
  else {
    if (attr_str)
      PyErr_Format(
          PyExc_KeyError, "value = scene[key]: KX_Scene, key \"%s\" does not exist", attr_str);
    else
      PyErr_SetString(PyExc_KeyError, "value = scene[key]: KX_Scene, key does not exist");
    return nullptr;
  }
}

static int Map_SetItem(PyObject *self_v, PyObject *key, PyObject *val)
{
  KX_Scene *self = static_cast<KX_Scene *> EXP_PROXY_REF(self_v);
  const char *attr_str = _PyUnicode_AsString(key);
  if (attr_str == nullptr)
    PyErr_Clear();

  if (self == nullptr) {
    PyErr_SetString(PyExc_SystemError, "scene[key] = value: KX_Scene, " EXP_PROXY_ERROR_MSG);
    return -1;
  }

  if (!self->m_attr_dict)
    self->m_attr_dict = PyDict_New();

  if (val == nullptr) { /* del ob["key"] */
    int del = 0;

    if (self->m_attr_dict)
      del |= (PyDict_DelItem(self->m_attr_dict, key) == 0) ? 1 : 0;

    if (del == 0) {
      if (attr_str)
        PyErr_Format(
            PyExc_KeyError, "scene[key] = value: KX_Scene, key \"%s\" could not be set", attr_str);
      else
        PyErr_SetString(PyExc_KeyError, "del scene[key]: KX_Scene, key could not be deleted");
      return -1;
    }
    else if (self->m_attr_dict) {
      PyErr_Clear(); /* PyDict_DelItem sets an error when it fails */
    }
  }
  else { /* ob["key"] = value */
    int set = 0;

    if (self->m_attr_dict == nullptr) /* lazy init */
      self->m_attr_dict = PyDict_New();

    if (PyDict_SetItem(self->m_attr_dict, key, val) == 0)
      set = 1;
    else
      PyErr_SetString(PyExc_KeyError,
                      "scene[key] = value: KX_Scene, key not be added to internal dictionary");

    if (set == 0)
      return -1; /* pythons error value */
  }

  return 0; /* success */
}

static int Seq_Contains(PyObject *self_v, PyObject *value)
{
  KX_Scene *self = static_cast<KX_Scene *> EXP_PROXY_REF(self_v);

  if (self == nullptr) {
    PyErr_SetString(PyExc_SystemError, "val in scene: KX_Scene, " EXP_PROXY_ERROR_MSG);
    return -1;
  }

  if (!self->m_attr_dict)
    self->m_attr_dict = PyDict_New();

  if (self->m_attr_dict && PyDict_GetItem(self->m_attr_dict, value))
    return 1;

  return 0;
}

PyMappingMethods KX_Scene::Mapping = {
    (lenfunc) nullptr,          /* inquiry mp_length */
    (binaryfunc)Map_GetItem,    /* binaryfunc mp_subscript */
    (objobjargproc)Map_SetItem, /* objobjargproc mp_ass_subscript */
};

PySequenceMethods KX_Scene::Sequence = {
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

PyObject *KX_Scene::pyattr_get_name(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Scene *self = static_cast<KX_Scene *>(self_v);
  return PyUnicode_FromStdString(self->GetName());
}

PyObject *KX_Scene::pyattr_get_objects(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Scene *self = static_cast<KX_Scene *>(self_v);
  return self->GetObjectList()->GetProxy();
}

PyObject *KX_Scene::pyattr_get_objects_inactive(EXP_PyObjectPlus *self_v,
                                                const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Scene *self = static_cast<KX_Scene *>(self_v);
  return self->GetInactiveList()->GetProxy();
}

PyObject *KX_Scene::pyattr_get_lights(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Scene *self = static_cast<KX_Scene *>(self_v);
  return self->GetLightList()->GetProxy();
}

PyObject *KX_Scene::pyattr_get_filter_manager(EXP_PyObjectPlus *self_v,
                                              const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Scene *self = static_cast<KX_Scene *>(self_v);
  KX_2DFilterManager *filterManager = (KX_2DFilterManager *)self->Get2DFilterManager();

  return filterManager->GetProxy();
}

PyObject *KX_Scene::pyattr_get_texts(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Scene *self = static_cast<KX_Scene *>(self_v);
  return self->GetFontList()->GetProxy();
}

PyObject *KX_Scene::pyattr_get_cameras(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Scene *self = static_cast<KX_Scene *>(self_v);
  return self->GetCameraList()->GetProxy();
}

PyObject *KX_Scene::pyattr_get_active_camera(EXP_PyObjectPlus *self_v,
                                             const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Scene *self = static_cast<KX_Scene *>(self_v);
  KX_Camera *cam = self->GetActiveCamera();
  if (cam)
    return self->GetActiveCamera()->GetProxy();
  else
    Py_RETURN_NONE;
}

int KX_Scene::pyattr_set_active_camera(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef,
                                       PyObject *value)
{
  KX_Scene *self = static_cast<KX_Scene *>(self_v);
  KX_Camera *camOb;

  if (!ConvertPythonToCamera(self, value, &camOb, false, "scene.active_camera = value: KX_Scene"))
    return PY_SET_ATTR_FAIL;

  self->SetActiveCamera(camOb);
  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Scene::pyattr_get_overrideCullingCamera(EXP_PyObjectPlus *self_v,
                                                     const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Scene *self = static_cast<KX_Scene *>(self_v);
  KX_Camera *cam = self->GetOverrideCullingCamera();
  return (cam) ? cam->GetProxy() : Py_None;
}

int KX_Scene::pyattr_set_overrideCullingCamera(EXP_PyObjectPlus *self_v,
                                               const EXP_PYATTRIBUTE_DEF *attrdef,
                                               PyObject *value)
{
  KX_Scene *self = static_cast<KX_Scene *>(self_v);
  KX_Camera *cam;

  if (!ConvertPythonToCamera(self, value, &cam, true, "scene.active_camera = value: KX_Scene")) {
    return PY_SET_ATTR_FAIL;
  }

  self->SetOverrideCullingCamera(cam);
  return PY_SET_ATTR_SUCCESS;
}

static std::map<const std::string, KX_Scene::DrawingCallbackType> callbacksTable = {
    {"pre_draw", KX_Scene::PRE_DRAW},
    {"pre_draw_setup", KX_Scene::PRE_DRAW_SETUP},
    {"post_draw", KX_Scene::POST_DRAW}};

PyObject *KX_Scene::pyattr_get_drawing_callback(EXP_PyObjectPlus *self_v,
                                                const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Scene *self = static_cast<KX_Scene *>(self_v);

  const DrawingCallbackType type = callbacksTable[attrdef->m_name];
  if (!self->m_drawCallbacks[type]) {
    self->m_drawCallbacks[type] = PyList_New(0);
  }

  Py_INCREF(self->m_drawCallbacks[type]);

  return self->m_drawCallbacks[type];
}

int KX_Scene::pyattr_set_drawing_callback(EXP_PyObjectPlus *self_v,
                                          const EXP_PYATTRIBUTE_DEF *attrdef,
                                          PyObject *value)
{
  KX_Scene *self = static_cast<KX_Scene *>(self_v);

  if (!PyList_CheckExact(value)) {
    PyErr_SetString(PyExc_ValueError, "Expected a list");
    return PY_SET_ATTR_FAIL;
  }

  const DrawingCallbackType type = callbacksTable[attrdef->m_name];

  Py_XDECREF(self->m_drawCallbacks[type]);

  Py_INCREF(value);
  self->m_drawCallbacks[type] = value;

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Scene::pyattr_get_remove_callback(EXP_PyObjectPlus *self_v,
                                               const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Scene *self = static_cast<KX_Scene *>(self_v);

  if (!self->m_removeCallbacks) {
    self->m_removeCallbacks = PyList_New(0);
  }

  Py_INCREF(self->m_removeCallbacks);

  return self->m_removeCallbacks;
}

int KX_Scene::pyattr_set_remove_callback(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef,
                                         PyObject *value)
{
  KX_Scene *self = static_cast<KX_Scene *>(self_v);

  if (!PyList_CheckExact(value)) {
    PyErr_SetString(PyExc_ValueError, "Expected a list");
    return PY_SET_ATTR_FAIL;
  }

  Py_XDECREF(self->m_removeCallbacks);

  Py_INCREF(value);
  self->m_removeCallbacks = value;

  return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Scene::pyattr_get_gravity(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef)
{
  KX_Scene *self = static_cast<KX_Scene *>(self_v);

  return PyObjectFrom(self->GetGravity());
}

int KX_Scene::pyattr_set_gravity(EXP_PyObjectPlus *self_v,
                                 const EXP_PYATTRIBUTE_DEF *attrdef,
                                 PyObject *value)
{
  KX_Scene *self = static_cast<KX_Scene *>(self_v);

  MT_Vector3 vec;
  if (!PyVecTo(value, vec))
    return PY_SET_ATTR_FAIL;

  self->SetGravity(vec);
  return PY_SET_ATTR_SUCCESS;
}

PyAttributeDef KX_Scene::Attributes[] = {
    EXP_PYATTRIBUTE_RO_FUNCTION("name", KX_Scene, pyattr_get_name),
    EXP_PYATTRIBUTE_RO_FUNCTION("objects", KX_Scene, pyattr_get_objects),
    EXP_PYATTRIBUTE_RO_FUNCTION("objectsInactive", KX_Scene, pyattr_get_objects_inactive),
    EXP_PYATTRIBUTE_RO_FUNCTION("lights", KX_Scene, pyattr_get_lights),
    EXP_PYATTRIBUTE_RO_FUNCTION("texts", KX_Scene, pyattr_get_texts),
    EXP_PYATTRIBUTE_RO_FUNCTION("cameras", KX_Scene, pyattr_get_cameras),
    EXP_PYATTRIBUTE_RO_FUNCTION("filterManager", KX_Scene, pyattr_get_filter_manager),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "active_camera", KX_Scene, pyattr_get_active_camera, pyattr_set_active_camera),
    EXP_PYATTRIBUTE_RW_FUNCTION("overrideCullingCamera",
                                KX_Scene,
                                pyattr_get_overrideCullingCamera,
                                pyattr_set_overrideCullingCamera),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "pre_draw", KX_Scene, pyattr_get_drawing_callback, pyattr_set_drawing_callback),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "onRemove", KX_Scene, pyattr_get_remove_callback, pyattr_set_remove_callback),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "post_draw", KX_Scene, pyattr_get_drawing_callback, pyattr_set_drawing_callback),
    EXP_PYATTRIBUTE_RW_FUNCTION(
        "pre_draw_setup", KX_Scene, pyattr_get_drawing_callback, pyattr_set_drawing_callback),
    EXP_PYATTRIBUTE_RW_FUNCTION("gravity", KX_Scene, pyattr_get_gravity, pyattr_set_gravity),
    EXP_PYATTRIBUTE_BOOL_RO("activityCulling", KX_Scene, m_activityCulling),
    EXP_PYATTRIBUTE_BOOL_RO("dbvt_culling", KX_Scene, m_dbvt_culling),
    EXP_PYATTRIBUTE_RO_FUNCTION("logger", KX_Scene, KX_PythonProxy::pyattr_get_logger),
    EXP_PYATTRIBUTE_RO_FUNCTION("loggerName", KX_Scene, KX_PythonProxy::pyattr_get_logger_name),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

EXP_PYMETHODDEF_DOC(KX_Scene,
                    addObject,
                    "addObject(object, other, time=0, fullCopy=0)\n"
                    "Returns the added object.\n")
{
  PyObject *pyob, *pyreference = Py_None;
  KX_GameObject *ob, *reference;

  float time = 0.0f;

  // Full duplication of ob->data
  int fullCopy = 0;

  if (!PyArg_ParseTuple(args, "O|Ofi:addObject", &pyob, &pyreference, &time, &fullCopy))
    return nullptr;

  if (!ConvertPythonToGameObject(
          m_logicmgr,
          pyob,
          &ob,
          false,
          "scene.addObject(object, reference, time, fullCopy): KX_Scene (first argument)") ||
      !ConvertPythonToGameObject(
          m_logicmgr,
          pyreference,
          &reference,
          true,
          "scene.addObject(object, reference, time, fullCopy): KX_Scene (second argument)"))
    return nullptr;

  if (!m_inactivelist->SearchValue(ob)) {
    PyErr_Format(
        PyExc_ValueError,
        "scene.addObject(object, reference, time, fullCopy): KX_Scene (first argument): object "
        "must be in an inactive layer");
    return nullptr;
  }
  bool deepCopy = fullCopy == 1;
  const CopyReuseScope reuse(*this);
  KX_GameObject *replica = !deepCopy ? AddReplicaObject(ob, reference, time) :
                                       AddFullCopyObject(ob, reference, time);

  /* Can happen when trying to Duplicate an instance_collection */
  if (replica == nullptr) {
    Py_RETURN_NONE;
  }

  // release here because AddReplicaObject AddRef's
  // the object is added to the scene so we don't want python to own a reference
  if (!deepCopy) {
    replica->Release();
  }
  return replica->GetProxy();
}

EXP_PYMETHODDEF_DOC(KX_Scene,
                    end,
                    "end()\n"
                    "Removes this scene from the game.\n")
{

  KX_GetActiveEngine()->RemoveScene(m_sceneName);

  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_Scene,
                    restart,
                    "restart()\n"
                    "Restarts this scene.\n")
{
  KX_GetActiveEngine()->ReplaceScene(m_sceneName, m_sceneName);

  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(
    KX_Scene,
    replace,
    "replace(newScene)\n"
    "Replaces this scene with another one.\n"
    "Return True if the new scene exists and scheduled for replacement, False otherwise.\n")
{
  char *name = (char *)"";

  if (!PyArg_ParseTuple(args, "s:replace", &name))
    return nullptr;

  if (KX_GetActiveEngine()->ReplaceScene(m_sceneName, name))
    Py_RETURN_TRUE;

  Py_RETURN_FALSE;
}

EXP_PYMETHODDEF_DOC(KX_Scene,
                    drawObstacleSimulation,
                    "drawObstacleSimulation()\n"
                    "Draw debug visualization of obstacle simulation.\n")
{
  if (GetObstacleSimulation())
    GetObstacleSimulation()->DrawObstacles();

  Py_RETURN_NONE;
}

/* Matches python dict.get(key, [default]) */
EXP_PYMETHODDEF_DOC(KX_Scene, get, "")
{
  PyObject *key;
  PyObject *def = Py_None;
  PyObject *ret;

  if (!PyArg_ParseTuple(args, "O|O:get", &key, &def))
    return nullptr;

  if (m_attr_dict && (ret = PyDict_GetItem(m_attr_dict, key))) {
    Py_INCREF(ret);
    return ret;
  }

  Py_INCREF(def);
  return def;
}

EXP_PYMETHODDEF_DOC(KX_Scene,
                    convertBlenderObject,
                    "convertBlenderObject()\n"
                    "\n")
{
  PyObject *bl_object = Py_None;

  if (!PyArg_ParseTuple(args, "O:", &bl_object)) {
    PyErr_SetString(PyExc_TypeError,
                    "convertBlenderObject: Expected a bpy.types.blender::Object.");
    return nullptr;
  }

  blender::ID *id;
  if (!pyrna_id_FromPyObject(bl_object, &id)) {
    PyErr_SetString(PyExc_RuntimeError, "convertBlenderObject: Failed to convert object.");
    return nullptr;
  }
  blender::Object *ob = (blender::Object *)id;
  ConvertBlenderObject(ob);
  /* The object may come with collections the index doesn't know (Add Object full copies convert
   * through ConvertBlenderObject too, and keep their source's collections). */
  NoteCollectionsChanged();
  KX_GameObject *newgameobj = m_sceneConverter->FindGameObject(ob);
  if (!newgameobj) {
    /* It can happen for example if we are trying to convert the same object several times
     * or if we are trying to convert game_default_cam https://github.com/UPBGE/upbge/issues/1847
     */
    PyErr_Format(PyExc_RuntimeError,
                 "convertBlenderObject: blender::Scene converter failed to convert %s: ",
                 ob->id.name + 2);
    Py_RETURN_NONE;
  }
  return newgameobj->GetProxy();
}

EXP_PYMETHODDEF_DOC(KX_Scene,
                    convertBlenderObjectsList,
                    "convertBlenderObjectsList(objects, asynchronous=False)\n"
                    "\n")
{
  PyObject *list;
  int asynchronous = 0;

  static const char *kwlist[] = {"objects", "asynchronous", nullptr};

  if (!PyArg_ParseTupleAndKeywords(
          args, kwds, "O!|i:", const_cast<char **>(kwlist), &PyList_Type, &list, &asynchronous))
  {
    PyErr_SetString(PyExc_TypeError,
                    "convertBlenderObjectsList(objects, asynchronous=False): Expected a "
                    "bpy.types.blender::Object list.");
    return nullptr;
  }

  std::vector<blender::Object *> objectslist;
  Py_ssize_t list_size = PyList_Size(list);

  for (Py_ssize_t i = 0; i < list_size; i++) {
    PyObject *bl_object = PyList_GetItem(list, i);

    blender::ID *id;
    if (!pyrna_id_FromPyObject(bl_object, &id)) {
      PyErr_SetString(
          PyExc_RuntimeError,
          "convertBlenderObjectsList(objects, asynchronous=False): Failed to convert object.");
      return nullptr;
    }

    blender::Object *ob = (blender::Object *)id;
    objectslist.push_back(ob);
  }

  ConvertBlenderObjectsList(objectslist, asynchronous);
  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_Scene,
                    convertBlenderCollection,
                    "convertBlenderCollection(collection, asynchronous=False)\n"
                    "\n")
{
  PyObject *bl_collection = Py_None;
  int asynchronous = 0;

  static const char *kwlist[] = {"collection", "asynchronous", nullptr};

  if (!PyArg_ParseTupleAndKeywords(
          args, kwds, "O|i:", const_cast<char **>(kwlist), &bl_collection, &asynchronous))
  {
    PyErr_SetString(PyExc_TypeError,
                    "convertBlenderCollection(collection, asynchronous=False): Expected a "
                    "bpy.types.blender::Collection.");
    return nullptr;
  }

  blender::ID *id;
  if (!pyrna_id_FromPyObject(bl_collection, &id)) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "convertBlenderCollection(collection, asynchronous=False): Failed to convert collection.");
    return nullptr;
  }

  blender::Collection *co = (blender::Collection *)id;
  ConvertBlenderCollection(co, asynchronous);
  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_Scene,
                    convertBlenderAction,
                    "convertBlenderAction(bpy.types.Action)\n"
                    "\n")
{
  PyObject *bl_action = Py_None;

  if (!PyArg_ParseTuple(args, "O:", &bl_action)) {
    PyErr_SetString(PyExc_TypeError, "convertBlenderAction: Expected a bpy.types.Action.");
    return nullptr;
  }

  blender::ID *id;
  if (!pyrna_id_FromPyObject(bl_action, &id)) {
    PyErr_SetString(PyExc_RuntimeError, "convertBlenderAction: Failed to convert action.");
    return nullptr;
  }

  blender::bAction *act = (blender::bAction *)id;
  ConvertBlenderAction(act);
  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_Scene,
                    unregisterBlenderAction,
                    "unregisterBlenderAction(bpy.types.Action)\n"
                    "\n")
{
  PyObject *bl_action = Py_None;

  if (!PyArg_ParseTuple(args, "O:", &bl_action)) {
    PyErr_SetString(PyExc_TypeError, "unregisterBlenderAction: Expected a bpy.types.Action.");
    return nullptr;
  }

  blender::ID *id;
  if (!pyrna_id_FromPyObject(bl_action, &id)) {
    PyErr_SetString(PyExc_RuntimeError,
                    "unregisterBlenderAction: Failed to find action to unregister.");
    return nullptr;
  }

  blender::bAction *act = (blender::bAction *)id;
  // Now unregister actions.
  std::map<std::string, void *>::iterator it = GetLogicManager()->GetActionMap().find(
      act->id.name + 2);
  std::map<std::string, void *> &mapStringToActions = GetLogicManager()->GetActionMap();
  if (it != mapStringToActions.end()) {
    mapStringToActions.erase(it);
  }
  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_Scene,
                    addOverlayCollection,
                    "addOverlayCollection(KX_Camera *cam, blender::Collection *col)\n"
                    "\n")
{
  PyObject *pyCamera = Py_None;
  PyObject *pyCollection = Py_None;

  if (!PyArg_ParseTuple(args, "OO:", &pyCamera, &pyCollection)) {
    PyErr_SetString(
        PyExc_TypeError,
        "addOverlayCollection: Expected a KX_Camera and a bpy.types.blender::Collection.");
    return nullptr;
  }

  KX_Camera *kxCam = nullptr;
  if (!(ConvertPythonToCamera(this, pyCamera, &kxCam, false, nullptr))) {
    PyErr_SetString(PyExc_RuntimeError, "addOverlayCollection: Failed to convert KX_Camera");
    return nullptr;
  }

  blender::ID *id = nullptr;
  if (!pyrna_id_FromPyObject(pyCollection, &id)) {
    PyErr_SetString(PyExc_RuntimeError, "addOverlayCollection: Failed to convert collection.");
    return nullptr;
  }

  blender::Collection *co = (blender::Collection *)id;
  AddOverlayCollection(kxCam, co);
  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_Scene,
                    removeOverlayCollection,
                    "removeOverlayCollection(blender::Collection *col)\n"
                    "\n")
{
  PyObject *pyCollection = Py_None;

  if (!PyArg_ParseTuple(args, "O:", &pyCollection)) {
    PyErr_SetString(PyExc_TypeError,
                    "removeOverlayCollection: Expected a bpy.types.blender::Collection.");
    return nullptr;
  }

  blender::ID *id = nullptr;
  if (!pyrna_id_FromPyObject(pyCollection, &id)) {
    PyErr_SetString(PyExc_RuntimeError, "removeOverlayCollection: Failed to convert collection.");
    return nullptr;
  }

  blender::Collection *co = (blender::Collection *)id;
  RemoveOverlayCollection(co);
  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_Scene,
                    getGameObjectFromObject,
                    "getGameObjectFromObject(blender::Object *ob)\n"
                    "\n")
{
  PyObject *pyBlenderObject = Py_None;

  if (!PyArg_ParseTuple(args, "O:", &pyBlenderObject)) {
    PyErr_SetString(PyExc_TypeError,
                    "getGameObjectFromObject: Expected a bpy.types.blender::Object.");
    return nullptr;
  }

  blender::ID *id = nullptr;
  if (!pyrna_id_FromPyObject(pyBlenderObject, &id)) {
    PyErr_Format(PyExc_RuntimeError,
                 "getGameObjectFromObject: Failed to convert blender::Object %s: ",
                 id->name + 2);
    return nullptr;
  }

  blender::Object *ob = (blender::Object *)id;
  if (ob) {
    KX_GameObject *gameobj = GetGameObjectFromObject(ob);
    if (gameobj->IsUpbgeDupliInstance()) {
      PyErr_Format(
          PyExc_ValueError,
          "getGameObjectFromObject: Can't be used with a dupli instance of dupli Base %s: ",
          ob->id.name + 2);
      Py_RETURN_NONE;
    }
    if (gameobj) {
      return gameobj->GetProxy();
    }
    PyErr_Format(PyExc_ValueError,
                 "getGameObjectFromObject: No KX_GameObject found for this blender::Object %s ",
                 ob->id.name + 2);
    Py_RETURN_NONE;
  }

  Py_RETURN_NONE;
}

bool ConvertPythonToScene(PyObject *value,
                          KX_Scene **scene,
                          bool py_none_ok,
                          const char *error_prefix)
{
  if (value == nullptr) {
    PyErr_Format(PyExc_TypeError, "%s, python pointer nullptr, should never happen", error_prefix);
    *scene = nullptr;
    return false;
  }

  if (value == Py_None) {
    *scene = nullptr;

    if (py_none_ok) {
      return true;
    }
    else {
      PyErr_Format(PyExc_TypeError,
                   "%s, expected KX_Scene or a KX_Scene name, None is invalid",
                   error_prefix);
      return false;
    }
  }

  if (PyUnicode_Check(value)) {
    *scene = (KX_Scene *)KX_GetActiveEngine()->CurrentScenes()->FindValue(
        std::string(_PyUnicode_AsString(value)));

    if (*scene) {
      return true;
    }
    else {
      PyErr_Format(PyExc_ValueError,
                   "%s, requested name \"%s\" did not match any in game",
                   error_prefix,
                   _PyUnicode_AsString(value));
      return false;
    }
  }

  if (PyObject_TypeCheck(value, &KX_Scene::Type)) {
    *scene = static_cast<KX_Scene *> EXP_PROXY_REF(value);

    // Sets the error.
    if (*scene == nullptr) {
      PyErr_Format(PyExc_SystemError, "%s, " EXP_PROXY_ERROR_MSG, error_prefix);
      return false;
    }

    return true;
  }

  *scene = nullptr;

  if (py_none_ok) {
    PyErr_Format(PyExc_TypeError, "%s, expect a KX_Scene, a string or None", error_prefix);
  }
  else {
    PyErr_Format(PyExc_TypeError, "%s, expect a KX_Scene or a string", error_prefix);
  }

  return false;
}

#endif  // WITH_PYTHON
