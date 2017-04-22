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
#include "KX_CullingNode.h" // For KX_CullingNodeList.
#include "KX_PythonComponentManager.h"

#include <vector>
#include <set>
#include <list>

#include "SG_Node.h"
#include "SG_Frustum.h"
#include "SCA_IScene.h"
#include "MT_Transform.h"

#include "RAS_Rasterizer.h" // For RAS_Rasterizer::DrawType.
#include "RAS_FramingManager.h"
#include "RAS_Rect.h"

#include "EXP_PyObjectPlus.h"
#include "EXP_Value.h"

/**
 * \section Forward declarations
 */
struct SM_MaterialProps;
struct SM_ShapeProps;
struct Scene;

template <class T>
class EXP_ListValue;

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
class KX_WorldInfo;
class KX_Camera;
class KX_FontObject;
class KX_GameObject;
class KX_LightObject;
class RAS_MeshObject;
class RAS_BoundingBoxManager;
class RAS_BucketManager;
class RAS_MaterialBucket;
class RAS_IPolyMaterial;
class RAS_Rasterizer;
class RAS_DebugDraw;
class RAS_OffScreen;
class RAS_2DFilterManager;
class KX_2DFilterManager;
class SCA_JoystickManager;
class btCollisionShape;
class BL_BlenderSceneConverter;
struct KX_ClientObjectInfo;
class KX_ObstacleSimulation;
struct TaskPool;

/* for ID freeing */
#define IS_TAGGED(_id) ((_id) && (((ID *)_id)->tag & LIB_TAG_DOIT))

/**
 * The KX_Scene holds all data for an independent scene. It relates
 * KX_Objects to the specific objects in the modules.
 * */
class KX_Scene : public EXP_Value, public SCA_IScene
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

private:
	Py_Header

#ifdef WITH_PYTHON
	PyObject*	m_attr_dict;
	PyObject*	m_drawCallbacks[MAX_DRAW_CALLBACK];
#endif

	struct CullingInfo {
		int m_layer;
		KX_CullingNodeList& m_nodes;

		CullingInfo(int layer, KX_CullingNodeList& nodes)
			:m_layer(layer),
			m_nodes(nodes)
		{
		}
	};

protected:
	KX_TextureRendererManager *m_rendererManager;
	RAS_BucketManager*	m_bucketmanager;

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
	EXP_ListValue<KX_GameObject> *m_parentlist; // all 'root' parents
	EXP_ListValue<KX_LightObject> *m_lightlist;
	EXP_ListValue<KX_GameObject> *m_inactivelist;	// all objects that are not in the active layer
	/// All animated objects, no need of EXP_ListValue because the list isn't exposed in python.
	std::vector<KX_GameObject *> m_animatedlist;

	/// The set of cameras for this scene
	EXP_ListValue<KX_Camera> *m_cameralist;
	/// The set of fonts for this scene
	EXP_ListValue<KX_FontObject> *m_fontlist;
	
	SG_QList			m_sghead;		// list of nodes that needs scenegraph update
										// the Dlist is not object that must be updated
										// the Qlist is for objects that needs to be rescheduled
										// for updates after udpate is over (slow parent, bone parent)

	/**
	 * Various SCA managers used by the scene
	 */
	SCA_LogicManager*		m_logicmgr;
	SCA_KeyboardManager*	m_keyboardmgr;
	SCA_MouseManager*		m_mousemgr;
	SCA_TimeEventManager*	m_timemgr;

	KX_PythonComponentManager m_componentManager;

	/**
	 * physics engine abstraction
	 */
	//e_PhysicsEngine m_physicsEngine; //who needs this ?
	class PHY_IPhysicsEnvironment*		m_physicsEnvironment;

	/**
	 * The name of the scene
	 */
	std::string	m_sceneName;
	
	/**
	 * stores the world-settings for a scene
	 */
	KX_WorldInfo* m_worldinfo;

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
	SG_Node* m_rootnode;

	/**
	 * The active camera for the scene
	 */
	KX_Camera* m_active_camera;
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
	std::vector<KX_GameObject*>	m_logicHierarchicalGameObjects;
	
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

	SCA_ISystem* m_kxsystem;

	/**
	 * The execution priority of replicated object actuators?
	 */
	int	m_ueberExecutionPriority;

	/**
	 * Activity 'bubble' settings :
	 * Suspend (freeze) the entire scene.
	 */
	bool m_suspend;
	double m_suspendeddelta;

	/// Toggle to enable or disable object activity culling.
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
	static void PhysicsCullingCallback(KX_ClientObjectInfo* objectInfo, void* cullingInfo);

	struct Scene* m_blenderScene;

	KX_2DFilterManager *m_filterManager;

	KX_ObstacleSimulation* m_obstacleSimulation;

	AnimationPoolData m_animationPoolData;
	TaskPool *m_animationPool;

	/**
	 * LOD Hysteresis settings
	 */
	bool m_isActivedHysteresis;
	int m_lodHysteresisValue;

public:
	KX_Scene(SCA_IInputDevice *inputDevice,
		const std::string& scenename,
		struct Scene* scene,
		class RAS_ICanvas* canvas,
		KX_NetworkMessageManager *messageManager);

	virtual
	~KX_Scene();

	RAS_BucketManager* GetBucketManager() const;
	KX_TextureRendererManager *GetTextureRendererManager() const;
	RAS_BoundingBoxManager *GetBoundingBoxManager() const;
	RAS_MaterialBucket*	FindBucket(RAS_IPolyMaterial* polymat, bool &bucketCreated);
	void RenderBuckets(const KX_CullingNodeList& nodes, RAS_Rasterizer::DrawType drawingMode, const MT_Transform& cameratransform,
			RAS_Rasterizer *rasty, RAS_OffScreen *offScreen);
	void RenderTextureRenderers(KX_TextureRendererManager::RendererCategory category, RAS_Rasterizer *rasty, RAS_OffScreen *offScreen,
			KX_Camera *sceneCamera, const RAS_Rect& viewport, const RAS_Rect& area);

	/**
	 * Update all transforms according to the scenegraph.
	 */
	static bool KX_ScenegraphUpdateFunc(SG_Node* node,void* gameobj,void* scene);
	static bool KX_ScenegraphRescheduleFunc(SG_Node* node,void* gameobj,void* scene);
	void UpdateParents(double curtime);
	void DupliGroupRecurse(KX_GameObject *groupobj, int level);
	bool IsObjectInGroup(KX_GameObject* gameobj)
	{ 
		return (m_groupGameObjects.empty() || 
				m_groupGameObjects.find(gameobj) != m_groupGameObjects.end());
	}
	void AddObjectDebugProperties(KX_GameObject *gameobj);
	KX_GameObject* AddReplicaObject(KX_GameObject *gameobj, KX_GameObject *locationobj, float lifespan=0.0f);
	KX_GameObject* AddNodeReplicaObject(SG_Node* node, KX_GameObject *gameobj);
	void RemoveNodeDestructObject(SG_Node *node, KX_GameObject *gameobj);
	void RemoveObject(KX_GameObject *gameobj);
	void RemoveDupliGroup(KX_GameObject *gameobj);
	void DelayedRemoveObject(KX_GameObject *gameobj);

	bool NewRemoveObject(KX_GameObject *gameobj);

	void AddAnimatedObject(KX_GameObject *gameobj);

	/**
	 * \section Logic stuff
	 * Initiate an update of the logic system.
	 */
	void LogicBeginFrame(double curtime, double framestep);
	void LogicUpdateFrame(double curtime);
	void UpdateAnimations(double curtime);

		void
	LogicEndFrame(
	);

	EXP_ListValue<KX_GameObject> *GetObjectList() const;
	EXP_ListValue<KX_GameObject> *GetInactiveList() const;
	EXP_ListValue<KX_GameObject> *GetRootParentList() const;
	EXP_ListValue<KX_LightObject> *GetLightList() const;

	SCA_LogicManager *GetLogicManager() const;

	SCA_TimeEventManager *GetTimeEventManager() const;

	KX_PythonComponentManager& GetPythonComponentManager();

	EXP_ListValue<KX_Camera> *GetCameraList() const;
	EXP_ListValue<KX_FontObject> *GetFontList() const;

	/** Find the currently active camera. */
		KX_Camera*
	GetActiveCamera(
	);

	/** 
	 * Set this camera to be the active camera in the scene. If the
	 * camera is not present in the camera list, it will be added
	 */

		void
	SetActiveCamera(
		class KX_Camera*
	);

	KX_Camera *GetOverrideCullingCamera() const;
	void SetOverrideCullingCamera(KX_Camera *cam);

	/**
	 * Move this camera to the end of the list so that it is rendered last.
	 * If the camera is not on the list, it will be added
	 */
		void
	SetCameraOnTop(
		class KX_Camera*
	);

	/**
	 * Set the framing options for this scene
	 */

		void
	SetFramingType(
		RAS_FrameSettings & frame_settings
	);

	/**
	 * Return a const reference to the framing 
	 * type set by the above call.
	 * The contents are not guaranteed to be sensible
	 * if you don't call the above function.
	 */

	const
		RAS_FrameSettings &
	GetFramingType(
	) const;

	/**
	 * \section Accessors to different scenes of this scene
	 */
	void SetNetworkMessageScene(KX_NetworkMessageScene *newScene);
	KX_NetworkMessageScene *GetNetworkMessageScene();

	void SetWorldInfo(class KX_WorldInfo* wi);
	KX_WorldInfo* GetWorldInfo();
	void CalculateVisibleMeshes(KX_CullingNodeList& nodes, KX_Camera *cam, int layer);
	void CalculateVisibleMeshes(KX_CullingNodeList& nodes, const SG_Frustum& frustum, int layer);

	/// \section Debug draw.
	void DrawDebug(RAS_DebugDraw& debugDraw, const KX_CullingNodeList& nodes);
	void RenderDebugProperties(RAS_DebugDraw& debugDraw, int xindent, int ysize, int& xcoord, int& ycoord, unsigned short propsMax);

	/**
	 * Replicate the logic bricks associated to this object.
	 */

	void ReplicateLogic(class KX_GameObject* newobj);
	static SG_Callbacks	m_callbacks;

	// Suspend the entire scene.
	void Suspend();

	// Resume a suspended scene.
	void Resume();

	/// Update the mesh for objects based on level of detail settings
	void UpdateObjectLods(KX_Camera *cam, const KX_CullingNodeList& nodes);

	// LoD Hysteresis functions
	void SetLodHysteresis(bool active);
	bool IsActivedLodHysteresis();
	void SetLodHysteresisValue(int hysteresisvalue);
	int GetLodHysteresisValue();
	
	/// Update the activity culling of objects in this scene, if needed.
	void UpdateObjectActivity();
	/// Enable/disable activity culling.
	void SetActivityCulling(bool b);

	bool IsSuspended();
	// use of DBVT tree for camera culling
	void SetDbvtCulling(bool b) { m_dbvt_culling = b; }
	bool GetDbvtCulling() { return m_dbvt_culling; }
	void SetDbvtOcclusionRes(int i) { m_dbvt_occlusion_res = i; }
	int GetDbvtOcclusionRes() { return m_dbvt_occlusion_res; }
	
	void SetSceneConverter(class BL_BlenderSceneConverter* sceneConverter);

	class PHY_IPhysicsEnvironment*		GetPhysicsEnvironment()
	{
		return m_physicsEnvironment;
	}

	void SetPhysicsEnvironment(class PHY_IPhysicsEnvironment*	physEnv);

	void	SetGravity(const MT_Vector3& gravity);
	MT_Vector3 GetGravity();

	short GetAnimationFPS();

	/**
	 * 2D Filters
	 */
	RAS_2DFilterManager *Get2DFilterManager() const;
	RAS_OffScreen *Render2DFilters(RAS_Rasterizer *rasty, RAS_ICanvas *canvas, RAS_OffScreen *inputofs, RAS_OffScreen *targetofs);

	KX_ObstacleSimulation *GetObstacleSimulation();
	void SetObstacleSimulation(KX_ObstacleSimulation *obstacleSimulation);

	/**  Inherited from EXP_Value -- returns the name of this object. */
	virtual std::string GetName();

	/** Inherited from EXP_Value -- set the name of this object. */
	virtual void SetName(const std::string& name);

#ifdef WITH_PYTHON
	/* --------------------------------------------------------------------- */
	/* Python interface ---------------------------------------------------- */
	/* --------------------------------------------------------------------- */

	EXP_PYMETHOD_DOC(KX_Scene, addObject);
	EXP_PYMETHOD_DOC(KX_Scene, end);
	EXP_PYMETHOD_DOC(KX_Scene, restart);
	EXP_PYMETHOD_DOC(KX_Scene, replace);
	EXP_PYMETHOD_DOC(KX_Scene, suspend);
	EXP_PYMETHOD_DOC(KX_Scene, resume);
	EXP_PYMETHOD_DOC(KX_Scene, get);
	EXP_PYMETHOD_DOC(KX_Scene, drawObstacleSimulation);


	/* attributes */
	static PyObject*	pyattr_get_name(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject*	pyattr_get_objects(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject*	pyattr_get_objects_inactive(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject*	pyattr_get_lights(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject*	pyattr_get_texts(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject*	pyattr_get_cameras(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject*	pyattr_get_filter_manager(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject*	pyattr_get_world(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject*	pyattr_get_active_camera(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int			pyattr_set_active_camera(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject*	pyattr_get_overrideCullingCamera(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int			pyattr_set_overrideCullingCamera(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject*	pyattr_get_drawing_callback(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int			pyattr_set_drawing_callback(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject*	pyattr_get_gravity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int			pyattr_set_gravity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	
	/* getitem/setitem */
	static PyMappingMethods	Mapping;
	static PySequenceMethods	Sequence;

	/**
	 * Run the registered python drawing functions.
	 */
	void RunDrawingCallbacks(DrawingCallbackType callbackType, KX_Camera *camera);
#endif

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
	/**
	 * Returns the Blender scene this was made from
	 */
	struct Scene *GetBlenderScene() { return m_blenderScene; }

	bool MergeScene(KX_Scene *other);


	//void PrintStats(int verbose_level) {
	//	m_bucketmanager->PrintStats(verbose_level)
	//}
};

#ifdef WITH_PYTHON
bool ConvertPythonToScene(PyObject *value, KX_Scene **scene, bool py_none_ok, const char *error_prefix);
#endif

typedef std::vector<KX_Scene*> KX_SceneList;

#endif  /* __KX_SCENE_H__ */
