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

#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "EXP_PyObjectPlus.h"
#include "EXP_Value.h"
#include "KX_CameraRenderMemory.h"
#include "KX_DepsgraphThreads.h"
#include "KX_DrawCulling.h"
#include "KX_OverlayResource.h"
#include "KX_PhysicsEngineEnums.h"
#include "KX_PlayerViews.h"
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
namespace blender { struct ID; }
namespace blender { struct Object; }
namespace blender { struct Mesh; }
namespace blender { struct Scene; }
namespace blender { struct Main; }
namespace blender { struct Depsgraph; }
namespace blender { struct bContext; }
namespace blender { struct GPUViewport; }
namespace blender { struct Collection; }
namespace blender { struct wmWindow; }
namespace blender { struct bScreen; }
namespace blender { struct ScrArea; }
namespace blender { struct ARegion; }
namespace blender { struct View3D; }
namespace blender { struct RegionView3D; }

template<class T> class EXP_ListValue;


class EXP_Value;
struct KX_DupliDrawGroups;
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
class LN_Manager;
// struct TaskPool; removed to avoid ambiguity

typedef struct BackupObj {
  blender::Object *ob;
  void *obtfm;
} BackupObj;

/** Why runtime ownership is being destroyed. Persistence uses this to distinguish gameplay
 * history from reconciliation and engine cleanup, including recursive scene-graph destruction. */
enum class KX_ObjectRemovalReason : uint8_t {
  Gameplay = 0,
  Lifetime,
  LoadReconcile,
  SceneTeardown,
  SystemCleanup,
};

#include "KX_OverlayResource.h"

/**
 * The KX_Scene holds all data for an independent scene. It relates
 * KX_Objects to the specific objects in the modules.
 * */
class KX_Scene : public KX_PythonProxy, public SCA_IScene {
 public:
  enum DrawingCallbackType { PRE_DRAW = 0, POST_DRAW, PRE_DRAW_SETUP, MAX_DRAW_CALLBACK };

 private:
  Py_Header

#ifdef WITH_PYTHON
      PyObject *m_attr_dict;
  PyObject *m_drawCallbacks[MAX_DRAW_CALLBACK];
  PyObject *m_removeCallbacks;
#endif

 protected:
  /***************EEVEE INTEGRATION*****************/
  blender::Object *m_gameDefaultCamera;
  std::vector<blender::Collection *> m_overlay_collections;
  /** Root replicas created solely to expose inactive templates in each overlay collection. */
  std::unordered_map<blender::Collection *, std::unordered_set<KX_GameObject *>>
      m_overlayCollectionReplicas;
  /** Every generated hierarchy member, including collection-instance members that may share a
   * Blender object. Used to keep the global overlay-only flag reference-safe. */
  std::unordered_map<blender::Collection *, std::unordered_set<KX_GameObject *>>
      m_overlayCollectionMembers;
  /** Active objects authored with Disable in Renders and exposed transiently by each overlay. */
  std::unordered_map<blender::Collection *, std::unordered_set<KX_GameObject *>>
      m_overlayCollectionShownObjects;
  /** Authored source object for each generated overlay member. */
  std::unordered_map<KX_GameObject *, blender::Object *> m_overlayMemberSources;
  /** Per-player menus: the copy of its overlay each generated member belongs to (absent: copy 0,
   * made when the overlay opened). */
  std::unordered_map<KX_GameObject *, int32_t> m_overlayMemberInstance;
  /** Each copy's object made from an authored object (not the grid slots copied from it later):
   * what a copy's logic finds by that object's name. */
  std::map<std::pair<const blender::Object *, int32_t>, KX_GameObject *> m_overlayCopyOfSource;
  /** Open overlays whose copy 0 was removed while another player's copy stayed. */
  std::unordered_set<const blender::Collection *> m_overlayCopyZeroRemoved;
  /** Overlays already warned that they cannot be copied for another player. */
  std::unordered_set<const blender::Collection *> m_overlayCopyRefusedWarned;
  /** The menu (lease handle) each copy belongs to: a copy is shown with its own menu. */
  std::map<std::pair<const blender::Collection *, int32_t>, int32_t> m_overlayInstanceLease;
  int32_t m_nextOverlayInstance = 1;
  /** Scene-local overlay interaction domain, rebuilt only when overlay ownership changes. */
  std::unordered_set<KX_GameObject *> m_overlayRuntimeObjects;
  /** Scene-local render overrides; never mutate authored Blender collection restriction flags. */
  std::unordered_map<blender::Collection *, bool> m_runtimeCollectionRenderVisibility;
  /** Collections reachable through at least one visible collection path, while there are render
   * overrides. An object is drawn when one of its collections (CollectionsOf) is among them. */
  std::unordered_set<const blender::Collection *> m_runtimeVisibleCollections;
  /** The file's collections in this scene (reached from its master collection): each authored
   * object's collections and each collection's parents, for collection membership while the game
   * runs. Built at first use, again after NoteCollectionsChanged. */
  struct CollectionIndex {
    bool built = false;
    std::unordered_map<const blender::Object *, std::vector<const blender::Collection *>>
        object_collections;
    std::unordered_map<const blender::Collection *, std::vector<const blender::Collection *>>
        parents;
    /** Each collection with the collections above it (found at first use). */
    std::unordered_map<const blender::Collection *, std::unordered_set<const blender::Collection *>>
        ancestors;
  };
  CollectionIndex m_collectionIndex;
  /** Changes with the collection index: every object finds its collections again. */
  uint32_t m_membershipVersion = 1;
  /** Counts Set Collection Member changes (and Load Game's), for CollectionMembers' cache. */
  uint64_t m_membershipEdits = 0;
  /** CollectionMembers' answers by (collection, with child collections), valid while the object
   * list, the membership version and the edits are unchanged. */
  struct CollectionMembersCache {
    uint64_t object_list_generation = 0;
    uint32_t membership_version = 0;
    uint64_t membership_edits = 0;
    std::vector<KX_GameObject *> members;
  };
  std::map<std::pair<const blender::Collection *, bool>, CollectionMembersCache>
      m_collectionMembersCache;
  void EnsureCollectionIndex();
  /** The collection and every collection above it (just itself outside the scene). */
  const std::unordered_set<const blender::Collection *> &CollectionAncestors(
      const blender::Collection *collection);
  /** The authored objects whose file collections `object` counts as in: its collection source
   * (a menu copy's authored object), and the Empty's for a collection instance's copy. */
  std::pair<const blender::Object *, const blender::Object *> CollectionSourcesOf(
      KX_GameObject *object) const;
  blender::GPUViewport *m_currentGPUViewport;
  /** Renderers shared by camera passes that keep no history and draw with equal settings
   * (KX_CameraRenderMemory::CanShareRenderer), such as two minimaps of the same size. */
  struct SharedGPUViewport {
    KX_CameraRenderMemory::PassSettings settings;
    blender::GPUViewport *viewport;
    double last_use;
  };
  std::vector<SharedGPUViewport> m_sharedGPUViewports;
  /** Live runtime replica used for overlay rendering and hit testing. */
  KX_Camera *m_overlayCamera;
  /** Inactive authored camera selected by the API; never activated or mutated in place. */
  KX_Camera *m_overlayCameraSource;
  /** Root and members owned exclusively by the live overlay camera instance. */
  KX_GameObject *m_overlayCameraReplicaRoot;
  std::unordered_set<KX_GameObject *> m_overlayCameraReplicaMembers;
  /** Independent-request and menu-lease ownership for instantiated overlay collections. */
  KX_OverlayResourceRegistry m_overlayResources;
  /** Use Viewport Render draws no overlay pass: the first overlay collection says so, once. */
  bool m_viewportRenderOverlayWarned = false;
  /** The same for Set Camera Visibility. */
  bool m_viewportRenderVisibilityWarned = false;
  /** Split screen: each player's camera and where its view is drawn. */
  KX_PlayerViews m_playerViews;
  /** Set Camera Visibility: what each camera does not draw. */
  struct CameraHiddenObject {
    const KX_Camera *camera;
    KX_GameObject *object;
    bool keep_shadow;
  };
  std::vector<CameraHiddenObject> m_cameraHiddenObjects;
  /** Objects Set Visible hid from the drawing only (KX_GameObject::m_drawHidden): hidden at each
   * camera pass with Set Camera Visibility's hiding, still in Blender's graph. */
  std::vector<KX_GameObject *> m_drawHiddenObjects;
  /** The frame RetireDrawHiddenObjects last looked at the list. */
  uint64_t m_drawHiddenRetireFrame = 0;
  /** Blender objects of removed ordinary copies, kept in Blender's graph (hidden at each camera
   * pass) for the next copy of the same source (KX_GameObject::ReplicateBlenderObject): adding
   * or deleting a Blender object rebuilds Blender's whole graph. */
  struct ParkedCopy {
    blender::Object *object;
    /** The source's session id (a source may be a copy that goes too). */
    uint32_t source_uid;
    uint64_t since;
  };
  std::vector<ParkedCopy> m_parkedCopies;
  uint64_t m_parkedRetireFrame = 0;
  /** Copies made now are spawns (Add Object): they may take a parked Blender object and be
   * parked when removed. Other copies (menus, overlays, UI drag pictures) are made and deleted
   * as before. */
  int m_copyReuseDepth = 0;
  /** Draw As Auto: each group's Blender object (a copy of a member made at game start, deleted at
   * the end), which its members are drawn from (bge_dupli_provider); hidden itself at each camera
   * pass. */
  std::vector<blender::Object *> m_sharedDrawBases;
  std::unordered_set<const blender::Object *> m_sharedDrawBaseSet;
  /** The grouped (Auto) objects by their own Blender object, out of the graph while grouped: a
   * change queued for that object (AppendToIdsToUpdate) makes it leave its group or redraws the
   * group. */
  std::unordered_map<const blender::Object *, KX_GameObject *> m_sharedMemberOfObject;
  /** Draw As Merged: a Blender object made at game start from nearby still objects, deleted at
   * the end. Its members' parts collapse onto a point to hide them. Owned (new/delete). */
  struct MergedBatch {
    blender::Object *object = nullptr;
    blender::Mesh *mesh = nullptr;
    /** Vertex positions as merged (x, y, z), to show a hidden part again. */
    std::vector<float> rest_positions;
  };
  std::vector<MergedBatch *> m_mergedBatches;
  void SetupAutoDrawing(blender::Depsgraph *depsgraph, const std::vector<KX_GameObject *> &objects);
  void DeleteSharedDrawBases();
  void SetupMergedDrawing(blender::Depsgraph *depsgraph,
                          const std::vector<KX_GameObject *> &objects);
  void DeleteMergedBatches();
  /** Deletes parked copies (those parked before \a before), in one go. */
  void DeleteParkedCopies(uint64_t before);
  /** The evaluated objects HideCameraHiddenObjects changed, with their flags before. */
  std::vector<std::pair<blender::Object *, int>> m_cameraHiddenRestore;
  /** The camera drawing between HideCameraHiddenObjects and RestoreCameraHiddenObjects. */
  const KX_Camera *m_cameraHiddenPass = nullptr;
  /** That pass draws with Workbench (no shadows, so Keep Shadow hides fully). */
  bool m_cameraHiddenPassWorkbench = false;
  /** That camera's Collection setting: index in m_cameraCollections (-1: none), Hide mode. */
  int m_cameraHiddenPassCollection = -1;
  bool m_cameraHiddenPassCollectionHide = false;
  /** That pass's draw culling (CulledInCameraPass). */
  struct CullPass {
    /** Counts the passes: an object's answer is given once a pass. */
    uint64_t id = 0;
    /** Clip End culling; draw distances (the camera's "use_draw_distances" custom property). */
    bool clip = false;
    bool distances = false;
    /** The camera's bit in the objects' culled cameras (0 past 32 cameras: no hysteresis). */
    uint32_t camera_bit = 0;
    /** The camera's renderer draws anew this pass: its view or Blender's update count changed
     * since its last pass. Otherwise a culling change is told to the renderers and applied at
     * the next pass (CulledInCameraPass). */
    bool redraws = true;
    KX_DrawCulling::View view;
    /** The lights casting shadows, whose shadows Clip End culling keeps. */
    std::vector<KX_DrawCulling::Light> lights;
    blender::Depsgraph *depsgraph = nullptr;
  };
  CullPass m_cullPass;
  /** The cameras culling has met, by bit, and what each one's last pass drew with. */
  std::vector<const KX_Camera *> m_cullCameras;
  struct CullCameraPass {
    bool known = false;
    KX_DrawCulling::View view;
    uint64_t update_count = 0;
  };
  std::vector<CullCameraPass> m_cullCameraPasses;
  void PrepareCullPass(blender::Depsgraph *depsgraph,
                       KX_Camera *camera,
                       bool workbench,
                       bool is_overlay_pass);
  /** The largest "draw_distance" custom property of `object`'s collections (or of their nearest
   * parents having one), 0 without one. */
  float CollectionDrawDistance(KX_GameObject *object);
  /** CulledInCameraPass's answer for the pass, worked out once. */
  bool CullDecision(KX_GameObject *object);
  /** The cameras' Collection settings met so far (Camera::game_collection). Bit i of a game
   * object's camera collection bits: it is in m_cameraCollections[i] (child collections
   * included). The session UID tells a new collection at the address of a freed one. */
  struct CameraCollection {
    const blender::Collection *collection;
    uint32_t session_uid;
  };
  std::vector<CameraCollection> m_cameraCollections;
  /** Changes with m_cameraCollections and the collection index: the bits are found again. */
  uint32_t m_cameraCollectionsVersion = 1;
  std::vector<KX_Camera *> m_imageRenderCameraList;
  BL_SceneConverter *m_sceneConverter;
  bool m_isPythonMainLoop;
  std::vector<KX_GameObject *> m_kxobWithLod;
  /** Drawable copies of borrowed meshes whose objects the depsgraph never evaluates. */
  std::unordered_map<RAS_MeshObject *, blender::Mesh *> m_replaceMeshEvalMeshes;
  std::map<blender::Object *, short> m_obVisibilityFlag;
  /** Dupli Bases disabled in viewports that the game keeps in Blender's graph
   * (KeepDupliBaseInGraph), and what it changed for that, undone by RestoreVisibilityFlag. */
  struct DupliBaseInGraph {
    blender::Object *object = nullptr;
    /** It closed the base's eye; it linked the base into the scene's own collection (all of its
     * own collections were disabled). */
    bool eye_closed = false;
    bool linked = false;
  };
  std::vector<DupliBaseInGraph> m_dupliBasesInGraph;
  /** Objects of scenes merged into this one (Load Scene) that the merge linked into this scene's
   * own collection, so that this scene's graph draws them (AdoptMergedScene). Unlinked by
   * RestoreVisibilityFlag; left out of the collection index (their own scene's collections
   * still gather them). */
  std::unordered_set<blender::Object *> m_mergedObjectLinks;
  /** The Blender scenes merged into this one: their collections are this scene's too
   * (EnsureCollectionIndex). */
  std::vector<const blender::Scene *> m_mergedBlenderScenes;
  /** Converted to be merged into another scene, then deleted (Load Scene, LibLoad): the merge,
   * not its conversion, brings its objects into the drawing (Game Drawing, Dupli Bases). */
  bool m_mergeSource = false;
  /** MergeScene's part for the drawing: `other`'s bookkeeping becomes this scene's, its objects
   * go into this scene's graph as they were in their own, and Game Drawing groups them as the
   * game's start does. `merged` are all of other's objects, `active` its placed ones. */
  void AdoptMergedScene(KX_Scene *other,
                        const std::vector<KX_GameObject *> &merged,
                        const std::vector<KX_GameObject *> &active);
  /** Draw identities of Dupli copies that are gone (KX_GameObject::SetDupliDrawIdentity), with
   * the frame they went at. Renderers keep the addresses as keys until their next draw, so they
   * are freed a few frames later: a new copy at the same address would take over the old copy's
   * motion. Owned (allocated with new). */
  std::deque<std::pair<uint64_t, blender::Object *>> m_retiredDupliDrawOriginals;
  bool m_collectionRemap;
  bool m_relationsUpdatePending;
  /** Counts the Blender relation rebuilds the engine asked for (spawns, removals, visibility,
   * Full Copy): objects choose their pose path again after one (KX_GameObject::PosePath). */
  uint64_t m_renderSyncRelations = 1;
  /** Counts camera passes (UpdateDepsgraph). */
  uint64_t m_renderSyncPasses = 0;
  /** The pass's objects whose moves skip Blender's evaluation (KX_GameObject::WriteDirectPose
   * after the update), and those to look at after it (TagForTransformUpdateEvaluated). */
  std::vector<KX_GameObject *> m_directPoseObjects;
  std::vector<KX_GameObject *> m_poseCheckObjects;
  std::vector<BackupObj *> m_backupObList;
  int m_backupOverlayFlag;

  /** True when any active runtime scene still renders this Blender object as an overlay. */
  bool AnyRuntimeOverlayContainsObject(blender::Object *object) const;
  /** Restore transient overlay flags before this runtime scene releases its objects. */
  void ClearOverlayStateForTeardown();
  void RebuildOverlayRuntimeObjectIndex();
  /** Collection visibility of one object: Set Collection Visibility on the collections it is in
   * and, for overlay objects, on the overlay collection itself. */
  void ApplyRuntimeCollectionRenderVisibility(KX_GameObject *game_object);
  void RebuildRuntimeCollectionRenderVisibility();
  void ApplyOverlayResourceVisibility(blender::Collection *collection);
  /** Records a replica hierarchy as content of \a collection (copy \a instance of it); false
   * when staging failed. */
  bool RegisterOverlayReplica(blender::Main &bmain,
                              blender::Scene &blender_scene,
                              blender::Collection *collection,
                              KX_GameObject *replica,
                              const std::vector<KX_GameObject *> &members,
                              const std::unordered_map<KX_GameObject *, KX_GameObject *> &mapping,
                              int32_t instance = 0);
  /** Copies the roots of \a templates into the open overlay \a collection as its copy
   * \a instance; false when a copy failed. */
  bool ReplicateOverlayTemplates(blender::Main &bmain,
                                 blender::Scene &blender_scene,
                                 blender::Collection *collection,
                                 const std::vector<KX_GameObject *> &templates,
                                 int32_t instance,
                                 std::map<SCA_IObject *, SCA_IObject *> &r_copies,
                                 std::vector<KX_GameObject *> &r_members,
                                 bool &r_created);
  /** Drops \a member from m_overlayCopyOfSource; call before forgetting its source. */
  void ForgetOverlayCopy(KX_GameObject *member);
  /** Copies the open overlay \a collection's hidden templates as its copy \a instance. */
  bool CopyOverlayTemplates(blender::Collection *collection, int32_t instance);
  /** Every overlay resource's visibility, after something reset the layers it uses. */
  void ApplyAllOverlayResourceVisibility();
  KX_Camera *ResolveOverlayRequestCamera(KX_Camera *requested) const;
  void RestoreOverlayCameraViewState(KX_Camera *runtime_camera);
  bool CreateOverlayCameraRuntimeInstance(KX_Camera *source_camera,
                                          blender::Main &bmain,
                                          blender::Scene &scene);
  void DestroyOverlayCameraRuntimeInstance(bool queue_removal);

  /* Ids to tag for update. Those for every pass are tagged at the next pass and dropped: the
   * evaluation stays for the frame's later passes, whose renderers find the new update stamps.
   * Those of the menu/HUD overlay are tagged at the overlay pass. */
  std::vector<std::pair<blender::ID *, blender::IDRecalcFlag>> m_idsToUpdateInAllRenderPasses;
  std::vector<std::pair<blender::ID *, blender::IDRecalcFlag>> m_idsToUpdateInOverlayPass;
  /** Whether the updates use Blender's worker threads (UpdateDepsgraph). */
  KX_DepsgraphThreads m_depsgraphThreads;
  /*************************************************/

  RAS_BucketManager *m_bucketmanager;

  std::vector<KX_GameObject *> m_tempObjectList;
  /** Objects that required render interpolation in the previous frame. */
  std::vector<KX_GameObject *> m_interpolatedObjects;
  /** An object here chose Interpolate or Extrapolate (once set, it stays: a cheap guard). */
  bool m_hasSmoothMotion = false;
  /** Frames drawn with Smooth Motion (blend fades compare frame numbers). */
  uint64_t m_smoothMotionFrame = 0;
  /** Skip render interpolation for objects marked invisible by the renderer. */
  bool m_skipInvisibleInterpolation;

  /**
   * The list of objects which have been removed during the
   * course of one frame. They are actually destroyed in
   * LogicEndFrame() via a call to RemoveObject().
   */
  std::vector<KX_GameObject *> m_euthanasyobjects;
  std::unordered_map<KX_GameObject *, KX_ObjectRemovalReason> m_euthanasyRemovalReasons;
  KX_ObjectRemovalReason m_activeRemovalReason = KX_ObjectRemovalReason::SystemCleanup;

  EXP_ListValue<KX_GameObject> *m_objectlist;
  uint64_t m_objectListGeneration = 0;
  EXP_ListValue<KX_GameObject> *m_parentlist;  // all 'root' parents
  EXP_ListValue<KX_LightObject> *m_lightlist;
  EXP_ListValue<KX_GameObject> *m_inactivelist;  // all objects that are not in the active layer
  /// All animated objects, no need of EXP_ListValue because the list isn't exposed in python.
  std::vector<KX_GameObject *> m_animatedlist;
  /** During UpdateAnimations: Blender's depsgraph, looked up at the first request. */
  bool m_animationUpdate = false;
  blender::Depsgraph *m_animationDepsgraph = nullptr;
  uint64_t m_animationUpdateCount = 0;
#ifdef WITH_GAMEENGINE_LOGICNODES
  std::unordered_set<KX_GameObject *> m_pauseAnimatedObjects;
#endif

  /// The set of cameras for this scene
  EXP_ListValue<KX_Camera> *m_cameralist;
  /// The set of fonts for this scene
  EXP_ListValue<KX_FontObject> *m_fontlist;
  std::vector<KX_GameObject *> m_upbgeDupliInstancesList;
  /** The copies of the current camera pass drawn as groups (BeginDupliDrawGroups), and their
   * memory, kept between passes. */
  std::shared_ptr<KX_DupliDrawGroups> m_dupliDrawGroups;
  /** The grouped pass being drawn (0: none), and the last one's number. */
  uint64_t m_dupliGroupPass = 0;
  uint64_t m_dupliGroupPassCount = 0;

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

#ifdef WITH_GAMEENGINE_LOGICNODES
  std::shared_ptr<LN_Manager> m_logicNodeManager;
#endif

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
  KX_NetworkMessageScene *m_networkMessageScene;

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

  /** Optional callers collecting every object created by nested hierarchy replication. */
  std::vector<std::vector<KX_GameObject *> *> m_activeReplicaCaptures;
  /** Optional callers collecting authored-source identity for nested hierarchy replication. */
  std::vector<std::unordered_map<KX_GameObject *, KX_GameObject *> *>
      m_activeReplicaMappingCaptures;

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

  blender::Scene *m_blenderScene;

  KX_2DFilterManager *m_filterManager;

  KX_ObstacleSimulation *m_obstacleSimulation;

  /**
   * LOD Hysteresis settings
   */
  bool m_isActivedHysteresis;
  int m_lodHysteresisValue;
  /** Level of detail is decided at the frame's first world camera pass (UpdateObjectLods). */
  bool m_lodUpdateDue = false;

  // Convert objects list & collection helpers
  void convert_blender_objects_list_synchronous(std::vector<blender::Object *> objectslist);
  void convert_blender_collection_synchronous(blender::Collection *co);

#ifdef WITH_GAMEENGINE_LOGICNODES
  void ReplicateLogicNodeRuntimeTreesForReplicaMap();
#endif

 public:
  KX_Scene(SCA_IInputDevice *inputDevice,
           const std::string &scenename,
           blender::Scene *scene,
           class RAS_ICanvas *canvas,
           KX_NetworkMessageManager *messageManager);

  virtual ~KX_Scene();

#ifdef WITH_GAMEENGINE_LOGICNODES
  virtual void ProcessReplica() override;
#endif

  /******************EEVEE INTEGRATION************************/
  void ConvertBlenderObject(blender::Object *ob);
  void ConvertBlenderObjectsList(std::vector<blender::Object *> objectslist, bool asynchronous);
  void ConvertBlenderCollection(blender::Collection *co, bool asynchronous);
  void ConvertBlenderAction(blender::bAction *act);

  bool m_isRuntime;  // Too lazy to put that in protected

  void RenderAfterCameraSetup(KX_Camera *cam,
                              class RAS_FrameBuffer *background_fb,
                              const RAS_Rect &viewport,
                              bool is_overlay_pass,
                              bool is_last_render_pass);
  /** Makes the renderer `cam` draws this pass through the current one: the camera's own, or one
   * shared with other passes of equal settings. */
  void PrepareGPUViewport(KX_Camera *cam, const KX_CameraRenderMemory::PassSettings &settings);
  /** Gives back the renderers of cameras that have not drawn for KX_CameraRenderMemory::
   * IdleSeconds (a minimap turned off, a closed menu), and of unused shared ones. Never those in
   * `in_use` (each scene's current renderer) nor those of render-to-texture cameras. Needs the
   * GPU context. */
  void FreeIdleGPUViewports(double now, const std::set<blender::GPUViewport *> &in_use);
  void FreeSharedGPUViewports();
  /** Before a camera pass: hands Blender what changed since the last pass and evaluates it. A
   * pass that finds nothing new evaluates nothing (the renderers then keep their samples). */
  void UpdateDepsgraph(blender::Main *bmain, blender::Scene *scene, bool is_overlay_pass, KX_Camera *cam);
  bool ViewportRender(KX_Camera *cam,
                      const RAS_Rect &viewport,
                      const blender::rcti &window,
                      RAS_ICanvas *canvas,
                      blender::Scene *scene,
                      blender::bContext *C);

  void RenderAfterCameraSetupImageRender(KX_Camera *cam, const blender::rcti *window);
  blender::Object *GetGameDefaultCamera();
  void ReinitBlenderContextVariables();
  /** What building a scene changes in the editor's window: its constructor switches the window
   * to its Blender scene (ED_screen_scene_change: the window's scene and view layer, the 3D
   * views' cameras, a 3D view out of camera view when that scene has no camera). A scene built
   * only to be merged puts it all back (KX_KetsjiEngine::MergeScene): before, the editor's view
   * left camera view after a game that loaded a scene without a camera. */
  struct EditorViewState {
    /** Blender's context as it was (the scene's constructor sets it to its own). */
    blender::wmWindow *context_window = nullptr;
    blender::bScreen *context_screen = nullptr;
    blender::ScrArea *context_area = nullptr;
    blender::ARegion *context_region = nullptr;
    blender::Scene *context_scene = nullptr;
    blender::wmWindow *window = nullptr;
    blender::Scene *scene = nullptr;
    std::string view_layer_name;
    std::vector<std::pair<blender::View3D *, blender::Object *>> view_cameras;
    struct RegionView {
      blender::RegionView3D *region_view = nullptr;
      int persp = 0;
      int rflag = 0;
    };
    std::vector<RegionView> region_views;
  };
  static EditorViewState SaveEditorViewState();
  static void RestoreEditorViewState(const EditorViewState &state);
  /** Validate without mutating runtime or Blender scene state. */
  KX_OverlayCollectionResult ValidateAddOverlayCollection(
      KX_Camera *overlay_cam, blender::Collection *collection);
  KX_OverlayCollectionResult ValidateRemoveOverlayCollection(
      blender::Collection *collection);
  KX_OverlayCollectionResult AddOverlayCollection(
      KX_Camera *overlay_cam,
      blender::Collection *collection,
      bool report_failure = true);
  KX_OverlayCollectionResult RemoveOverlayCollection(
      blender::Collection *collection,
      bool report_failure = false,
      bool notify_logic_nodes = true);
  KX_OverlayCollectionResult AcquireOverlayMenuLease(
      KX_Camera *overlay_cam,
      blender::Collection *collection,
      int32_t menu_handle,
      bool visible);
  KX_OverlayCollectionResult ReleaseOverlayMenuLease(
      blender::Collection *collection,
      int32_t menu_handle);
  void SetOverlayMenuLeaseVisible(
      blender::Collection *collection, int32_t menu_handle, bool visible);
  uint64_t GetOverlayMembershipGeneration(const blender::Collection *collection) const;
  void NoteOverlayMembershipChanged(blender::Collection *collection);
  /** The open overlay that holds \a object (a member, or a hidden template of one), or null. */
  blender::Collection *GetOverlayCollectionOf(KX_GameObject *object) const;
  /**
   * Makes a runtime copy (\a mapping: original to copy, from AddReplicaObject) of an object of an
   * open overlay part of that overlay: drawn and clicked there, removed when it closes. Copies of
   * other objects are left alone. A copy of a hidden template joins the player's copy of the
   * overlay that \a adder (the object whose logic added it, or where it was added) belongs to.
   */
  void AdoptOverlayReplica(KX_GameObject *replica,
                           const std::vector<KX_GameObject *> &members,
                           const std::unordered_map<KX_GameObject *, KX_GameObject *> &mapping,
                           const KX_GameObject *adder = nullptr);
  bool OverlayWouldExistWithoutLease(const blender::Collection *collection,
                                     int32_t menu_handle) const;
  KX_Camera *GetOverlayCameraSource();
  /** Append active replicas generated for an overlay collection, including hierarchy and
   * collection-instance members that are not direct Blender collection entries. */
  void AppendOverlayCollectionRuntimeObjects(blender::Collection *collection,
                                             std::vector<KX_GameObject *> &r_objects,
                                             int32_t instance = -1) const;
  /** Return whether a live runtime object belongs to this authored overlay collection (and to
   * its copy \a instance, unless -1). */
  bool RuntimeObjectBelongsToOverlayCollection(KX_GameObject *game_object,
                                               const blender::Collection *collection,
                                               int32_t instance = -1) const;
  /** Append every live overlay object that belongs to `collection`: the set
   * RuntimeObjectBelongsToOverlayCollection accepts, in time linear to the collection's size. */
  void AppendOverlayRuntimeObjectsInCollection(blender::Collection *collection,
                                               std::vector<KX_GameObject *> &r_objects,
                                               int32_t instance = -1) const;
  /**
   * Per-player menus: another full copy of the open overlay \a collection (its hidden templates
   * copied again), shown with the menu \a menu_handle. Returns the copy's number (> 0), or -1
   * (also for a collection whose objects are active scene objects shown from Disable in Renders:
   * those cannot be copied). Copy 0 is the one made when the overlay opened.
   */
  int32_t AddOverlayInstance(blender::Collection *collection, int32_t menu_handle);
  /** Copy 0 is there (not removed by RemoveOverlayInstance while another copy stayed). */
  bool OverlayHasCopyZero(const blender::Collection *collection) const
  {
    return !m_overlayCopyZeroRemoved.contains(collection);
  }
  /** Removes one copy of an open overlay (its objects); the overlay stays open. */
  void RemoveOverlayInstance(blender::Collection *collection, int32_t instance);
  /** Copy \a instance is shown with the menu \a menu_handle (0: with the overlay itself). */
  void SetOverlayInstanceLease(blender::Collection *collection,
                               int32_t instance,
                               int32_t menu_handle);
  /** The copy of its overlay an object belongs to (0 for the first copy and other objects). */
  int32_t GetOverlayInstanceOf(const KX_GameObject *object) const;
  /**
   * Per-player menus: when \a viewer is in a copy of an open overlay and \a object is the same
   * overlay's object in another copy, the viewer's copy of it (a menu's logic finds its own
   * objects by name), or null when the viewer's copy no longer has it. Else \a object.
   */
  KX_GameObject *OverlayCounterpart(KX_GameObject *object, const KX_GameObject *viewer) const;
  /** Opened by Set Overlay Collection (not only by menus). */
  bool OverlayOpenedIndependently(const blender::Collection *collection) const
  {
    return m_overlayResources.IndependentEnabled(collection);
  }
  bool IsOverlayRuntimeObject(const KX_GameObject *game_object) const;
  bool SetRuntimeCollectionRenderVisibility(blender::Collection *collection,
                                            bool visible,
                                            bool include_child_collections);
  void SetCurrentGPUViewport(blender::GPUViewport *viewport);
  blender::GPUViewport *GetCurrentGPUViewport();
  void SetOverlayCamera(KX_Camera *cam);
  KX_Camera *GetOverlayCamera();
  /** Return whether `camera` is either the authored overlay-camera template or its live runtime
   * instance. Inactive overlay cameras are replicated, so pointer identity alone is insufficient
   * for ownership checks after the first collection is enabled. */
  bool OverlayCameraMatches(const KX_Camera *camera) const;
  void AddImageRenderCamera(KX_Camera *cam);
  void RemoveImageRenderCamera(KX_Camera *cam);
  bool CameraIsInactive(KX_Camera *cam);
  void SetIsPythonMainLoop(bool isPython);
  void AddObjToLodObjList(KX_GameObject *gameobj);
  void RemoveObjFromLodObjList(KX_GameObject *gameobj);
  void BackupVisibilityFlag(blender::Object *ob, short visibilityFlag);
  /** A runtime-converted object being deleted: nothing is given back to it at the end. */
  void ForgetVisibilityFlag(blender::Object *ob);
  /** A Dupli Base disabled in viewports (its own setting or its collections') goes into
   * Blender's graph for the game, hidden with its eye as the documentation asks: its copies are
   * drawn from its evaluated object, and outside the graph nothing told the renderers they moved
   * (a still view kept the old picture) and they drew unevaluated data. False when it cannot be
   * done. RestoreVisibilityFlag puts it back. */
  bool KeepDupliBaseInGraph(blender::Object *ob);
  /** A Blender object the game deletes (a copy): the merge's link of it goes with it. */
  void ForgetMergedLink(blender::Object *ob);
  /** Converted to be merged into another scene (m_mergeSource). */
  void SetMergeSource(bool merge_source)
  {
    m_mergeSource = merge_source;
  }
  bool IsMergeSource() const
  {
    return m_mergeSource;
  }
  void RestoreVisibilityFlag();
  void TagForCollectionRemap();
  void TagForRelationsUpdate();
  /** Set Visible's hide from the drawing only (KX_GameObject::m_drawHidden). */
  void AddDrawHidden(KX_GameObject *object);
  void RemoveDrawHidden(KX_GameObject *object);
  /** Objects hidden from the drawing for a while leave Blender's graph together, in one rebuild:
   * in the graph each still costs a share of every update. */
  void RetireDrawHiddenObjects(uint64_t frame);
  /** The pool of removed copies' Blender objects (m_parkedCopies). ParkCopy keeps one (false:
   * delete it as before), TakeParkedCopy hands one out for a new copy of \a source (nullptr:
   * make one). Parked for a while, they are deleted together. */
  bool ParkCopy(blender::Object *object, uint32_t source_uid);
  blender::Object *TakeParkedCopy(const blender::Object *source);
  void RetireParkedCopies(uint64_t frame);
  /** Game start (BL_ConvertBlenderObjects) and a scene merge (AdoptMergedScene): of `objects`
   * (placed objects), identical mesh objects set to Draw As Auto become instances of one Blender
   * object per group, those set to Merged are joined into still-scenery batches; the members' own
   * Blender objects leave the graph. `depsgraph` holds `objects`. */
  void SetupGameDrawing(blender::Depsgraph *depsgraph,
                        const std::vector<KX_GameObject *> &objects);
  /** An Auto group's Blender object (never converted to a game object). */
  bool IsSharedDrawBase(const blender::Object *object) const
  {
    return m_sharedDrawBaseSet.count(object) != 0;
  }
  /** A grouped object leaves its group or goes. */
  void ForgetSharedMember(KX_GameObject *object);
  /** Collapses (hides) or restores a Merged object's part of its batch. */
  void SetMergedPartShown(int batch, uint32_t first_vertex, uint32_t vertex_count, bool shown);
  /** Around an Add Object (actuator, Python, Logic Nodes): its copies use the pool. */
  struct CopyReuseScope {
    KX_Scene &scene;
    explicit CopyReuseScope(KX_Scene &s) : scene(s)
    {
      ++scene.m_copyReuseDepth;
    }
    ~CopyReuseScope()
    {
      --scene.m_copyReuseDepth;
    }
  };
  bool CopyReuseActive() const
  {
    return m_copyReuseDepth > 0;
  }
  /** Lists an object for the camera pass in progress (UpdateDepsgraph). */
  void NoteDirectPose(KX_GameObject *object)
  {
    m_directPoseObjects.push_back(object);
  }
  void NotePoseCheck(KX_GameObject *object)
  {
    m_poseCheckObjects.push_back(object);
  }
  KX_GameObject *GetGameObjectFromObject(blender::Object *ob);
  void BackupObjectsMatToWorld(BackupObj *back);
  void RestoreObjectsMatToWorld();
  void TagForObjectsMatToWorldRestore();
  bool OrigObCanBeTransformedInRealtime(blender::Object *ob);
  void IgnoreParentTxBGE(blender::Main *bmain,
                         blender::Depsgraph *depsgraph,
                         blender::Scene *scene,
                         blender::Object *ob,
                         std::vector<blender::Object *> children);
  void ReserveIdsToUpdate(size_t additional_all_render_passes,
                          size_t additional_overlay_pass);
  void AppendToIdsToUpdate(blender::ID *id, blender::IDRecalcFlag flag, bool in_overlay_collection_only);
  void TagForExtraIdsUpdate(blender::Main *bmain, KX_Camera *cam);
  void TagBlenderPhysicsObject(blender::Scene *scene, blender::Object *ob);
  KX_GameObject *AddFullCopyObject(KX_GameObject *gameobj,
                                   KX_GameObject *reference,
                                   float lifespan);
  /** What `kxcam` draws a pass of `width` x `height` pixels with, in a window of `window_pixels`
   * (0: no window share, as for render to texture): its Game Camera Settings (Use Workbench and
   * the effects it turns off), in a split screen player's view Split Screen Quality, and the
   * shadow pool for its share of the window. */
  KX_CameraRenderMemory::PassSettings CameraPassSettings(KX_Camera *kxcam,
                                                         bool isOverlayPass,
                                                         int width,
                                                         int height,
                                                         int64_t window_pixels);
  /** Applies `settings` (CameraPassSettings) to the evaluated scene for this camera's pass, until
   * the scene's own settings are restored after the draw. True: draw one sample only. */
  bool DisableCameraEffects(blender::Depsgraph *depsgraph,
                            KX_Camera *kxcam,
                            bool isOverlayPass,
                            const KX_CameraRenderMemory::PassSettings &settings);
  /** Set Camera Visibility: `camera` does not draw `object` (Keep Shadow: it still casts shadows
   * and shows in reflections there), or draws it again. The menu camera's settings apply to its
   * live copy. */
  void SetCameraVisibility(KX_Camera *camera, KX_GameObject *object, bool visible, bool keep_shadow);
  /** The cameras draw `object` differently from now on, by a change Blender does not see (the
   * per-camera hide rules, applied around each draw). A renderer that finished its samples shows
   * its last picture until the depsgraph changes, so the object is updated once. */
  void NoteDrawChanged(KX_GameObject *object);
  /** `camera` (or the menu camera of which it is the live copy) does not draw `object`. */
  bool IsHiddenForCamera(const KX_GameObject *object, const KX_Camera *camera) const;
  /** Hides from this camera's draw the objects it does not draw (Set Camera Visibility, its
   * Collection setting, draw culling), until RestoreCameraHiddenObjects (the evaluated copies
   * only: the objects and the file never change). A Workbench pass draws no shadows: Keep Shadow
   * hides fully. */
  void HideCameraHiddenObjects(blender::Depsgraph *depsgraph,
                               KX_Camera *camera,
                               bool workbench,
                               bool is_overlay_pass = false);
  void RestoreCameraHiddenObjects();
  /** An instanced copy (UPBGE Dupli Base) shares its Blender object with the others, so its draw
   * copy is hidden instead: the camera drawing now does not draw it. `r_keep_shadow`: it still
   * casts shadows there. */
  bool IsHiddenInCameraPass(KX_GameObject *object, bool &r_keep_shadow);
  /** Draw culling (KX_DrawCulling): the camera drawing now leaves `object` out, being entirely
   * beyond its Clip End with no shadow reaching the view, or farther than its draw distance. A
   * change for that camera is told to the renderers (NoteDrawChanged). */
  bool CulledInCameraPass(KX_GameObject *object);
  /** The index of a camera's Collection setting in m_cameraCollections (added at first use), or
   * -1 without one or past 32 collections. */
  int CameraCollectionIndex(const blender::Collection *collection);
  /** True when a camera's Collection setting (`index`, `hide`: Hide mode) leaves `object` out of
   * its draw (IsInCollection, child collections included). Lights, light probes and cameras are
   * never left out. */
  bool HiddenByCameraCollection(KX_GameObject *object, int index, bool hide);

  /* Collection membership while the game runs, the one answer for cameras' Collection settings,
   * Set Collection Render Visibility and Get Collection Objects. An object is directly in the
   * collections its collection source (KX_GameObject::GetCollectionSource: an Add Object copy's
   * source) is linked to in the file, and for a collection instance's copy those the Empty
   * instancing it is in (its Set Collection Member changes included); then the object's own Set
   * Collection Member changes apply in order: putting it in a
   * collection, or taking it out of one and its child collections. Python edits to Blender
   * collections while playing are not followed. A collection the scene doesn't link holds what
   * the file links to it (by the same sources), and Set Collection Member can't change it. */

  /** The collections of this scene `object` is directly in now (cached on the object). */
  const std::vector<const blender::Collection *> &CollectionsOf(KX_GameObject *object);
  /** `object` is in `collection`, or with `include_children` in one of its child collections. */
  bool IsInCollection(KX_GameObject *object,
                      const blender::Collection *collection,
                      bool include_children);
  /** The scene's objects in `collection` (IsInCollection): first those linked to it in the file
   * in its order, then the others (copies, objects put in while playing) in object list order.
   * Cached until objects are added or removed or memberships change. */
  const std::vector<KX_GameObject *> &CollectionMembers(const blender::Collection *collection,
                                                        bool include_children);
  /** Set Collection Member: puts `object` in `collection`, or takes it out of it and its child
   * collections. False when the collection is not part of this scene. */
  bool SetCollectionMember(KX_GameObject *object, blender::Collection *collection, bool member);
  /** Load Game: replaces all of an object's Set Collection Member changes. */
  void RestoreCollectionMemberships(
      KX_GameObject *object,
      std::vector<std::pair<const blender::Collection *, bool>> memberships);
  /** `object`'s collections changed (its changes, or its instancing Empty left): it and the
   * copies of the collection it instances, nested ones too, find theirs again, and are drawn or
   * hidden by them. */
  void NoteObjectCollectionsChanged(KX_GameObject *object);
  /** The collection is this scene's master collection or one below it. */
  bool CollectionInScene(const blender::Collection *collection);
  /** Load Game: the collection below this scene's master collection with this name and library
   * file path (empty for the game's own file), or null. */
  const blender::Collection *FindSceneCollection(const std::string &name,
                                                 const std::string &library_path);
  /** The scene's collections or their objects changed (a library merged or freed, objects
   * converted while playing): the collection index is built again, and Set Collection Member
   * changes for collections that left the scene are dropped. */
  void NoteCollectionsChanged();
  const std::vector<KX_GameObject *> &GetDupliListVector() const
  {
    return m_upbgeDupliInstancesList;
  }
  void AddUpbgeDupliInstanceToList(KX_GameObject *gameobj);
  void RemoveUpbgeDupliInstanceFromList(KX_GameObject *gameobj);
  /** Takes a gone Dupli copy's draw identity (allocated with new) and frees it a few frames
   * later. */
  void RetireDupliDrawOriginal(blender::Object *draw_original);
  void FreeRetiredDupliDrawOriginals(uint64_t frame);
  /**
   * The copies (Dupli Base copies, Auto objects) this camera pass draws, sorted into groups that
   * Blender's grouped instance drawing syncs once each (DRW_game_render_loop's instance groups):
   * one per drawn object, color, facing, Keep Shadow and "moved now". Built after the camera's
   * hiding; until EndDupliDrawGroups, the copies' own draw entries (bge_dupli_provider) leave the
   * grouped ones out. Copies a group cannot draw keep their own entries.
   */
  void BeginDupliDrawGroups(blender::Depsgraph *depsgraph, bool is_overlay_pass, bool workbench);
  void EndDupliDrawGroups();
  /** The groups BeginDupliDrawGroups built (empty outside a grouped pass). */
  KX_DupliDrawGroups &GetDupliDrawGroups();
  bool IsDrawnInDupliGroup(const KX_GameObject *object) const;
  /***************End of EEVEE INTEGRATION**********************/

  RAS_BucketManager *GetBucketManager() const;
  RAS_MaterialBucket *FindBucket(RAS_IPolyMaterial *polymat, bool &bucketCreated);

  /**
   * Update all transforms according to the scenegraph.
   */
  static bool KX_ScenegraphUpdateFunc(SG_Node *node, void *gameobj, void *scene);
  static bool KX_ScenegraphRescheduleFunc(SG_Node *node, void *gameobj, void *scene);
  void UpdateParents(double curtime);

  /* Warning: This is different from upbge dupli instances
   * -> it is related to Groups (in old Blender version)
   *    or Instance Collections (in newer Blender versions) */
  void DupliGroupRecurse(KX_GameObject *groupobj, int level);
  bool IsObjectInGroup(KX_GameObject *gameobj)
  {
    return (m_groupGameObjects.empty() ||
            m_groupGameObjects.find(gameobj) != m_groupGameObjects.end());
  }
  void AddObjectDebugProperties(KX_GameObject *gameobj);
  KX_GameObject *AddReplicaObject(KX_GameObject *gameobj,
                                  KX_GameObject *locationobj,
                                  float lifespan = 0.0f,
                                  std::vector<KX_GameObject *> *created_members = nullptr,
                                  const std::set<KX_GameObject *> *replica_filter = nullptr,
                                  std::unordered_map<KX_GameObject *, KX_GameObject *> *
                                      created_mapping = nullptr);
  KX_GameObject *AddNodeReplicaObject(SG_Node *node, KX_GameObject *gameobj);
  void RemoveNodeDestructObject(SG_Node *node, KX_GameObject *gameobj);
  void RemoveObject(KX_GameObject *gameobj,
                    KX_ObjectRemovalReason reason = KX_ObjectRemovalReason::SystemCleanup);
  void RemoveDupliGroup(KX_GameObject *gameobj, KX_ObjectRemovalReason reason);
  void DelayedRemoveObject(KX_GameObject *gameobj,
                           KX_ObjectRemovalReason reason = KX_ObjectRemovalReason::Gameplay);

  bool NewRemoveObject(KX_GameObject *gameobj, KX_ObjectRemovalReason reason);
  void ReplaceMesh(KX_GameObject *gameobj, RAS_MeshObject *mesh, bool use_gfx, bool use_phys);
  /** A drawable copy of \a mesh, made once per scene, for meshes of objects that are never
   * drawn (Disable in Renders, hidden collections). */
  blender::Mesh *GetReplaceMeshEvalMesh(RAS_MeshObject *mesh);

  void AddAnimatedObject(KX_GameObject *gameobj);

  /**
   * \section Logic stuff
   * Initiate an update of the logic system.
   */
  void LogicBeginFrame(double curtime, double framestep);
#ifdef WITH_GAMEENGINE_LOGICNODES
  /** Capture input for fixed simulation on scheduler iterations without a rendered frame. */
  void LogicNodeCaptureInput();
  /** Commit asynchronous Logic Nodes services before activity culling and simulation. */
  void LogicNodeBeginFrame();
  /** Run the render-frame Logic Nodes phase, including while simulation time is stopped. */
  void LogicNodeUpdateFrame(double curtime,
                            double scaledDeltaTime,
                            double unscaledDeltaTime,
                            bool useFixedPhysicsTimestep,
                            bool gamePaused);
  void UpdatePausedAnimations(double pauseTime, double normalTime);
  void RebasePausedAnimations(double pauseTime, double normalTime);
#endif
  void LogicUpdateFrame(double curtime,
                        bool useFixedPhysicsTimestep,
                        double fixedDt,
                        double unscaledDt);
  void UpdateAnimations(double curtime);
  /** Blender's depsgraph for the actions of the animation update running now (looked up once for
   * all objects), or null outside it. */
  blender::Depsgraph *GetAnimationDepsgraph();
  /** Animation updates so far (played and paused ones). */
  uint64_t GetAnimationUpdateCount() const
  {
    return m_animationUpdateCount;
  }

  void LogicEndFrame();
#ifdef WITH_GAMEENGINE_LOGICNODES
  /** Finalize removals requested by the render-frame Logic Nodes phase. */
  void LogicNodeEndFrame();
#endif

  /**
   * Smooth Motion: objects are drawn between their fixed steps by their own choice (Object >
   * Physics), Scene Default being the parent's choice, or `scene_interpolates` (the scene's
   * Physics Interpolation) for an object without a parent.
   * Before each fixed step: the poses the step starts from, for the smoothed objects only.
   */
  void StorePhysicsInterpolationState(bool scene_interpolates);
  /** A drawn frame: the smoothed objects' poses `interpolation` between their last two steps
   * (or that far ahead), their children with them; `frame_seconds` of game time since the frame
   * before (blend fades). Undone by ClearPhysicsInterpolationState. */
  void ApplyPhysicsInterpolation(double interpolation,
                                 bool scene_interpolates,
                                 double frame_seconds);
  void ClearPhysicsInterpolationState();
  /** Whether an object here chose Interpolate or Extrapolate itself. */
  bool HasSmoothMotion() const
  {
    return m_hasSmoothMotion;
  }
  void NoteSmoothMotion()
  {
    m_hasSmoothMotion = true;
  }
  /** Discard transform changes accumulated while the simulation was paused. */
  void RebaseKinematicPhysics();
  /** While per-render-frame logic (On Update) runs: a transform it changes moves the step
   * history along (SG_Node::SetHistoryFollowsUpdates), so the change shows at once and the steps'
   * motion stays smooth. */
  void BeginPhysicsInterpolationFrameUpdate();
  void EndPhysicsInterpolationFrameUpdate();
  /** Track objects that enter render interpolation so we can clear them efficiently. */
  void RegisterInterpolatedObject(KX_GameObject *gameobj);
  /** Remove an object from all interpolation tracking lists before destruction. */
  void UnregisterInterpolatedObject(KX_GameObject *gameobj);
  /** Toggle skipping interpolation for invisible objects (e.g. frustum culled). */
  void SetSkipInvisibleInterpolation(bool skip);
  /** Returns whether invisible objects are excluded from interpolation updates. */
  bool GetSkipInvisibleInterpolation() const;

  EXP_ListValue<KX_GameObject> *GetObjectList() const;
  /** Changes whenever objects join or leave the active object list, so systems that index scene
   * objects (such as the UI element registry) rescan only when something changed. */
  uint64_t GetObjectListGeneration() const
  {
    return m_objectListGeneration;
  }
  void NoteObjectListChanged()
  {
    ++m_objectListGeneration;
  }
  EXP_ListValue<KX_GameObject> *GetInactiveList() const;
  EXP_ListValue<KX_GameObject> *GetRootParentList() const;
  EXP_ListValue<KX_LightObject> *GetLightList() const;

  SCA_LogicManager *GetLogicManager() const;

  SCA_TimeEventManager *GetTimeEventManager() const;

  KX_PythonProxyManager &GetPythonProxyManager();

#ifdef WITH_GAMEENGINE_LOGICNODES
  LN_Manager *GetLogicNodeManager() const;
#endif

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

  KX_PlayerViews &GetPlayerViews()
  {
    return m_playerViews;
  }

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

  /** Update the mesh for objects based on level of detail settings, once a frame (at the first
   * world camera pass \a cam after SetLodUpdateDue, else nothing): each object takes the detail
   * that the nearest of the frame's views needs. */
  void UpdateObjectLods(KX_Camera *cam);
  /** The engine, before drawing the scene's cameras of a frame. */
  void SetLodUpdateDue()
  {
    m_lodUpdateDue = true;
  }

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
  blender::Scene *GetBlenderScene()
  {
    return m_blenderScene;
  }
  const blender::Scene *GetBlenderScene() const
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
