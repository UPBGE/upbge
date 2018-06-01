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

#ifndef __KX_SCENE_H__
#define __KX_SCENE_H__


#include "KX_PhysicsEngineEnums.h"
#include "KX_TextureRendererManager.h" // For KX_TextureRendererManager::RendererCategory.
#include "KX_PythonComponentManager.h"
#include "KX_KetsjiEngine.h" // For KX_DebugOption.

#include "SG_Scene.h"
#include "SG_Frustum.h"
#include "SCA_IScene.h"

#include "RAS_Rasterizer.h" // For RAS_Rasterizer::DrawType.
#include "RAS_DebugDraw.h"
#include "RAS_FramingManager.h"
#include "RAS_Rect.h"

#include "EXP_PyObjectPlus.h"
#include "EXP_Value.h"

#include <set>

template <class T>
class EXP_ListValue;
class EXP_Value;
class SCA_LogicManager;
class SCA_KeyboardManager;
class SCA_TimeEventManager;
class SCA_MouseManager;
class SCA_IInputDevice;
class SCA_JoystickManager;
class KX_NetworkMessageScene;
class KX_NetworkMessageManager;
class KX_2DFilterManager;
class KX_ObstacleSimulation;
class KX_WorldInfo;
class KX_Camera;
class KX_FontObject;
class KX_GameObject;
class KX_LightObject;
struct KX_ClientObjectInfo;
class BL_SceneConverter;
class SG_Node;
class PHY_IPhysicsEnvironment;
class RAS_Mesh;
class RAS_BoundingBoxManager;
class RAS_BucketManager;
class RAS_MaterialBucket;
class RAS_IMaterial;
class RAS_Rasterizer;
class RAS_OffScreen;
class RAS_2DFilterManager;

struct Scene;
struct TaskPool;

class KX_Scene : public EXP_Value, public SCA_IScene, public SG_Scene
{
public:
	enum DrawingCallbackType {
		PRE_DRAW = 0,
		POST_DRAW,
		PRE_DRAW_SETUP,
		MAX_DRAW_CALLBACK
	};

	struct AnimationPoolData
	{
		double curtime;
	};

	static SG_Callbacks m_callbacks;

private:
	Py_Header

#ifdef WITH_PYTHON
	PyObject *m_attrDict;
	PyObject *m_removeCallbacks;
	PyObject *m_drawCallbacks[MAX_DRAW_CALLBACK];
#endif

	struct CullingInfo
	{
		int m_layer;
		std::vector<KX_GameObject *>& m_objects;

		CullingInfo(int layer, std::vector<KX_GameObject *>& objects)
			:m_layer(layer),
			m_objects(objects)
		{
		}
	};

	KX_TextureRendererManager *m_rendererManager;
	RAS_BucketManager *m_bucketmanager;

	/// Manager used to update all the mesh bounding box.
	RAS_BoundingBoxManager *m_boundingBoxManager;

	std::vector<KX_GameObject *> m_tempObjectList;

	/**
	 * The list of objects which have been removed during the
	 * course of one frame. They are actually destroyed in
	 * LogicEndFrame() via a call to RemoveObject().
	 */
	std::vector<KX_GameObject *> m_euthanasyobjects;

	EXP_ListValue<KX_GameObject> *m_objectlist;
	EXP_ListValue<KX_LightObject> *m_lightlist;
	/// All objects that are not in the active layer.
	EXP_ListValue<KX_GameObject> *m_inactivelist;
	/// All animated objects, no need of EXP_ListValue because the list isn't exposed in python.
	std::vector<KX_GameObject *> m_animatedlist;

	/// The list of cameras for this scene.
	EXP_ListValue<KX_Camera> *m_cameralist;
	/// The list of fonts for this scene.
	EXP_ListValue<KX_FontObject> *m_fontlist;

	/// Various SCA managers used by the scene
	SCA_LogicManager *m_logicmgr;
	SCA_KeyboardManager *m_keyboardmgr;
	SCA_MouseManager *m_mousemgr;
	SCA_TimeEventManager *m_timemgr;

	KX_PythonComponentManager m_componentManager;

	/// Physics engine abstraction.
	PHY_IPhysicsEnvironment *m_physicsEnvironment;

	/// The name of the scene.
	std::string m_sceneName;

	/// Stores the world-settings for a scene.
	KX_WorldInfo *m_worldinfo;

	/// Network scene.
	KX_NetworkMessageScene *m_networkScene;

	/// The active camera for the scene.
	KX_Camera *m_activeCamera;
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

	/// The execution priority of replicated object actuators.
	int m_ueberExecutionPriority;

	/**
	 * Activity 'bubble' settings :
	 * Suspend (freeze) the entire scene.
	 */
	bool m_suspend;
	double m_suspendedDelta;

	/// Toggle to enable or disable object activity culling.
	bool m_activityCulling;

	/// Toggle to enable or disable culling via DBVT broadphase of Bullet.
	bool m_dbvtCulling;

	/// Occlusion culling resolution.
	int m_dbvtOcclusionRes;

	/// The framing settings used by this scene
	RAS_FrameSettings m_frameSettings;

	/**
	 * This scenes viewport into the game engine
	 * canvas.Maintained externally, initially [0,0] -> [0,0]
	 */
	RAS_Rect m_viewport;

	/// Debug drawing registering.
	RAS_DebugDraw m_debugDraw;

	/// Visibility testing functions.
	static void PhysicsCullingCallback(KX_ClientObjectInfo *objectInfo, void *cullingInfo);

	Scene *m_blenderScene;

	KX_2DFilterManager *m_filterManager;

	KX_ObstacleSimulation *m_obstacleSimulation;

	AnimationPoolData m_animationPoolData;
	TaskPool *m_animationPool;
	double m_previousAnimTime;

	/// LOD Hysteresis settings.
	bool m_isActivedHysteresis;
	int m_lodHysteresisValue;

	void RemoveNodeDestructObject(KX_GameObject *gameobj);
	void RemoveObject(KX_GameObject *gameobj);
	void RemoveDupliGroup(KX_GameObject *gameobj);
	bool NewRemoveObject(KX_GameObject *gameobj);

public:
	KX_Scene(SCA_IInputDevice *inputDevice,
	         const std::string& scenename,
	         Scene *scene,
			 KX_NetworkMessageManager *messageManager);
	virtual ~KX_Scene();

	RAS_BucketManager *GetBucketManager() const;
	KX_TextureRendererManager *GetTextureRendererManager() const;
	RAS_BoundingBoxManager *GetBoundingBoxManager() const;
	void RenderBuckets(const std::vector<KX_GameObject *>& objects, RAS_Rasterizer::DrawType drawingMode,
			const mt::mat3x4& cameratransform, unsigned short viewportIndex,
			RAS_Rasterizer *rasty, RAS_OffScreen *offScreen);

	/// Update lights settings.
	void UpdateLights(RAS_Rasterizer *rasty);
	/// Return list of shadow schedulers.
	std::vector<KX_TextureRenderSchedule> ScheduleShadowsRender();
	/// Return list of texture renderer schedules.
	std::vector<KX_TextureRenderSchedule> ScheduleTexturesRender(RAS_Rasterizer *rasty, const KX_SceneRenderSchedule& sceneData);

	virtual SG_Object *ReplicateNodeObject(SG_Node *node, SG_Object *origObject);
	virtual void DestructNodeObject(SG_Node *node, SG_Object *object);

	void DupliGroupRecurse(KX_GameObject *groupobj, int level);
	bool IsObjectInGroup(KX_GameObject *gameobj) const;
	void AddObjectDebugProperties(KX_GameObject *gameobj);
	KX_GameObject *AddReplicaObject(KX_GameObject *gameobj, KX_GameObject *locationobj, float lifespan = 0.0f);
	KX_GameObject *AddNodeReplicaObject(SG_Node *node, KX_GameObject *gameobj);

	/// Add an object to remove.
	void DelayedRemoveObject(KX_GameObject *gameobj);
	/// Effectivly remove object added with DelayedRemoveObject
	void RemoveEuthanasyObjects();

	void AddAnimatedObject(KX_GameObject *gameobj);

	/**
	 * \section Logic stuff
	 * Initiate an update of the logic system.
	 */
	void LogicBeginFrame(double curtime, double framestep);
	void LogicUpdateFrame(double curtime);
	void UpdateAnimations(double curtime, bool restrict);

	void LogicEndFrame();

	EXP_ListValue<KX_GameObject> *GetObjectList() const;
	EXP_ListValue<KX_GameObject> *GetInactiveList() const;
	EXP_ListValue<KX_LightObject> *GetLightList() const;
	EXP_ListValue<KX_Camera> *GetCameraList() const;
	EXP_ListValue<KX_FontObject> *GetFontList() const;

	SCA_LogicManager *GetLogicManager() const;
	SCA_TimeEventManager *GetTimeEventManager() const;
	KX_PythonComponentManager& GetPythonComponentManager();

	/// Return the currently active camera.
	KX_Camera *GetActiveCamera();

	/**
	 * Set this camera to be the active camera in the scene. If the
	 * camera is not present in the camera list, it will be added
	 */
	void SetActiveCamera(KX_Camera *camera);

	KX_Camera *GetOverrideCullingCamera() const;
	void SetOverrideCullingCamera(KX_Camera *cam);

	/**
	 * Move this camera to the end of the list so that it is rendered last.
	 * If the camera is not on the list, it will be added
	 */
	void SetCameraOnTop(KX_Camera *camera);

	/// Set the framing options for this scene.
	void SetFramingType(const RAS_FrameSettings& frameSettings);

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
	void SetNetworkMessageScene(KX_NetworkMessageScene *netScene);
	KX_NetworkMessageScene *GetNetworkMessageScene() const;

	void SetWorldInfo(KX_WorldInfo *wi);
	KX_WorldInfo *GetWorldInfo() const;

	std::vector<KX_GameObject *> CalculateVisibleMeshes(KX_Camera *cam, RAS_Rasterizer::StereoEye eye, int layer);
	std::vector<KX_GameObject *> CalculateVisibleMeshes(bool frustumCulling, const SG_Frustum& frustum, int layer);
	std::vector<KX_GameObject *> CalculateVisibleMeshes(const SG_Frustum& frustum, int layer);

	RAS_DebugDraw& GetDebugDraw();
	/// \section Debug draw.
	void DrawDebug(const std::vector<KX_GameObject *>& objects,
			KX_DebugOption showBoundingBox, KX_DebugOption showArmatures);
	void RenderDebugProperties(RAS_DebugDraw& debugDraw, int xindent, int ysize, int& xcoord, int& ycoord, unsigned short propsMax);
	void FlushDebugDraw(RAS_Rasterizer *rasty, RAS_ICanvas *canvas);

	/// Replicate the logic bricks associated to this object.
	void ReplicateLogic(KX_GameObject *newobj);

	// Suspend the entire scene.
	void Suspend();

	// Resume a suspended scene.
	void Resume();

	/// Update the mesh for objects based on level of detail settings
	void UpdateObjectLods(KX_Camera *cam, const std::vector<KX_GameObject *>& objects);
	void UpdateObjectLods(const mt::vec3& camPos, float lodFactor, const std::vector<KX_GameObject *>& objects);

	// LoD Hysteresis functions
	void SetLodHysteresis(bool active);
	bool IsActivedLodHysteresis() const;
	void SetLodHysteresisValue(int hysteresisvalue);
	int GetLodHysteresisValue() const;

	/// Update the activity culling of objects in this scene, if needed.
	void UpdateObjectActivity();
	/// Enable/disable activity culling.
	void SetActivityCulling(bool b);

	bool IsSuspended() const;

	/// Use of DBVT tree for camera culling
	void SetDbvtCulling(bool b);
	bool GetDbvtCulling() const;
	void SetDbvtOcclusionRes(int i);
	int GetDbvtOcclusionRes() const;

	void SetSceneConverter(BL_SceneConverter *sceneConverter);

	PHY_IPhysicsEnvironment *GetPhysicsEnvironment() const;
	void SetPhysicsEnvironment(PHY_IPhysicsEnvironment *physEnv);

	void SetGravity(const mt::vec3& gravity);
	mt::vec3 GetGravity() const;

	/**
	 * Sets the difference between the local time of the scene (when it
	 * was running and not suspended) and the "curtime"
	 */
	void SetSuspendedDelta(double suspendeddelta);
	/**
	 * Returns the difference between the local time of the scene (when it
	 * was running and not suspended) and the "curtime"
	 */
	double GetSuspendedDelta() const;

	/// Returns the Blender scene this was made from.
	Scene *GetBlenderScene() const;

	bool MergeScene(KX_Scene *other);

	/// 2D Filters.
	KX_2DFilterManager *Get2DFilterManager() const;
	RAS_OffScreen *Render2DFilters(RAS_Rasterizer *rasty, RAS_ICanvas *canvas, RAS_OffScreen *inputofs, RAS_OffScreen *targetofs);

	KX_ObstacleSimulation *GetObstacleSimulation();
	void SetObstacleSimulation(KX_ObstacleSimulation *obstacleSimulation);

	virtual std::string GetName();
	virtual void SetName(const std::string& name);

#ifdef WITH_PYTHON

	EXP_PYMETHOD_DOC(KX_Scene, addObject);
	EXP_PYMETHOD_DOC(KX_Scene, end);
	EXP_PYMETHOD_DOC(KX_Scene, restart);
	EXP_PYMETHOD_DOC(KX_Scene, replace);
	EXP_PYMETHOD_DOC(KX_Scene, suspend);
	EXP_PYMETHOD_DOC(KX_Scene, resume);
	EXP_PYMETHOD_DOC(KX_Scene, get);
	EXP_PYMETHOD_DOC(KX_Scene, drawObstacleSimulation);

	// Attributes.
	static PyObject *pyattr_get_name(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_objects(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_objects_inactive(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_lights(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_texts(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_cameras(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_filter_manager(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_world(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_active_camera(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_active_camera(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_overrideCullingCamera(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_overrideCullingCamera(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_drawing_callback(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_drawing_callback(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_remove_callback(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_remove_callback(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_gravity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_gravity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);

	// getitem/setitem
	static PyMappingMethods Mapping;
	static PySequenceMethods Sequence;

	/// Run the registered python drawing functions.
	void RunDrawingCallbacks(DrawingCallbackType callbackType, KX_Camera *camera);

	// Run the registered python callbacks when the scene is removed.
	void RunOnRemoveCallbacks();
#endif
};

#ifdef WITH_PYTHON
bool ConvertPythonToScene(PyObject *value, KX_Scene **scene, bool py_none_ok, const char *error_prefix);
#endif

#endif  // __KX_SCENE_H__
