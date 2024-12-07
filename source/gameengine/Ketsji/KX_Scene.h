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

/** \file KX_Scene.h
 *  \ingroup ketsji
 */

#pragma once

#include <list>
#include <set>
#include <vector>

#include "DNA_ID.h"  // For IDRecalcFlag

#include "EXP_PyObjectPlus.h"
#include "EXP_Value.h"
#include "KX_PhysicsEngineEnums.h"
#include "KX_PythonProxy.h"
#include "KX_PythonProxyManager.h"
#include "MT_Transform.h"
#include "RAS_FramingManager.h"
#include "RAS_Rect.h"
#include "SCA_IScene.h"
#include "SG_Frustum.h"
#include "SG_Node.h"

/**
 * \section Forward declarations
 */
struct Scene;

template<class T> class EXP_ListValue;

class EXP_Value;
class SCA_LogicManager;
class SCA_KeyboardManager;
class SCA_TimeEventManager;
class SCA_MouseManager;
class SCA_ISystem;
class SCA_IInputDevice;
class KX_NetworkMessageScene;
class KX_NetworkMessageManager;
class SG_Node;
class SG_Node;
class KX_Camera;
class KX_FontObject;
class KX_GameObject;
class KX_LightObject;
class RAS_MeshObject;
class RAS_BucketManager;
class RAS_MaterialBucket;
class RAS_IPolyMaterial;
class RAS_Rasterizer;
class RAS_DebugDraw;
class RAS_FrameBuffer;
class RAS_2DFilterManager;
class KX_2DFilterManager;
class BL_SceneConverter;
struct KX_ClientObjectInfo;
class KX_ObstacleSimulation;
struct TaskPool;

/*********EEVEE INTEGRATION************/
struct bNodeTree;
struct Mesh;
struct Object;
/**************************************/

typedef struct BackupObj {
  Object *ob;
  void *obtfm;
} BackupObj;

/**
 * The KX_Scene holds all data for an independent scene. It relates
 * KX_Objects to the specific objects in the modules.
 * */
class KX_Scene : public KX_PythonProxy, public SCA_IScene {
 public:
  enum DrawingCallbackType { PRE_DRAW = 0, POST_DRAW, PRE_DRAW_SETUP, MAX_DRAW_CALLBACK };

  struct AnimationPoolData {
    double curtime;
  };

 private:
  Py_Header

#ifdef WITH_PYTHON
      PyObject *m_attr_dict;
  PyObject *m_drawCallbacks[MAX_DRAW_CALLBACK];
  PyObject *m_removeCallbacks;
#endif

 protected:
  /***************EEVEE INTEGRATION*****************/
  Object *m_gameDefaultCamera;
  std::vector<struct Collection *> m_overlay_collections;
  struct GPUViewport *m_currentGPUViewport;
  /* In the current state of the code, we need this
   * to Initialize KX_BlenderMaterial and BL_Texture.
   * BL_Texture(s) is/are used for ImageRender.
   */
  struct GPUViewport *m_initMaterialsGPUViewport;
  KX_Camera *m_overlayCamera;
  std::vector<KX_Camera *> m_imageRenderCameraList;
  BL_SceneConverter *m_sceneConverter;
  bool m_isPythonMainLoop;
  std::vector<KX_GameObject *> m_kxobWithLod;
  std::map<Object *, char> m_obRestrictFlags;
  bool m_collectionRemap;
  std::vector<BackupObj *> m_backupObList;
  int m_backupOverlayFlag;
  int m_backupOverlayGameFlag;

  /* Objects to update at each render pass */
  /* Note: We could try to get the right render pass where
   * we need to update these objects but it would make
   * the code more complex. We can only do that for overlay render pass
   * because the other render pass can contain the same objects
   * which need to be notified + flushed again.
   */
  std::vector<std::pair<ID *, IDRecalcFlag>> m_idsToUpdateInAllRenderPasses;
  std::vector<std::pair<ID *, IDRecalcFlag>> m_idsToUpdateInOverlayPass;
  /*************************************************/

  RAS_BucketManager *m_bucketmanager;

  std::vector<KX_GameObject *> m_tempObjectList;

  /**
   * The list of objects which have been removed during the
   * course of one frame. They are actually destroyed in
   * LogicEndFrame() via a call to RemoveObject().
   */
  std::vector<KX_GameObject *> m_euthanasyobjects;

  EXP_ListValue<KX_GameObject> *m_objectlist;
  EXP_ListValue<KX_GameObject> *m_parentlist;  // all 'root' parents
  EXP_ListValue<KX_LightObject> *m_lightlist;
  EXP_ListValue<KX_GameObject> *m_inactivelist;  // all objects that are not in the active layer
  /// All animated objects, no need of EXP_ListValue because the list isn't exposed in python.
  std::vector<KX_GameObject *> m_animatedlist;

  /// The set of cameras for this scene
  EXP_ListValue<KX_Camera> *m_cameralist;
  /// The set of fonts for this scene
  EXP_ListValue<KX_FontObject> *m_fontlist;

  SG_QList m_sghead;  // list of nodes that needs scenegraph update
                      // the Dlist is not object that must be updated
                      // the Qlist is for objects that needs to be rescheduled
                      // for updates after udpate is over (slow parent, bone parent)

  /**
   * Various SCA managers used by the scene
   */
  SCA_LogicManager *m_logicmgr;
  SCA_KeyboardManager *m_keyboardmgr;
  SCA_MouseManager *m_mousemgr;
  SCA_TimeEventManager *m_timemgr;

  KX_PythonProxyManager m_proxyManager;

  /**
   * physics engine abstraction
   */
  // e_PhysicsEngine m_physicsEngine; //who needs this ?
  class PHY_IPhysicsEnvironment *m_physicsEnvironment;

  /**
   * The name of the scene
   */
  std::string m_sceneName;

  /**
   * \section Different scenes, linked to ketsji scene
   */

  /**
   * Network scene.
   */
  KX_NetworkMessageScene *m_networkScene;

  /**
   * A temporary variable used to parent objects together on
   * replication. Don't get confused by the name it is not
   * the scene's root node!
   */
  SG_Node *m_rootnode;

  /**
   * The active camera for the scene
   */
  KX_Camera *m_active_camera;
  /// The active camera for scene culling.
  KX_Camera *m_overrideCullingCamera;

  /**
   * Another temporary variable outstaying its welcome
   * used in AddReplicaObject to map game objects to their
   * replicas so pointers can be updated.
   */
  std::map<SCA_IObject *, SCA_IObject *> m_map_gameobject_to_replica;

  /**
   * Another temporary variable outstaying its welcome
   * used in AddReplicaObject to keep a record of all added
   * objects. Logic can only be updated when all objects
   * have been updated. This stores a list of the new objects.
   */
  std::vector<KX_GameObject *> m_logicHierarchicalGameObjects;

  /**
   * This temporary variable will contain the list of
   * object that can be added during group instantiation.
   * objects outside this list will not be added (can
   * happen with children that are outside the group).
   * Used in AddReplicaObject. If the list is empty, it
   * means don't care.
   */
  std::set<KX_GameObject *> m_groupGameObjects;

  /**
   * Pointer to system variable passed in in constructor
   * only used in constructor so we do not need to keep it
   * around in this class.
   */

  SCA_ISystem *m_kxsystem;

  /**
   * The execution priority of replicated object actuators?
   */
  int m_ueberExecutionPriority;

  /**
   * Toggle to enable or disable activity culling.
   */
  bool m_activityCulling;

  /**
   * Toggle to enable or disable culling via DBVT broadphase of Bullet.
   */
  bool m_dbvt_culling;

  /**
   * Occlusion culling resolution
   */
  int m_dbvt_occlusion_res;

  /**
   * The framing settings used by this scene
   */

  RAS_FrameSettings m_frame_settings;

  /**
   * This scenes viewport into the game engine
   * canvas.Maintained externally, initially [0,0] -> [0,0]
   */
  RAS_Rect m_viewport;

  /**
   * Visibility testing functions.
   */
  static void PhysicsCullingCallback(KX_ClientObjectInfo *objectInfo, void *cullingInfo);

  struct Scene *m_blenderScene;

  KX_2DFilterManager *m_filterManager;

  KX_ObstacleSimulation *m_obstacleSimulation;

  AnimationPoolData m_animationPoolData;
  TaskPool *m_animationPool;

  /**
   * LOD Hysteresis settings
   */
  bool m_isActivedHysteresis;
  int m_lodHysteresisValue;

  // Convert objects list & collection helpers
  void convert_blender_objects_list_synchronous(std::vector<Object *> objectslist);
  void convert_blender_collection_synchronous(Collection *co);

 public:
  KX_Scene(SCA_IInputDevice *inputDevice,
           const std::string &scenename,
           struct Scene *scene,
           class RAS_ICanvas *canvas,
           KX_NetworkMessageManager *messageManager);

  virtual ~KX_Scene();

  /******************EEVEE INTEGRATION************************/
  void ConvertBlenderObject(struct Object *ob);
  void ConvertBlenderObjectsList(std::vector<Object *> objectslist, bool asynchronous);
  void ConvertBlenderCollection(struct Collection *co, bool asynchronous);
  void ConvertBlenderAction(struct bAction *act);

  bool m_isRuntime;  // Too lazy to put that in protected
  std::vector<Object *> m_hiddenObjectsDuringRuntime;

  void RenderAfterCameraSetup(KX_Camera *cam,
                              class RAS_FrameBuffer *background_fb,
                              const RAS_Rect &viewport,
                              bool is_overlay_pass,
                              bool is_last_render_pass);
  void RenderAfterCameraSetupImageRender(KX_Camera *cam, const struct rcti *window);
  Object *GetGameDefaultCamera();
  void ReinitBlenderContextVariables();
  void AddOverlayCollection(KX_Camera *overlay_cam, struct Collection *collection);
  void RemoveOverlayCollection(struct Collection *collection);
  void SetCurrentGPUViewport(struct GPUViewport *viewport);
  struct GPUViewport *GetCurrentGPUViewport();
  void SetInitMaterialsGPUViewport(struct GPUViewport *viewport);
  struct GPUViewport *GetInitMaterialsGPUViewport();
  void SetOverlayCamera(KX_Camera *cam);
  KX_Camera *GetOverlayCamera();
  void AddImageRenderCamera(KX_Camera *cam);
  void RemoveImageRenderCamera(KX_Camera *cam);
  bool CameraIsInactive(KX_Camera *cam);
  void SetIsPythonMainLoop(bool isPython);
  void AddObjToLodObjList(KX_GameObject *gameobj);
  void RemoveObjFromLodObjList(KX_GameObject *gameobj);
  void BackupRestrictFlag(Object *ob, char restrictFlag);
  void RestoreRestrictFlags();
  void TagForCollectionRemap();
  KX_GameObject *GetGameObjectFromObject(Object *ob);
  void BackupObjectsMatToWorld(BackupObj *back);
  void RestoreObjectsMatToWorld();
  void TagForObjectsMatToWorldRestore();
  bool OrigObCanBeTransformedInRealtime(Object *ob);
  void IgnoreParentTxBGE(struct Main *bmain,
                         struct Depsgraph *depsgraph,
                         Scene *scene,
                         Object *ob,
                         std::vector<Object *> children);
  bool SomethingIsMoving();
  void AppendToIdsToUpdateInAllRenderPasses(ID *id, IDRecalcFlag flag);
  void AppendToIdsToUpdateInOverlayPass(ID *id, IDRecalcFlag flag);
  void TagForExtraIdsUpdate(Main *bmain, KX_Camera *cam);
  void TagBlenderPhysicsObject(Scene *scene, Object *ob);
  KX_GameObject *AddDuplicaObject(KX_GameObject *gameobj,
                                  KX_GameObject *reference,
                                  float lifespan);
  void OverlayPassDisableEffects(struct Depsgraph *depsgraph,
                                 KX_Camera *kxcam,
                                 bool isOverlayPass);
  /***************End of EEVEE INTEGRATION**********************/

  RAS_BucketManager *GetBucketManager() const;
  RAS_MaterialBucket *FindBucket(RAS_IPolyMaterial *polymat, bool &bucketCreated);

  /**
   * Update all transforms according to the scenegraph.
   */
  static bool KX_ScenegraphUpdateFunc(SG_Node *node, void *gameobj, void *scene);
  static bool KX_ScenegraphRescheduleFunc(SG_Node *node, void *gameobj, void *scene);
  void UpdateParents(double curtime);
  void DupliGroupRecurse(KX_GameObject *groupobj, int level);
  bool IsObjectInGroup(KX_GameObject *gameobj)
  {
    return (m_groupGameObjects.empty() ||
            m_groupGameObjects.find(gameobj) != m_groupGameObjects.end());
  }
  void AddObjectDebugProperties(KX_GameObject *gameobj);
  KX_GameObject *AddReplicaObject(KX_GameObject *gameobj,
                                  KX_GameObject *locationobj,
                                  float lifespan = 0.0f);
  KX_GameObject *AddNodeReplicaObject(SG_Node *node, KX_GameObject *gameobj);
  void RemoveNodeDestructObject(SG_Node *node, KX_GameObject *gameobj);
  void RemoveObject(KX_GameObject *gameobj);
  void RemoveDupliGroup(KX_GameObject *gameobj);
  void DelayedRemoveObject(KX_GameObject *gameobj);

  bool NewRemoveObject(KX_GameObject *gameobj);
  void ReplaceMesh(KX_GameObject *gameobj, RAS_MeshObject *mesh, bool use_gfx, bool use_phys);

  void AddAnimatedObject(KX_GameObject *gameobj);

  /**
   * \section Logic stuff
   * Initiate an update of the logic system.
   */
  void LogicBeginFrame(double curtime, double framestep);
  void LogicUpdateFrame(double curtime);
  void UpdateAnimations(double curtime);

  void LogicEndFrame();

  EXP_ListValue<KX_GameObject> *GetObjectList() const;
  EXP_ListValue<KX_GameObject> *GetInactiveList() const;
  EXP_ListValue<KX_GameObject> *GetRootParentList() const;
  EXP_ListValue<KX_LightObject> *GetLightList() const;

  SCA_LogicManager *GetLogicManager() const;

  SCA_TimeEventManager *GetTimeEventManager() const;

  KX_PythonProxyManager &GetPythonProxyManager();

  EXP_ListValue<KX_Camera> *GetCameraList() const;
  void SetCameraList(EXP_ListValue<KX_Camera> *camList);
  EXP_ListValue<KX_FontObject> *GetFontList() const;

  /** Find the currently active camera. */
  KX_Camera *GetActiveCamera();

  /**
   * Set this camera to be the active camera in the scene. If the
   * camera is not present in the camera list, it will be added
   */

  void SetActiveCamera(class KX_Camera *);

  KX_Camera *GetOverrideCullingCamera() const;
  void SetOverrideCullingCamera(KX_Camera *cam);

  /**
   * Move this camera to the end of the list so that it is rendered last.
   * If the camera is not on the list, it will be added
   */
  void SetCameraOnTop(class KX_Camera *);

  /**
   * Set the framing options for this scene
   */

  void SetFramingType(RAS_FrameSettings &frame_settings);

  /**
   * Return a const reference to the framing
   * type set by the above call.
   * The contents are not guaranteed to be sensible
   * if you don't call the above function.
   */

  const RAS_FrameSettings &GetFramingType() const;

  /**
   * \section Accessors to different scenes of this scene
   */
  void SetNetworkMessageScene(KX_NetworkMessageScene *newScene);
  KX_NetworkMessageScene *GetNetworkMessageScene();

  /// \section Debug draw.
  void RenderDebugProperties(RAS_DebugDraw &debugDraw,
                             int xindent,
                             int ysize,
                             int &xcoord,
                             int &ycoord,
                             unsigned short propsMax);

  /**
   * Replicate the logic bricks associated to this object.
   */

  void ReplicateLogic(class KX_GameObject *newobj);
  static SG_Callbacks m_callbacks;

  /// Update the mesh for objects based on level of detail settings
  void UpdateObjectLods(KX_Camera *cam);

  // LoD Hysteresis functions
  void SetLodHysteresis(bool active);
  bool IsActivedLodHysteresis();
  void SetLodHysteresisValue(int hysteresisvalue);
  int GetLodHysteresisValue();

  // Update the activity box settings for objects in this scene, if needed.
  void UpdateObjectActivity(void);

  // Enable/disable activity culling.
  void SetActivityCulling(bool b);

  // use of DBVT tree for camera culling
  void SetDbvtCulling(bool b)
  {
    m_dbvt_culling = b;
  }
  bool GetDbvtCulling()
  {
    return m_dbvt_culling;
  }
  void SetDbvtOcclusionRes(int i)
  {
    m_dbvt_occlusion_res = i;
  }
  int GetDbvtOcclusionRes()
  {
    return m_dbvt_occlusion_res;
  }

  void SetBlenderSceneConverter(class BL_SceneConverter *sceneConverter);
  class BL_SceneConverter *GetBlenderSceneConverter();

  class PHY_IPhysicsEnvironment *GetPhysicsEnvironment()
  {
    return m_physicsEnvironment;
  }

  void SetPhysicsEnvironment(class PHY_IPhysicsEnvironment *physEnv);

  void SetGravity(const MT_Vector3 &gravity);
  MT_Vector3 GetGravity();

  short GetAnimationFPS();

  /**
   * 2D Filters
   */
  RAS_2DFilterManager *Get2DFilterManager() const;
  RAS_FrameBuffer *Render2DFilters(RAS_Rasterizer *rasty,
                                   RAS_ICanvas *canvas,
                                   RAS_FrameBuffer *inputfb,
                                   RAS_FrameBuffer *targetfb);

  KX_ObstacleSimulation *GetObstacleSimulation()
  {
    return m_obstacleSimulation;
  }

  /**  Inherited from EXP_Value -- returns the name of this object. */
  virtual std::string GetName();

  /** Inherited from EXP_Value -- set the name of this object. */
  virtual void SetName(const std::string &name);

  virtual KX_Scene *NewInstance();

#ifdef WITH_PYTHON
  /* --------------------------------------------------------------------- */
  /* Python interface ---------------------------------------------------- */
  /* --------------------------------------------------------------------- */

  EXP_PYMETHOD_DOC(KX_Scene, addObject);
  EXP_PYMETHOD_DOC(KX_Scene, end);
  EXP_PYMETHOD_DOC(KX_Scene, restart);
  EXP_PYMETHOD_DOC(KX_Scene, replace);
  EXP_PYMETHOD_DOC(KX_Scene, get);
  EXP_PYMETHOD_DOC(KX_Scene, drawObstacleSimulation);
  EXP_PYMETHOD_DOC(KX_Scene, convertBlenderObject);
  EXP_PYMETHOD_DOC(KX_Scene, convertBlenderObjectsList);
  EXP_PYMETHOD_DOC(KX_Scene, convertBlenderCollection);
  EXP_PYMETHOD_DOC(KX_Scene, convertBlenderAction);
  EXP_PYMETHOD_DOC(KX_Scene, unregisterBlenderAction);
  EXP_PYMETHOD_DOC(KX_Scene, addOverlayCollection);
  EXP_PYMETHOD_DOC(KX_Scene, removeOverlayCollection);
  EXP_PYMETHOD_DOC(KX_Scene, getGameObjectFromObject);

  /* attributes */
  static PyObject *pyattr_get_name(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_objects(EXP_PyObjectPlus *self_v,
                                      const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_objects_inactive(EXP_PyObjectPlus *self_v,
                                               const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_lights(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_texts(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_cameras(EXP_PyObjectPlus *self_v,
                                      const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_filter_manager(EXP_PyObjectPlus *self_v,
                                             const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_active_camera(EXP_PyObjectPlus *self_v,
                                            const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_active_camera(EXP_PyObjectPlus *self_v,
                                      const EXP_PYATTRIBUTE_DEF *attrdef,
                                      PyObject *value);
  static PyObject *pyattr_get_overrideCullingCamera(EXP_PyObjectPlus *self_v,
                                                    const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_overrideCullingCamera(EXP_PyObjectPlus *self_v,
                                              const EXP_PYATTRIBUTE_DEF *attrdef,
                                              PyObject *value);
  static PyObject *pyattr_get_drawing_callback(EXP_PyObjectPlus *self_v,
                                               const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_drawing_callback(EXP_PyObjectPlus *self_v,
                                         const EXP_PYATTRIBUTE_DEF *attrdef,
                                         PyObject *value);
  static PyObject *pyattr_get_remove_callback(EXP_PyObjectPlus *self_v,
                                              const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_remove_callback(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef,
                                        PyObject *value);
  static PyObject *pyattr_get_gravity(EXP_PyObjectPlus *self_v,
                                      const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_gravity(EXP_PyObjectPlus *self_v,
                                const EXP_PYATTRIBUTE_DEF *attrdef,
                                PyObject *value);

  /* getitem/setitem */
  static PyMappingMethods Mapping;
  static PySequenceMethods Sequence;

  /**
   * Run the registered python drawing functions.
   */
  void RunDrawingCallbacks(DrawingCallbackType callbackType, KX_Camera *camera);
  void RunOnRemoveCallbacks();
#endif

  /**
   * Returns the Blender scene this was made from
   */
  struct Scene *GetBlenderScene()
  {
    return m_blenderScene;
  }

  bool MergeScene(KX_Scene *other);

  // void PrintStats(int verbose_level) {
  //	m_bucketmanager->PrintStats(verbose_level)
  //}
};

#ifdef WITH_PYTHON
bool ConvertPythonToScene(PyObject *value,
                          KX_Scene **scene,
                          bool py_none_ok,
                          const char *error_prefix);
#endif

typedef std::vector<KX_Scene *> KX_SceneList;
