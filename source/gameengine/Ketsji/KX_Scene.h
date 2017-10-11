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

#include "BL_ResourceCollection.h"

#include "SG_Node.h"
#include "SG_Frustum.h"

#include "RAS_Rasterizer.h" // For RAS_Rasterizer::DrawType.
#include "RAS_DebugDraw.h"
#include "RAS_FramingManager.h"
#include "RAS_Rect.h"

#include "EXP_ListValue.h"

#include <set>

class EXP_Value;
class SCA_IInputDevice;
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

class KX_Scene : public EXP_Value
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
	Py_Header(KX_Scene)

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

	struct DebugProp
	{
		KX_GameObject *m_obj;
		std::string m_name;
	};

#ifdef WITH_PYTHON
	PyObject *m_attrDict;
	PyObject *m_removeCallbacks;
	PyObject *m_drawCallbacks[MAX_DRAW_CALLBACK];
#endif

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

	EXP_ListValue<KX_GameObject> m_objectlist;
	/// All 'root' parents.
	EXP_ListValue<KX_GameObject> m_parentlist;
	EXP_ListValue<KX_LightObject> m_lightlist;
	/// All objects that are not in the active layer.
	EXP_ListValue<KX_GameObject> m_inactivelist;
	/// All animated objects, no need of EXP_ListValue because the list isn't exposed in python.
	std::vector<KX_GameObject *> m_animatedlist;

	/// The list of cameras for this scene.
	EXP_ListValue<KX_Camera> m_cameralist;
	/// The list of fonts for this scene.
	EXP_ListValue<KX_FontObject> m_fontlist;

	std::vector<DebugProp> m_debugList;

	/**
	 * List of nodes that needs scenegraph update
	 * the Dlist is not object that must be updated
	 * the Qlist is for objects that needs to be rescheduled
	 * for updates after udpate is over (slow parent, bone parent).
	 */
	SG_QList m_sghead;

	KX_PythonComponentManager m_componentManager;

	/// Physics engine abstraction.
	PHY_IPhysicsEnvironment *m_physicsEnvironment;

	/// The name of the scene.
	std::string m_name;

	/// Stores the world-settings for a scene.
	std::unique_ptr<KX_WorldInfo> m_worldinfo;

	/// Network scene.
	KX_NetworkMessageScene *m_networkScene;

	/// The active camera for the scene.
	KX_Camera *m_activeCamera;
	/// The active camera for scene culling.
	KX_Camera *m_overrideCullingCamera;

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

	BL_ResourceCollection m_resssources;

public:
	KX_Scene(SCA_IInputDevice *inputDevice,
	         const std::string& scenename,
	         Scene *scene,
			 RAS_ICanvas *canvas,
			 KX_NetworkMessageManager *messageManager);
	virtual ~KX_Scene();

	BL_ResourceCollection& GetResources();
	void SetResources(BL_ResourceCollection&& ressources);

	RAS_BucketManager *GetBucketManager() const;
	KX_TextureRendererManager *GetTextureRendererManager() const;
	RAS_BoundingBoxManager *GetBoundingBoxManager() const;
	void RenderBuckets(const std::vector<KX_GameObject *>& objects, RAS_Rasterizer::DrawType drawingMode,
	                   const mt::mat3x4& cameratransform, RAS_Rasterizer *rasty, RAS_OffScreen *offScreen);
	void RenderTextureRenderers(KX_TextureRendererManager::RendererCategory category, RAS_Rasterizer *rasty, RAS_OffScreen *offScreen,
	                            KX_Camera *sceneCamera, const RAS_Rect& viewport, const RAS_Rect& area);

	/// Update all transforms according to the scenegraph.
	static bool KX_ScenegraphUpdateFunc(SG_Node *node, void *gameobj, void *scene);
	static bool KX_ScenegraphRescheduleFunc(SG_Node *node, void *gameobj, void *scene);
	/// SceneGraph transformation update.
	void UpdateParents();

	void DupliGroupRecurse(KX_GameObject *groupobj, int level);
	bool IsObjectInGroup(KX_GameObject *gameobj) const;
	void AddObjectDebugProperties(KX_GameObject *gameobj);
	KX_GameObject *AddReplicaObject(KX_GameObject *gameobj, KX_GameObject *locationobj, float lifespan = 0.0f);
	KX_GameObject *AddNodeReplicaObject(SG_Node *node, KX_GameObject *gameobj);

	void RemoveNodeDestructObject(KX_GameObject *gameobj);
	void RemoveObject(KX_GameObject *gameobj);
	void RemoveDupliGroup(KX_GameObject *gameobj);
	void DelayedRemoveObject(KX_GameObject *gameobj);
	void NewRemoveObject(KX_GameObject *gameobj);

	void AddAnimatedObject(KX_GameObject *gameobj);

	bool PropertyInDebugList(KX_GameObject *gameobj, const std::string &name);
	bool ObjectInDebugList(KX_GameObject *gameobj);
	void RemoveAllDebugProperties();
	void AddDebugProperty(KX_GameObject *gameobj, const std::string &name);
	void RemoveDebugProperty(KX_GameObject *gameobj, const std::string &name);
	void RemoveObjectDebugProperties(KX_GameObject *gameobj);

	/**
	 * \section Logic stuff
	 * Initiate an update of the logic system.
	 */
	void LogicBeginFrame(double curtime, double framestep);
	void LogicUpdateFrame(double curtime);
	void UpdateAnimations(double curtime, bool restrict);

	void LogicEndFrame();

	EXP_ListValue<KX_GameObject>& GetObjectList();
	EXP_ListValue<KX_GameObject>& GetInactiveList();
	EXP_ListValue<KX_GameObject>& GetRootParentList();
	EXP_ListValue<KX_LightObject>& GetLightList();
	EXP_ListValue<KX_Camera>& GetCameraList();
	EXP_ListValue<KX_FontObject>& GetFontList();

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

	std::vector<KX_GameObject *> CalculateVisibleMeshes(KX_Camera *cam, int layer);
	std::vector<KX_GameObject *> CalculateVisibleMeshes(const SG_Frustum& frustum, int layer);

	RAS_DebugDraw& GetDebugDraw();
	/// \section Debug draw.
	void DrawDebug(const std::vector<KX_GameObject *>& objects,
			KX_DebugOption showBoundingBox, KX_DebugOption showArmatures);
	void RenderDebugProperties(RAS_DebugDraw& debugDraw, int xindent, int ysize, int& xcoord, int& ycoord, unsigned short propsMax);
	void FlushDebugDraw(RAS_Rasterizer *rasty, RAS_ICanvas *canvas);

	// Suspend the entire scene.
	void Suspend();

	// Resume a suspended scene.
	void Resume();

	/// Update the mesh for objects based on level of detail settings
	void UpdateObjectLods(KX_Camera *cam, const std::vector<KX_GameObject *>& objects);

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

	bool Merge(KX_Scene *other);

	void RemoveTagged();

	/// 2D Filters.
	KX_2DFilterManager *Get2DFilterManager() const;
	RAS_OffScreen *Render2DFilters(RAS_Rasterizer *rasty, RAS_ICanvas *canvas, RAS_OffScreen *inputofs, RAS_OffScreen *targetofs);

	KX_ObstacleSimulation *GetObstacleSimulation();
	void SetObstacleSimulation(KX_ObstacleSimulation *obstacleSimulation);

	virtual std::string GetName() const;
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
	KX_Camera *pyattr_get_active_camera();
	bool pyattr_set_active_camera(PyObject *value);
	KX_Camera *pyattr_get_overrideCullingCamera();
	bool pyattr_set_overrideCullingCamera(PyObject *value);
	PyObject *pyattr_get_drawing_callback(const EXP_Attribute *attrdef);
	bool pyattr_set_drawing_callback(PyObject *value, const EXP_Attribute *attrdef);
	PyObject *pyattr_get_remove_callback();
	bool pyattr_set_remove_callback(PyObject *value);
	mt::vec3 pyattr_get_gravity();
	void pyattr_set_gravity(const mt::vec3& value);

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
