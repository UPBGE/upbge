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

#include "BKE_duplilist.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_mball.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BKE_scene.hh"
#include "BKE_screen.hh"
#include "BLI_math_matrix.h"
#include "BLI_task.h"
#include "DEG_depsgraph_query.hh"
#include "DNA_camera_types.h"
#include "DNA_collection_types.h"
#include "DNA_mesh_types.h"
#include "DNA_property_types.h"
#include "DNA_rigidbody_types.h"
#include "DRW_render.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"
#include "GPU_context.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"
#include "GPU_viewport.hh"
#include "WM_api.hh"
#include "wm_draw.hh"
#include "wm_event_system.hh"
#include "xr/wm_xr.hh"

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

#ifdef WITH_PYTHON
#  include "EXP_PythonCallBack.h"
#  include "bpy_rna.hh"
#endif

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
                   Scene *scene,
                   class RAS_ICanvas *canvas,
                   KX_NetworkMessageManager *messageManager)
    : KX_PythonProxy(),
      m_gameDefaultCamera(nullptr),           // eevee
      m_currentGPUViewport(nullptr),          // eevee
      m_initMaterialsGPUViewport(nullptr),    // eevee (See comment in .h)
      m_overlayCamera(nullptr),               // eevee (For overlay collections)
      m_sceneConverter(nullptr),              // eevee
      m_isPythonMainLoop(false),              // eevee
      m_collectionRemap(false),               // eevee (to uncheck viewport restrictflag)
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

  m_filterManager = new KX_2DFilterManager();
  m_logicmgr = new SCA_LogicManager();

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

  m_networkScene = new KX_NetworkMessageScene(messageManager);

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

  m_animationPool = BLI_task_pool_create(&m_animationPoolData, TASK_PRIORITY_LOW);

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
  m_obRestrictFlags = {};
  m_backupOverlayFlag = -1;
  m_backupOverlayGameFlag = -1;

  /* REMINDER TO SET bContext */
  /* 1.MAIN, 2.wmWindowManager, 3.wmWindow, 4.bScreen, 5.ScreenArea, 6.ARegion, 7.Scene */

  /* In the case of SetScene actuator (not game restart or load .blend)
   * We might need to Set bContext Variables here to be sure to have
   * the good environment.
   */
  ReinitBlenderContextVariables();

  bContext *C = KX_GetActiveEngine()->GetContext();
  Main *bmain = CTX_data_main(C);

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
  m_gameDefaultCamera->data = BKE_object_obdata_add_from_type(bmain, OB_CAMERA, NULL);
  BKE_collection_object_add(bmain, scene->master_collection, m_gameDefaultCamera);
  /* Fix crash at start with some files: See 68589a31ebfb79165f99a979357d237e5413e904 */
  BKE_view_layer_synced_ensure(scene, view_layer);
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
  CTX_wm_view3d(C)->shading.type = KX_GetActiveEngine()->ShadingTypeRuntime();

  if (!KX_GetActiveEngine()->UseViewportRender()) {
    /* We want to indicate that we are in bge runtime. The flag can be used in draw code but in
     * depsgraph code too later */
    scene->flag |= SCE_INTERACTIVE;

    bool is_vulkan_backend = GPU_backend_get_type() == GPU_BACKEND_VULKAN;
    if (!is_vulkan_backend) {
      /* We call Render here in KX_Scene constructor because
       * 1: It creates a depsgraph and ensure it will be activated.
       * 2: We need to create an eevee's cache to initialize
       * KX_BlenderMaterials and BL_Textures.
       */
      const RAS_Rect &viewport = KX_GetActiveEngine()->GetCanvas()->GetViewportArea();
      RenderAfterCameraSetup(nullptr, nullptr, viewport, false, true);
    }
  }
  else {
    scene->flag |= SCE_INTERACTIVE_VIEWPORT;
  }

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

  Scene *scene = GetBlenderScene();
  ViewLayer *view_layer = BKE_view_layer_default_view(scene);
  bContext *C = KX_GetActiveEngine()->GetContext();
  Main *bmain = CTX_data_main(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_on_load(C);

  /* Restore Objects object_to_world mat */
  if (scene->gm.flag & GAME_USE_UNDO) {
    RestoreObjectsMatToWorld();
    TagForObjectsMatToWorldRestore();
  }
  /*************************/

  if (!KX_GetActiveEngine()->UseViewportRender()) {
    if (!m_isPythonMainLoop) {
      /* This will free m_gpuViewport and m_gpuOffScreen */
      DRW_game_render_loop_end();
    }
    else {
      /* If we are in python loop and we called render code */
      if (!m_initMaterialsGPUViewport) {
        DRW_game_render_loop_end();
      }
      else {
        /* It has not been freed before because the main Render loop
         * is not executed then we free it now.
         */
        GPU_viewport_free(m_initMaterialsGPUViewport);
        //DRW_game_python_loop_end(DEG_get_evaluated_view_layer(depsgraph));
      }
    }
    DRW_game_gpu_viewport_set(nullptr);
  }
  else {
    // Free the allocated profile a last time
    DRW_game_viewport_render_loop_end(GetBlenderScene());
  }

  /* Fixes issue when switching .blend erm...*/
  GPU_shader_force_unbind();

  for (Object *hiddenOb : m_hiddenObjectsDuringRuntime) {
    Base *base = BKE_view_layer_base_find(view_layer, hiddenOb);
    base->flag &= ~BASE_HIDDEN;
    BKE_layer_collection_sync(scene, view_layer);
    DEG_id_tag_update(&scene->id, ID_RECALC_BASE_FLAGS);
  }

  // Put that before we flush depsgraph updates at scene exit
  scene->flag &= ~SCE_INTERACTIVE;
  scene->flag &= ~SCE_INTERACTIVE_VIEWPORT;

  /* End of EEVEE INTEGRATION */

  // The release of debug properties used to be in SCA_IScene::~SCA_IScene
  // It's still there but we remove all properties here otherwise some
  // reference might be hanging and causing late release of objects
  RemoveAllDebugProperties();

  while (GetRootParentList()->GetCount() > 0) {
    KX_GameObject *parentobj = GetRootParentList()->GetValue(0);
    this->RemoveObject(parentobj);
    BKE_view_layer_synced_ensure(scene, BKE_view_layer_default_view(scene));
  }

  if (m_obstacleSimulation)
    delete m_obstacleSimulation;

  if (m_animationPool) {
    BLI_task_pool_free(m_animationPool);
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

  if (m_filterManager) {
    delete m_filterManager;
  }

  if (m_logicmgr)
    delete m_logicmgr;

  if (m_physicsEnvironment)
    delete m_physicsEnvironment;

  if (m_networkScene)
    delete m_networkScene;

  if (m_bucketmanager) {
    delete m_bucketmanager;
  }
  if (m_sceneConverter) {
    delete m_sceneConverter;
  }

  RestoreRestrictFlags();
  m_obRestrictFlags.clear();

  // Flush depsgraph updates a last time at ge exit
  BKE_scene_graph_update_tagged(depsgraph, bmain);

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

void KX_Scene::ReinitBlenderContextVariables()
{
  bContext *C = KX_GetActiveEngine()->GetContext();
  wmWindowManager *wm = CTX_wm_manager(C);
  wmWindow *win = (wmWindow *)wm->windows.first;
  bScreen *screen = WM_window_get_active_screen(win);

  LISTBASE_FOREACH (ScrArea *, sa, &screen->areabase) {
    /* We choose the biggest ScrArea to match the behaviour in WM_init_game */
    if (sa->spacetype == SPACE_VIEW3D &&
        sa == BKE_screen_find_big_area(screen, SPACE_VIEW3D, 0)) {
      ListBase *regionbase = &sa->regionbase;
      LISTBASE_FOREACH (ARegion *, region, regionbase) {
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

Object *KX_Scene::GetGameDefaultCamera()
{
  return m_gameDefaultCamera;
}

void KX_Scene::AddOverlayCollection(KX_Camera *overlay_cam, Collection *collection)
{
  /* Check if camera is not already in use */
  if (!CameraIsInactive(overlay_cam)) {
    std::cout << "Camera is already used (active_cam or ImageRender cam, or custom Viewport cam)"
              << std::endl;
    return;
  }
  /* Check for already added collections */
  if (std::find(m_overlay_collections.begin(), m_overlay_collections.end(), collection) !=
      m_overlay_collections.end()) {
    std::cout << "Collection already added." << std::endl;
    return;
  }
  SetOverlayCamera(overlay_cam);
  m_overlay_collections.push_back(collection);

  /* This loops only on visibled objects */
  FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (collection, collection_object) {
    collection_object->gameflag |= OB_OVERLAY_COLLECTION;
  }
  FOREACH_COLLECTION_OBJECT_RECURSIVE_END;

  /* Handle the case of invisibled objects */
  for (KX_GameObject *gameobj : GetInactiveList()) {
    if (BKE_collection_has_object(collection, gameobj->GetBlenderObject())) {
      KX_GameObject *replica = AddReplicaObject(gameobj, nullptr, 0);
      replica->GetBlenderObject()->gameflag |= OB_OVERLAY_COLLECTION;
      bContext *C = KX_GetActiveEngine()->GetContext();
      Main *bmain = CTX_data_main(C);
      const Scene *scene = GetBlenderScene();
      BKE_collection_object_add(bmain, collection, replica->GetBlenderObject());
      /* If issue see: 68589a31ebfb79165f99a979357d237e5413e904 */
      BKE_view_layer_synced_ensure(scene, BKE_view_layer_default_view(scene));
      // release here because AddReplicaObject AddRef's
      // the object is added to the scene so we don't want python to own a reference
      replica->Release();
    }
  }
}

void KX_Scene::RemoveOverlayCollection(Collection *collection)
{
  /* Check for already removed collections */
  if (std::find(m_overlay_collections.begin(), m_overlay_collections.end(), collection) !=
      m_overlay_collections.end()) {
    /* If there is only one remaining overlay collection, we can Set the overlay camera to nullptr
     */
    if (m_overlay_collections.size() == 1) {
      SetOverlayCamera(nullptr);
    }
    m_overlay_collections.erase(
        std::find(m_overlay_collections.begin(), m_overlay_collections.end(), collection));

    /* Handle the case of replicas added */
    for (KX_GameObject *gameobj : GetObjectList()) {
      if (BKE_collection_has_object(collection, gameobj->GetBlenderObject())) {
        if (gameobj->IsReplica()) {
          bContext *C = KX_GetActiveEngine()->GetContext();
          Main *bmain = CTX_data_main(C);
          BKE_collection_object_remove(bmain, collection, gameobj->GetBlenderObject(), false);
          DelayedRemoveObject(gameobj);
        }
      }
    }

    FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (collection, collection_object) {
      collection_object->gameflag &= ~OB_OVERLAY_COLLECTION;
    }
    FOREACH_COLLECTION_OBJECT_RECURSIVE_END;
  }
}

void KX_Scene::OverlayPassDisableEffects(Depsgraph *depsgraph,
                                         KX_Camera *kxcam,
                                         bool isOverlayPass)
{
  if (!kxcam) {
    return;
  }
  Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);

  /* Restore original eevee post process flag in non overlay passes */
  if (!isOverlayPass) {
    scene_eval->eevee.flag = GetBlenderScene()->eevee.flag;
    scene_eval->eevee.gameflag |= SCE_EEVEE_WORLD_VOLUMES_ENABLED;
    return;
  }
  /* Don't use this if we don't use overlay collection feature */
  if (!GetOverlayCamera()) {
    return;
  }

  Object *obcam = kxcam->GetBlenderObject();
  Camera *cam = (Camera *)obcam->data;

  if (cam->gameflag & GAME_CAM_OVERLAY_DISABLE_AO) {
    scene_eval->eevee.flag &= ~SCE_EEVEE_GTAO_ENABLED;
  }
  if (cam->gameflag & GAME_CAM_OVERLAY_DISABLE_SSR) {
    scene_eval->eevee.flag &= ~SCE_EEVEE_SSR_ENABLED;
  }
  struct World *wo = scene_eval->world;
  if (wo) {
    if (cam->gameflag & GAME_CAM_OVERLAY_DISABLE_WORLD_VOLUMES) {
      scene_eval->eevee.gameflag &= ~SCE_EEVEE_WORLD_VOLUMES_ENABLED;
    }
  }

  if ((m_backupOverlayFlag != scene_eval->eevee.flag) ||
      (m_backupOverlayGameFlag != scene_eval->eevee.gameflag)) {
    /* Only tag if overlay settings changed since previous frame */
    AppendToIdsToUpdateInOverlayPass(&obcam->id, ID_RECALC_TRANSFORM);
  }
  m_backupOverlayFlag = scene_eval->eevee.flag;
  m_backupOverlayGameFlag = scene_eval->eevee.gameflag;
}

void KX_Scene::SetCurrentGPUViewport(GPUViewport *viewport)
{
  m_currentGPUViewport = viewport;

  /* We set a GPUViewport as soon as possible
   * to be able to call DRW_notify_view_update.
   * The GPUViewport set doesn't really matter
   * (as far I understood) but we need to have
   * one set when we use custom bge render loop. */
  DRW_game_gpu_viewport_set(viewport);
}

GPUViewport *KX_Scene::GetCurrentGPUViewport()
{
  return m_currentGPUViewport;
}

void KX_Scene::SetInitMaterialsGPUViewport(GPUViewport *viewport)
{
  if (!viewport) {
    GPU_viewport_free(m_initMaterialsGPUViewport);
  }
  m_initMaterialsGPUViewport = viewport;
}

GPUViewport *KX_Scene::GetInitMaterialsGPUViewport()
{
  return m_initMaterialsGPUViewport;
}

void KX_Scene::SetOverlayCamera(KX_Camera *cam)
{
  m_overlayCamera = cam;
}

KX_Camera *KX_Scene::GetOverlayCamera()
{
  return m_overlayCamera;
}

void KX_Scene::AddImageRenderCamera(KX_Camera *cam)
{
  m_imageRenderCameraList.push_back(cam);
}

void KX_Scene::RemoveImageRenderCamera(KX_Camera *cam)
{
  m_imageRenderCameraList.erase(
      std::find(m_imageRenderCameraList.begin(), m_imageRenderCameraList.end(), cam));
}

bool KX_Scene::CameraIsInactive(KX_Camera *cam)
{
  if (cam->GetViewport()) {
    return false;
  }
  if (cam == GetActiveCamera()) {
    return false;
  }
  if (std::find(m_imageRenderCameraList.begin(), m_imageRenderCameraList.end(), cam) !=
      m_imageRenderCameraList.end()) {
    return false;
  }
  return true;
}

static RAS_Rasterizer::FrameBufferType r = RAS_Rasterizer::RAS_FRAMEBUFFER_FILTER0;
static RAS_Rasterizer::FrameBufferType s = RAS_Rasterizer::RAS_FRAMEBUFFER_EYE_LEFT0;

void KX_Scene::RenderAfterCameraSetup(KX_Camera *cam,
                                      RAS_FrameBuffer *background_fb,
                                      const RAS_Rect &viewport,
                                      bool is_overlay_pass,
                                      bool is_last_render_pass)
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  RAS_Rasterizer *rasty = engine->GetRasterizer();
  RAS_ICanvas *canvas = engine->GetCanvas();
  bContext *C = engine->GetContext();
  Main *bmain = CTX_data_main(C);
  Scene *scene = GetBlenderScene();
  /* This ensures a depsgraph is allocated and activates it */
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  bool useViewportRender = KX_GetActiveEngine()->UseViewportRender();

  if (!useViewportRender) {  // Custom bge render loop only
    bool calledFromConstructor = cam == nullptr;
    if (calledFromConstructor) {
      m_currentGPUViewport = GPU_viewport_create();
      DRW_game_gpu_viewport_set(m_currentGPUViewport);
      SetInitMaterialsGPUViewport(m_currentGPUViewport);
    }
    else {
      SetCurrentGPUViewport(cam->GetGPUViewport());
    }
  }

  engine->CountDepsgraphTime();

  if (m_collectionRemap) {
    /* check 68589a31ebfb79165f99a979357d237e5413e904 for potential issue or improvement? */
    /* If problem with ReplicateBlenderObject, see other occurences of BKE_collection_object_add_from*/
    BKE_main_collection_sync_remap(bmain);
    m_collectionRemap = false;
  }

  /* Notify the depsgraph if object transform changed in the scene
   * for next drawing loop. */
  for (KX_GameObject *gameobj : GetObjectList()) {
    /* Update compatibles blender physics simulations */
    Object *ob = gameobj->GetBlenderObject();
    TagBlenderPhysicsObject(scene, ob);
    gameobj->TagForTransformUpdate(is_overlay_pass, is_last_render_pass);
  }

  /* Notify depsgraph for other changes */
  TagForExtraIdsUpdate(bmain, cam);

  if (is_last_render_pass) {
    m_idsToUpdateInAllRenderPasses.clear();
  }

  /* We need the changes to be flushed before each draw loop! */
  BKE_scene_graph_update_tagged(depsgraph, bmain);

  /* Update evaluated object object_to_world according to SceneGraph. */
  for (KX_GameObject *gameobj : GetObjectList()) {
    gameobj->TagForTransformUpdateEvaluated();
  }

  engine->EndCountDepsgraphTime();

  rcti window;
  int v[4];
  /* Custom BGE viewports*/
  if (cam && cam->GetViewport() && cam != GetOverlayCamera()) {
    v[0] = viewport.GetLeft();
    v[1] = viewport.GetBottom();
    v[2] = viewport.GetWidth() + 1;
    v[3] = viewport.GetHeight() + 1;
    window = {0, viewport.GetWidth(), 0, viewport.GetHeight()};
  }
  /* Main cam (when it has no custom viewport), overlay cam */
  else {
    v[0] = 0;
    v[1] = 0;
    v[2] = canvas->GetWidth() + 1;
    v[3] = canvas->GetHeight() + 1;
    window = {0, canvas->GetWidth(), 0, canvas->GetHeight()};
  }

  /* When we call wm_draw_update, bContext variables are unset,
   * then we need to set it again correctly to render the next frame.
   * wm_draw_update can also be called when playing dragging or resizing
   * blender window */
  ReinitBlenderContextVariables();

  /* Here we'll render directly the scene with viewport code. */
  if (useViewportRender) {
    /* Viewport render mode doesn't support several render passes then exit here
     * if we are trying to use not supported features. */
    if (cam && cam != KX_GetActiveEngine()->GetRenderingCameras().front()) {
      std::cout << "Warning: Viewport Render mode doesn't support multiple render passes"
                << std::endl;
      return;
    }

    /* Don't need any background framebuffer as everything will be redrawn */
    GPU_framebuffer_restore();

    if (cam) {
      if (canvas->IsBlenderPlayer()) {
        ARegion *region = CTX_wm_region(C);
        scene->flag |= SCE_IS_BLENDERPLAYER;
        region->winrct = window;
        region->winx = canvas->GetWidth();
        region->winy = canvas->GetHeight();
        /* Force camera projection matrix to be the same as viewport one (for mouse events) */
        cam->SetProjectionMatrix(MT_Matrix4x4(&CTX_wm_region_view3d(C)->winmat[0][0]));
      }

      CTX_wm_view3d(C)->camera = cam->GetBlenderObject();

#ifdef WITH_XR_OPENXR
      wmWindowManager *wm = CTX_wm_manager(C);
      if (WM_xr_session_exists(&wm->xr)) {
        if (WM_xr_session_is_ready(&wm->xr)) {
          wm_xr_events_handle(CTX_wm_manager(C));
          //wm_event_do_handlers(C);   // TODO: Find more specific XR code
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

      if (canvas->IsBlenderPlayer()) {
        scene->flag &= ~SCE_IS_BLENDERPLAYER;
      }

      return;
    }
  }

  /* Custom bge render loop only from here */
  if (cam) {
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

    UpdateObjectLods(cam);
  }

  /* Disable some post processing effects for overlay collections render pass */
  OverlayPassDisableEffects(depsgraph, cam, is_overlay_pass);

  short samples_per_frame = min_ii(scene->gm.samples_per_frame, scene->eevee.taa_samples);
  samples_per_frame = max_ii(samples_per_frame, 1);

  for (short i = 0; i < samples_per_frame; i++) {
    if (background_fb) {
      GPU_framebuffer_bind(background_fb->GetFrameBuffer());
      GPU_framebuffer_clear_depth(background_fb->GetFrameBuffer(), 1.0f);
      GPU_framebuffer_restore();
    }
    /* Draw custom viewport render loop into its own GPUViewport */
    DRW_game_render_loop(
        C, m_currentGPUViewport, depsgraph, &window, is_overlay_pass, cam == nullptr);
  }

  RAS_FrameBuffer *input = rasty->GetFrameBuffer(rasty->NextFilterFrameBuffer(r));
  RAS_FrameBuffer *output = rasty->GetFrameBuffer(rasty->NextRenderFrameBuffer(s));

  GPUTexture *color = GPU_viewport_color_texture(m_currentGPUViewport, 0);
  GPUAttachment config[] = {
      GPU_ATTACHMENT_TEXTURE(GPU_viewport_depth_texture(m_currentGPUViewport)),
      GPU_ATTACHMENT_TEXTURE(color)};

  GPU_framebuffer_config_array(
      input->GetFrameBuffer(), config, sizeof(config) / sizeof(GPUAttachment));

  output->UpdateSize(GPU_texture_width(color), GPU_texture_height(color));

  /* Clear output framebuffer to ensure it has no color from previous pass.
   * (was causing troubles in Vulkan + custom bge viewports) */
  GPU_framebuffer_bind(output->GetFrameBuffer());
  const float clear_col[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  GPU_framebuffer_clear_color_depth(output->GetFrameBuffer(), clear_col, 1.0f);
  GPU_framebuffer_restore();

  /* Draw 2D filters */
  RAS_FrameBuffer *f = is_overlay_pass || !background_fb ? input : Render2DFilters(rasty, canvas, input, output);

  GPU_framebuffer_restore();

  if (background_fb) {
    /* Draw this camera render into background framebuffer */
    GPU_framebuffer_bind(background_fb->GetFrameBuffer());
    GPU_viewport(v[0], v[1], v[2], v[3]);
    GPU_scissor_test(true);
    GPU_scissor(v[0], v[1], v[2], v[3]);
    rasty->DrawFrameBuffer(f, background_fb);
  }

  GPU_framebuffer_restore();

  GPU_blend(GPU_BLEND_NONE);
}

void KX_Scene::RenderAfterCameraSetupImageRender(KX_Camera *cam, const rcti *window)
{
  bContext *C = KX_GetActiveEngine()->GetContext();
  Depsgraph *depsgraph = CTX_data_depsgraph_on_load(C);

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

  DRW_game_render_loop(C, m_currentGPUViewport, depsgraph, window, false, false);
}

void KX_Scene::SetBlenderSceneConverter(BL_SceneConverter *sc_converter)
{
  m_sceneConverter = sc_converter;
}

BL_SceneConverter *KX_Scene::GetBlenderSceneConverter()
{
  return m_sceneConverter;
}

void KX_Scene::ConvertBlenderObject(Object *ob)
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  e_PhysicsEngine physics_engine = UseBullet;
  RAS_Rasterizer *rasty = engine->GetRasterizer();
  RAS_ICanvas *canvas = engine->GetCanvas();
  bContext *C = engine->GetContext();
  Depsgraph *depsgraph = CTX_data_expect_evaluated_depsgraph(C);
  Main *bmain = CTX_data_main(C);
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

void KX_Scene::convert_blender_objects_list_synchronous(std::vector<Object *> objectslist)
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  e_PhysicsEngine physics_engine = UseBullet;
  RAS_Rasterizer *rasty = engine->GetRasterizer();
  RAS_ICanvas *canvas = engine->GetCanvas();
  bContext *C = engine->GetContext();
  Depsgraph *depsgraph = CTX_data_expect_evaluated_depsgraph(C);
  Main *bmain = CTX_data_main(C);

  for (Object *obj : objectslist) {
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
  std::vector<Object *> objectslist;
  KX_KetsjiEngine *engine;
  e_PhysicsEngine physics_engine;
  KX_Scene *kxscene;
  BL_SceneConverter *converter;
  RAS_Rasterizer *rasty;
  RAS_ICanvas *canvas;
  Depsgraph *depsgraph;
  Main *bmain;
};

static void convert_blender_objects_list_thread_func(TaskPool *__restrict pool,
                                                     void *taskdata)
{
  ConvertBlenderObjectsListTaskData *task = static_cast<ConvertBlenderObjectsListTaskData *>(
      taskdata);

  for (Object *obj : task->objectslist) {
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

void KX_Scene::ConvertBlenderObjectsList(std::vector<Object *> objectslist, bool asynchronous)
{
  if (asynchronous) {
    /* Convert the Blender Objects list in a different thread, so that the
     * game engine can keep running at full speed. */
    ConvertBlenderObjectsListTaskData task;
    task.engine = KX_GetActiveEngine();
    task.physics_engine = UseBullet;
    task.kxscene = this;
    task.converter = m_sceneConverter;
    task.rasty = task.engine->GetRasterizer();
    task.canvas = task.engine->GetCanvas();
    bContext *C = task.engine->GetContext();
    task.depsgraph = CTX_data_expect_evaluated_depsgraph(C);
    task.bmain = CTX_data_main(C);
    task.objectslist = objectslist;

    TaskPool *taskpool = BLI_task_pool_create(&task, TASK_PRIORITY_LOW);

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
      Object *temp = task.objectslist.back();
      task.objectslist.pop_back();
      delete temp;
    }

    BLI_task_pool_free(taskpool);
    taskpool = nullptr;
  }
  else {
    convert_blender_objects_list_synchronous(objectslist);
  }
}

void KX_Scene::convert_blender_collection_synchronous(Collection *co)
{
  KX_KetsjiEngine *engine = KX_GetActiveEngine();
  e_PhysicsEngine physics_engine = UseBullet;
  RAS_Rasterizer *rasty = engine->GetRasterizer();
  RAS_ICanvas *canvas = engine->GetCanvas();
  bContext *C = engine->GetContext();
  Depsgraph *depsgraph = CTX_data_expect_evaluated_depsgraph(C);
  Main *bmain = CTX_data_main(C);

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
  Collection *co;
  KX_KetsjiEngine *engine;
  e_PhysicsEngine physics_engine;
  KX_Scene *kxscene;
  BL_SceneConverter *converter;
  RAS_Rasterizer *rasty;
  RAS_ICanvas *canvas;
  Depsgraph *depsgraph;
  Main *bmain;
};

static void convert_blender_collection_thread_func(TaskPool *__restrict pool,
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

void KX_Scene::ConvertBlenderCollection(Collection *co, bool asynchronous)
{
  if (asynchronous) {

    /* Convert the Blender collection in a different thread, so that the
     * game engine can keep running at full speed. */
    ConvertBlenderCollectionTaskData task;

    task.engine = KX_GetActiveEngine();
    task.physics_engine = UseBullet;
    task.co = co;
    task.kxscene = this;
    task.converter = m_sceneConverter;
    task.rasty = task.engine->GetRasterizer();
    task.canvas = task.engine->GetCanvas();
    bContext *C = task.engine->GetContext();
    task.depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
    task.bmain = CTX_data_main(C);

    TaskPool *taskpool = BLI_task_pool_create(&task, TASK_PRIORITY_LOW);

    BLI_task_pool_push(taskpool, convert_blender_collection_thread_func, &task, false, NULL);
    BLI_task_pool_work_and_wait(taskpool);
    BLI_task_pool_free(taskpool);
    taskpool = nullptr;
  }
  else {
    convert_blender_collection_synchronous(co);
  }
}

void KX_Scene::ConvertBlenderAction(bAction *action)
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

void KX_Scene::BackupRestrictFlag(Object *ob, char restrictFlag)
{
  m_obRestrictFlags.insert({ob, restrictFlag});
}

void KX_Scene::RestoreRestrictFlags()
{
  for (std::map<Object *, char>::iterator it = m_obRestrictFlags.begin();
       it != m_obRestrictFlags.end();
       it++) {
    Object *ob = it->first;
    ob->visibility_flag = it->second;
  }
}

void KX_Scene::TagForCollectionRemap()
{
  m_collectionRemap = true;
}

KX_GameObject *KX_Scene::GetGameObjectFromObject(Object *ob)
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

bool KX_Scene::OrigObCanBeTransformedInRealtime(Object *ob)
{
  FluidModifierData *fluidModifierData = (FluidModifierData *)BKE_modifiers_findby_type(
      ob, eModifierType_Fluid);
  if (fluidModifierData) {
    return false;
  }
  return true;
}

/* Look at object_transform for original function */
void KX_Scene::IgnoreParentTxBGE(Main *bmain,
                                 Depsgraph *depsgraph,
                                 Scene *scene,
                                 Object *ob,
                                 std::vector<Object *> children)
{
  Object *ob_child;

  Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);

  /* a change was made, adjust the children to compensate */
  for (Object *child : children) {
    if (child->parent == ob) {
      ob_child = child;
      Object *ob_child_eval = DEG_get_evaluated_object(depsgraph, ob_child);
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

    Object *ob_orig = backup->ob;

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
    MEM_freeN(backup->obtfm);
    delete backup;
  }
}

bool KX_Scene::SomethingIsMoving()
{
  for (KX_GameObject *gameobj : GetObjectList()) {
    if (!(compare_m4m4((float(*)[4])(gameobj->GetPrevObjectMatToWorld()),
                       gameobj->GetBlenderObject()->object_to_world().ptr(),
                       FLT_MIN))) {
      return true;
    }
  }
  return false;
}

void KX_Scene::AppendToIdsToUpdateInAllRenderPasses(ID *id, IDRecalcFlag flag)
{
  std::pair<ID *, IDRecalcFlag> it = {id, flag};
  if (std::find(m_idsToUpdateInAllRenderPasses.begin(),
                m_idsToUpdateInAllRenderPasses.end(),
                it) == m_idsToUpdateInAllRenderPasses.end()) {
    m_idsToUpdateInAllRenderPasses.push_back(it);
  }
}

void KX_Scene::AppendToIdsToUpdateInOverlayPass(ID *id, IDRecalcFlag flag)
{
  std::pair<ID *, IDRecalcFlag> it = {id, flag};
  if (std::find(m_idsToUpdateInOverlayPass.begin(),
                m_idsToUpdateInOverlayPass.end(),
                it) == m_idsToUpdateInOverlayPass.end()) {
    m_idsToUpdateInOverlayPass.push_back(it);
  }
}

void KX_Scene::TagForExtraIdsUpdate(Main *bmain, KX_Camera *cam)
{
  for (std::vector<std::pair<ID *, IDRecalcFlag>>::iterator it =
           m_idsToUpdateInAllRenderPasses.begin();
       it != m_idsToUpdateInAllRenderPasses.end();
       it++) {
    DEG_id_tag_update(it->first, it->second);
  }

  if (cam && cam == GetOverlayCamera()) {
    for (std::vector<std::pair<ID *, IDRecalcFlag>>::iterator it =
             m_idsToUpdateInOverlayPass.begin();
         it != m_idsToUpdateInOverlayPass.end();
         it++) {
      DEG_id_tag_update(it->first, it->second);
    }
    m_idsToUpdateInOverlayPass.clear();
  }
}

void KX_Scene::TagBlenderPhysicsObject(Scene *scene, Object *ob)
{
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
    if (ob->rigidbody_object &&
        ob->rigidbody_object->type == RBO_TYPE_ACTIVE)
    {
      DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM);
      ob->transflag |= OB_TRANSFLAG_OVERRIDE_GAME_PRIORITY;
    }
  }
}

KX_GameObject *KX_Scene::AddDuplicaObject(KX_GameObject *gameobj,
                                          KX_GameObject *reference,
                                          float lifespan)
{
  Object *ob = gameobj->GetBlenderObject();
  if (ob) {
    if (ob->instance_collection) {
      CM_Warning("Warning: Full duplication of an instance collection is not supported: " << ob->id.name + 2);
      return nullptr;
    }
    bContext *C = KX_GetActiveEngine()->GetContext();
    Main *bmain = CTX_data_main(C);
    Scene *scene = GetBlenderScene();
    ViewLayer *view_layer = BKE_view_layer_default_view(scene);
    Base *base = BKE_view_layer_base_find(view_layer, ob);
    if (base) {
      Base *basen = blender::ed::object::add_duplicate(
          bmain, scene, view_layer, base, USER_DUP_OBDATA);
      BKE_collection_object_add_from(bmain,
                                     scene,
                                     BKE_view_layer_camera_find(scene, view_layer),
                                     basen->object);  // add replica where is the active camera

      basen->flag |= (BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT |
                      BASE_ENABLED_AND_VISIBLE_IN_DEFAULT_VIEWPORT);
      basen->object->visibility_flag &= ~OB_HIDE_VIEWPORT;
      BKE_main_collection_sync_remap(bmain);

      DEG_relations_tag_update(bmain);
      // BKE_scene_graph_update_tagged(depsgraph, bmain);
      ConvertBlenderObject(basen->object);

      KX_GameObject *replica = m_sceneConverter->FindGameObject(basen->object);

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
  Object *blenderobject = gameobj->GetBlenderObject();
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
  if (NewRemoveObject(gameobj)) {
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

    // Child objects must be static
    if (parent)
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

        for (sensorpos = 0, sit = sensorlist.begin(); sit != sensorlist.end();
             sit++, sensorpos++) {
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
             ait++, actuatorpos++) {
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
  std::vector<KX_GameObject *>children = parent->GetChildren();
  for (KX_GameObject *child : children) {
    child->GetBlenderObject()->parent = parent->GetBlenderObject();
    if (parent->GetBlenderObject()->type == OB_ARMATURE) {
      ModifierData *mod;
      for (mod = (ModifierData *)child->GetBlenderObject()->modifiers.first; mod; mod = mod->next)
      {
        if (mod->type == eModifierType_Armature) {
          ((ArmatureModifierData *)mod)->object = child->GetBlenderObject()->parent;
        }
      }
    }
    remap_parents_recursive(child);
  }
}

void KX_Scene::DupliGroupRecurse(KX_GameObject *groupobj, int level)
{
  Object *blgroupobj = groupobj->GetBlenderObject();
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

  Collection *group = blgroupobj->instance_collection;
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
        // Object is not visible in the 3D view, will not be instantiated. ??
        /* 3 remarks:
         * - The comment souldn't be: "if blenderobj not in same layer than groupobj, don't convert
         * it as gameobj"?
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

  // now look if object in the hierarchy have dupli group and recurse
  for (KX_GameObject *gameobj : m_logicHierarchicalGameObjects) {
    /* Replicate all constraints. */
    if (gameobj->GetPhysicsController()) {
      gameobj->GetPhysicsController()->ReplicateConstraints(gameobj,
                                                            m_logicHierarchicalGameObjects);
      gameobj->ClearConstraints();
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
    }
  }

  for (KX_GameObject *gameobj : duplilist) {
    DupliGroupRecurse(gameobj, level + 1);
  }
}

KX_GameObject *KX_Scene::AddReplicaObject(KX_GameObject *originalobject,
                                          KX_GameObject *referenceobject,
                                          float lifespan)
{
  m_logicHierarchicalGameObjects.clear();
  m_map_gameobject_to_replica.clear();
  m_groupGameObjects.clear();

  KX_GameObject *originalobj = (KX_GameObject *)originalobject;
  KX_GameObject *referenceobj = (KX_GameObject *)referenceobject;

  m_ueberExecutionPriority++;

  // lets create a replica
  KX_GameObject *replica = (KX_GameObject *)AddNodeReplicaObject(nullptr, originalobj);

  // add a timebomb to this object
  // lifespan of zero means 'this object lives forever'
  if (lifespan > 0.0f) {
    // for now, convert between so called frames and realtime
    m_tempObjectList.push_back(replica);
    // this convert the life from frames to sort-of seconds, hard coded 0.016666667 that assumes we have
    // 60 frames per second if you change this value, make sure you change it in
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

void KX_Scene::RemoveObject(KX_GameObject *gameobj)
{
  // disconnect child from parent
  SG_Node *node = gameobj->GetSGNode();

  if (node) {
    node->DisconnectFromParent();

    // recursively destruct
    node->Destruct();
  }
}

void KX_Scene::RemoveDupliGroup(KX_GameObject *gameobj)
{
  if (gameobj->IsDupliGroup()) {
    for (KX_GameObject *instance : gameobj->GetInstanceObjects()) {
      DelayedRemoveObject(instance);
    }
  }
}

void KX_Scene::DelayedRemoveObject(KX_GameObject *gameobj)
{
  RemoveDupliGroup(gameobj);

  CM_ListAddIfNotFound(m_euthanasyobjects, gameobj);

  /* Unregister asap (don't wait next frame) to avoid issue
   * when objects are added/removed the same frame
   * https://github.com/UPBGE/upbge/issues/1600
   */
  GetBlenderSceneConverter()->UnregisterGameObject(gameobj);
}

bool KX_Scene::NewRemoveObject(KX_GameObject *gameobj)
{
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
  // instances refering to this group
  if (gameobj->GetInstanceObjects()) {
    for (KX_GameObject *instance : gameobj->GetInstanceObjects()) {
      instance->RemoveDupliGroupObject();
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

  gameobj->RemoveMeshes();

  bool ret = true;
  if (m_lightlist->RemoveValue(gameobj)) {
    ret = (gameobj->Release() != nullptr);
  }
  if (m_objectlist->RemoveValue(gameobj)) {
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
  CM_ListRemoveIfFound(m_tempObjectList, gameobj);

  if (gameobj == m_active_camera) {
    // no AddRef done on m_active_camera so no Release
    // m_active_camera->Release();
    m_active_camera = nullptr;
  }

  if (gameobj == m_overrideCullingCamera) {
    m_overrideCullingCamera = nullptr;
  }

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
      gameobj->GetPhysicsController()->ReinstancePhysicsShape(nullptr, mesh);
  }

  if (use_gfx || use_phys) {
    DEG_id_tag_update(&gameobj->GetBlenderObject()->id, ID_RECALC_GEOMETRY);
  }
}

KX_Camera *KX_Scene::GetActiveCamera()
{
  // nullptr if not defined
  return m_active_camera;
}

void KX_Scene::SetActiveCamera(KX_Camera *cam)
{
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
        DelayedRemoveObject(gameobj);
      }
    }
    else {
      // all object is the tempObjectList should have a clock
      BLI_assert(false);
    }
  }
  m_logicmgr->BeginFrame(curtime, framestep);
}

void KX_Scene::AddAnimatedObject(KX_GameObject *gameobj)
{
  CM_ListAddIfNotFound(m_animatedlist, gameobj);
}

// static void update_anim_thread_func(TaskPool *pool, void *taskdata, int UNUSED(threadid))
//{
//  KX_GameObject *gameobj, *parent;
//  bool needs_update;
//  KX_Scene::AnimationPoolData *data = (KX_Scene::AnimationPoolData
//  *)BLI_task_pool_user_data(pool); double curtime = data->curtime;

//  gameobj = (KX_GameObject *)taskdata;

//  // Non-armature updates are fast enough, so just update them
//  needs_update = gameobj->GetGameObjectType() != SCA_IObject::OBJ_ARMATURE;

//  if (!needs_update) {
//    // If we got here, we're looking to update an armature, so check its children meshes
//    // to see if we need to bother with a more expensive pose update
//    const std::vector<KX_GameObject *> children = gameobj->GetChildren();

//    bool has_mesh = false, has_non_mesh = false;

//    // Check for meshes that haven't been culled
//    for (KX_GameObject *child : children) {
//      // if (!child->GetCulled()) { // eevee disable armature animation culling
//      needs_update = true;
//      break;
//      //}

//      if (child->GetMeshCount() == 0)
//        has_non_mesh = true;
//      else
//        has_mesh = true;
//    }

//    // If we didn't find a non-culled mesh, check to see
//    // if we even have any meshes, and update if this
//    // armature has only non-mesh children.
//    if (!needs_update && !has_mesh && has_non_mesh)
//      needs_update = true;
//  }

//  // If the object is a culled armature, then we manage only the animation time and end of its
//  // animations.
//  gameobj->UpdateActionManager(curtime, needs_update);

//  if (needs_update) {
//    const std::vector<KX_GameObject *> children = gameobj->GetChildren();
//    parent = gameobj->GetParent();
//  }
//}

void KX_Scene::UpdateAnimations(double curtime)
{
  // m_animationPoolData.curtime = curtime;

  for (KX_GameObject *gameobj : m_animatedlist) {
    // BLI_task_pool_push(m_animationPool, update_anim_thread_func, gameobj, false,
    // TASK_PRIORITY_LOW);
    if (!gameobj->IsActionsSuspended()) {
      gameobj->UpdateActionManager(curtime, true);
    }
  }

  // BLI_task_pool_work_and_wait(m_animationPool);
}

void KX_Scene::LogicUpdateFrame(double curtime)
{
  m_proxyManager.Update();

  m_logicmgr->UpdateFrame(curtime);
}

void KX_Scene::LogicEndFrame()
{
  m_logicmgr->EndFrame();

  /* Don't remove the objects from the euthanasy list here as the child objects of a deleted
   * parent object are destructed directly from the sgnode in the same time the parent
   * object is destructed. These child objects must be removed automatically from the
   * euthanasy list to avoid double deletion in case the user ask to delete the child object
   * explicitly. NewRemoveObject is the place to do it.
   */
  while (!m_euthanasyobjects.empty()) {
    RemoveObject(m_euthanasyobjects.front());
  }

  // prepare obstacle simulation for new frame
  if (m_obstacleSimulation)
    m_obstacleSimulation->UpdateObstacles();

  for (KX_FontObject *font : m_fontlist) {
    font->UpdateTextFromProperty();
  }
}

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
  const MT_Vector3 &cam_pos = cam->NodeGetWorldPosition();
  const float lodfactor = cam->GetLodDistanceFactor();

  for (KX_GameObject *gameobj : m_kxobWithLod) {
    gameobj->UpdateLod(cam_pos, lodfactor);
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
        KX_GameObject::ActivityCullingInfo::ACTIVITY_NONE) {
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
  return m_networkScene;
}

void KX_Scene::SetNetworkMessageScene(KX_NetworkMessageScene *newScene)
{
  m_networkScene = newScene;
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
  brick->Replace_NetworkScene(to->GetNetworkMessageScene());
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
  }

  GetObjectList()->MergeList(other->GetObjectList());
  other->GetObjectList()->ReleaseAndRemoveAll();

  GetInactiveList()->MergeList(other->GetInactiveList());
  other->GetInactiveList()->ReleaseAndRemoveAll();

  GetRootParentList()->MergeList(other->GetRootParentList());
  other->GetRootParentList()->ReleaseAndRemoveAll();

  GetLightList()->MergeList(other->GetLightList());
  other->GetLightList()->ReleaseAndRemoveAll();

  GetCameraList()->MergeList(other->GetCameraList());
  other->GetCameraList()->ReleaseAndRemoveAll();

  GetFontList()->MergeList(other->GetFontList());
  other->GetFontList()->ReleaseAndRemoveAll();

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

KX_Scene *KX_Scene::NewInstance()
{
  return new KX_Scene(*this);
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
                    "addObject(object, other, time=0, dupli=0)\n"
                    "Returns the added object.\n")
{
  PyObject *pyob, *pyreference = Py_None;
  KX_GameObject *ob, *reference;

  float time = 0.0f;

  // Full duplication of ob->data
  int duplicate = 0;

  if (!PyArg_ParseTuple(args, "O|Ofi:addObject", &pyob, &pyreference, &time, &duplicate))
    return nullptr;

  if (!ConvertPythonToGameObject(
          m_logicmgr,
          pyob,
          &ob,
          false,
          "scene.addObject(object, reference, time, dupli): KX_Scene (first argument)") ||
      !ConvertPythonToGameObject(
          m_logicmgr,
          pyreference,
          &reference,
          true,
          "scene.addObject(object, reference, time, dupli): KX_Scene (second argument)"))
    return nullptr;

  if (!m_inactivelist->SearchValue(ob)) {
    PyErr_Format(
        PyExc_ValueError,
        "scene.addObject(object, reference, time, dupli): KX_Scene (first argument): object "
        "must be in an inactive layer");
    return nullptr;
  }
  bool dupli = duplicate == 1;
  KX_GameObject *replica = !dupli ? AddReplicaObject(ob, reference, time) :
                                    AddDuplicaObject(ob, reference, time);

  /* Can happen when trying to Duplicate an instance_collection */
  if (replica == nullptr) {
    Py_RETURN_NONE;
  }

  // release here because AddReplicaObject AddRef's
  // the object is added to the scene so we don't want python to own a reference
  if (!dupli) {
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
    std::cout << "Expected a bpy.types.Object." << std::endl;
    return nullptr;
  }

  ID *id;
  if (!pyrna_id_FromPyObject(bl_object, &id)) {
    std::cout << "Failed to convert object." << std::endl;
    return nullptr;
  }
  Object *ob = (Object *)id;
  ConvertBlenderObject(ob);
  KX_GameObject *newgameobj = m_sceneConverter->FindGameObject(ob);
  if (!newgameobj) {
    /* It can happen for example if we are trying to convert the same object several times
     * or if we are trying to convert game_default_cam https://github.com/UPBGE/upbge/issues/1847 */
    CM_Warning("Scene converter failed to convert: " << ob->id.name + 2);
    Py_RETURN_NONE;
  }
  return newgameobj->GetProxy();
}

EXP_PYMETHODDEF_DOC(KX_Scene,
                    convertBlenderObjectsList,
                    "convertBlenderObjectsList()\n"
                    "\n")
{
  PyObject *list;
  int asynchronous = 0;

  if (!PyArg_ParseTuple(args, "O!i:", &PyList_Type, &list, &asynchronous)) {
    std::cout << "Expected a bpy.types.Object list." << std::endl;
    return nullptr;
  }

  std::vector<Object *> objectslist;
  Py_ssize_t list_size = PyList_Size(list);

  for (Py_ssize_t i = 0; i < list_size; i++) {
    PyObject *bl_object = PyList_GetItem(list, i);

    ID *id;
    if (!pyrna_id_FromPyObject(bl_object, &id)) {
      std::cout << "Failed to convert object." << std::endl;
      return nullptr;
    }

    Object *ob = (Object *)id;
    objectslist.push_back(ob);
  }

  ConvertBlenderObjectsList(objectslist, asynchronous);
  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_Scene,
                    convertBlenderCollection,
                    "convertBlenderCollection()\n"
                    "\n")
{
  PyObject *bl_collection = Py_None;
  int asynchronous;

  if (!PyArg_ParseTuple(args, "Oi:", &bl_collection, &asynchronous)) {
    std::cout << "Expected a bpy.types.Collection." << std::endl;
    return nullptr;
  }

  ID *id;
  if (!pyrna_id_FromPyObject(bl_collection, &id)) {
    std::cout << "Failed to convert collection." << std::endl;
    return nullptr;
  }

  Collection *co = (Collection *)id;
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
    std::cout << "Expected a bpy.types.Action." << std::endl;
    return nullptr;
  }

  ID *id;
  if (!pyrna_id_FromPyObject(bl_action, &id)) {
    std::cout << "Failed to convert action." << std::endl;
    return nullptr;
  }

  bAction *act = (bAction *)id;
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
    std::cout << "Expected a bpy.types.Action." << std::endl;
    return nullptr;
  }

  ID *id;
  if (!pyrna_id_FromPyObject(bl_action, &id)) {
    std::cout << "Failed to find action to unregister." << std::endl;
    return nullptr;
  }

  bAction *act = (bAction *)id;
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
                    "addOverlayCollection(KX_Camera *cam, Collection *col)\n"
                    "\n")
{
  PyObject *pyCamera = Py_None;
  PyObject *pyCollection = Py_None;

  if (!PyArg_ParseTuple(args, "OO:", &pyCamera, &pyCollection)) {
    std::cout << "Expected a KX_Camera and a bpy.types.Collection." << std::endl;
    return nullptr;
  }

  KX_Camera *kxCam = nullptr;
  if (!(ConvertPythonToCamera(this, pyCamera, &kxCam, false, nullptr))) {
    std::cout << "Failed to convert KX_Camera" << std::endl;
    return nullptr;
  }

  ID *id = nullptr;
  if (!pyrna_id_FromPyObject(pyCollection, &id)) {
    std::cout << "Failed to convert collection." << std::endl;
    return nullptr;
  }

  Collection *co = (Collection *)id;
  AddOverlayCollection(kxCam, co);
  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_Scene,
                    removeOverlayCollection,
                    "removeOverlayCollection(Collection *col)\n"
                    "\n")
{
  PyObject *pyCollection = Py_None;

  if (!PyArg_ParseTuple(args, "O:", &pyCollection)) {
    std::cout << "Expected a bpy.types.Collection." << std::endl;
    return nullptr;
  }

  ID *id = nullptr;
  if (!pyrna_id_FromPyObject(pyCollection, &id)) {
    std::cout << "Failed to convert collection." << std::endl;
    return nullptr;
  }

  Collection *co = (Collection *)id;
  RemoveOverlayCollection(co);
  Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_Scene,
                    getGameObjectFromObject,
                    "getGameObjectFromObject(Object *ob)\n"
                    "\n")
{
  PyObject *pyBlenderObject = Py_None;

  if (!PyArg_ParseTuple(args, "O:", &pyBlenderObject)) {
    std::cout << "getGameObjectFromObject: Expected a bpy.types.Object." << std::endl;
    return nullptr;
  }

  ID *id = nullptr;
  if (!pyrna_id_FromPyObject(pyBlenderObject, &id)) {
    std::cout << "getGameObjectFromObject: Failed to convert Object " << id->name + 2
              << std::endl;
    return nullptr;
  }

  Object *ob = (Object *)id;
  if (ob) {
    KX_GameObject *gameobj = GetGameObjectFromObject(ob);
    if (gameobj) {
      return gameobj->GetProxy();
    }
    std::cout << "getGameObjectFromObject: No KX_GameObject found for this Object " << ob->id.name + 2 << std::endl;
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
