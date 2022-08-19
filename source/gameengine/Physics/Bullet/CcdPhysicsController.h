/*
   Bullet Continuous Collision Detection and Physics Library
   Copyright (c) 2003-2006 Erwin Coumans  http://continuousphysics.com/Bullet/

   This software is provided 'as-is', without any express or implied warranty.
   In no event will the authors be held liable for any damages arising from the use of this
   software. Permission is granted to anyone to use this software for any purpose, including
   commercial applications, and to alter it and redistribute it freely, subject to the following
   restrictions:

   1. The origin of this software must not be misrepresented; you must not claim that you wrote the
   original software. If you use this software in a product, an acknowledgment in the product
   documentation would be appreciated but is not required.
   2. Altered source versions must be plainly marked as such, and must not be misrepresented as
   being the original software.
   3. This notice may not be removed or altered from any source distribution.
 */

/** \file CcdPhysicsController.h
 *  \ingroup physbullet
 */

#pragma once

#include <map>
#include <vector>

///	PHY_IPhysicsController is the abstract simplified Interface to a physical object.
///	It contains the IMotionState and IDeformableMesh Interfaces.
#include "BulletDynamics/Character/btKinematicCharacterController.h"
#include "LinearMath/btTransform.h"
#include "btBulletDynamicsCommon.h"

#include "CM_RefCount.h"
#include "CcdMathUtils.h"
#include "PHY_ICharacter.h"
#include "PHY_IMotionState.h"
#include "PHY_IPhysicsController.h"

extern btScalar gDeactivationTime;
extern float gLinearSleepingTreshold;
extern float gAngularSleepingTreshold;
extern bool gDisableDeactivation;
class CcdPhysicsEnvironment;
class CcdPhysicsController;
class btMotionState;
class RAS_MeshObject;
struct DerivedMesh;
class btCollisionShape;

#define CCD_BSB_SHAPE_MATCHING 2
#define CCD_BSB_BENDING_CONSTRAINTS 8
#define CCD_BSB_AERO_VPOINT 16   /* aero model, Vertex normals are oriented toward velocity*/
#define CCD_BSB_AERO_VTWOSIDE 32 /* aero model, Vertex normals are flipped to match velocity */

/* BulletSoftBody.collisionflags */
#define CCD_BSB_COL_SDF_RS 2 /* SDF based rigid vs soft */
#define CCD_BSB_COL_CL_RS 4  /* Cluster based rigid vs soft */
#define CCD_BSB_COL_CL_SS 8  /* Cluster based soft vs soft */
#define CCD_BSB_COL_VF_SS 16 /* Vertex/Face based soft vs soft */

// Shape contructor
// It contains all the information needed to create a simple bullet shape at runtime
class CcdShapeConstructionInfo : public CM_RefCount<CcdShapeConstructionInfo> {
 public:
  struct UVco {
    float uv[2];
  };

  static CcdShapeConstructionInfo *FindMesh(class RAS_MeshObject *mesh,
                                            struct DerivedMesh *dm,
                                            bool polytope);

  CcdShapeConstructionInfo()
      : m_shapeType(PHY_SHAPE_NONE),
        m_radius(1.0f),
        m_height(1.0f),
        m_halfExtend(0.0f, 0.0f, 0.0f),
        m_childScale(1.0f, 1.0f, 1.0f),
        m_userData(nullptr),
        m_meshObject(nullptr),
        m_triangleIndexVertexArray(nullptr),
        m_forceReInstance(false),
        m_weldingThreshold1(0.0f),
        m_shapeProxy(nullptr)
  {
    m_childTrans.setIdentity();
  }

  ~CcdShapeConstructionInfo();

  bool IsUnused(void)
  {
    return (m_meshObject == nullptr && m_shapeArray.empty() && m_shapeProxy == nullptr);
  }

  void AddShape(CcdShapeConstructionInfo *shapeInfo);

  btStridingMeshInterface *GetMeshInterface()
  {
    return m_triangleIndexVertexArray;
  }

  CcdShapeConstructionInfo *GetChildShape(int i)
  {
    if (i < 0 || i >= (int)m_shapeArray.size())
      return nullptr;

    return m_shapeArray.at(i);
  }
  int FindChildShape(CcdShapeConstructionInfo *shapeInfo, void *userData)
  {
    if (shapeInfo == nullptr)
      return -1;
    for (int i = 0; i < (int)m_shapeArray.size(); i++) {
      CcdShapeConstructionInfo *childInfo = m_shapeArray.at(i);
      if ((userData == nullptr || userData == childInfo->m_userData) &&
          (childInfo == shapeInfo ||
           (childInfo->m_shapeType == PHY_SHAPE_PROXY && childInfo->m_shapeProxy == shapeInfo))) {
        return i;
      }
    }
    return -1;
  }

  bool RemoveChildShape(int i)
  {
    if (i < 0 || i >= (int)m_shapeArray.size())
      return false;
    m_shapeArray.at(i)->Release();
    if (i < (int)m_shapeArray.size() - 1)
      m_shapeArray[i] = m_shapeArray.back();
    m_shapeArray.pop_back();
    return true;
  }

  bool SetMesh(class KX_Scene *kxscene,
               class RAS_MeshObject *mesh,
               struct DerivedMesh *dm,
               bool polytope);

  RAS_MeshObject *GetMesh(void)
  {
    return m_meshObject;
  }

  bool UpdateMesh(class KX_GameObject *from_gameobj,
                  class RAS_MeshObject *from_meshobj,
                  bool evaluatedMesh = false);

  CcdShapeConstructionInfo *GetReplica();

  void ProcessReplica();

  bool SetProxy(CcdShapeConstructionInfo *shapeInfo);
  CcdShapeConstructionInfo *GetProxy(void)
  {
    return m_shapeProxy;
  }

  btCollisionShape *CreateBulletShape(btScalar margin,
                                      bool useGimpact = false,
                                      bool useBvh = true);

  // member variables
  PHY_ShapeType m_shapeType;
  btScalar m_radius;
  btScalar m_height;
  btVector3 m_halfExtend;
  btTransform m_childTrans;
  btVector3 m_childScale;
  void *m_userData;
  /** Contains both vertex array for polytope shape and triangle array for concave mesh shape.
   * Each vertex is 3 consecutive values. In this case a triangle is made of 3 consecutive points
   */
  btAlignedObjectArray<btScalar> m_vertexArray;
  /** Contains the array of polygon index in the original mesh that correspond to shape triangles.
   * only set for concave mesh shape.
   */
  std::vector<int> m_polygonIndexArray;

  /// Contains an array of triplets of face indices quads turn into 2 tris
  std::vector<int> m_triFaceArray;

  /// Contains an array of pair of UV coordinate for each vertex of faces quads turn into 2 tris
  std::vector<UVco> m_triFaceUVcoArray;

  void setVertexWeldingThreshold1(float threshold)
  {
    m_weldingThreshold1 = threshold * threshold;
  }

 protected:
  static std::map<RAS_MeshObject *, CcdShapeConstructionInfo *> m_meshShapeMap;
  /// Keep a pointer to the original mesh
  RAS_MeshObject *m_meshObject;
  /// The list of vertexes and indexes for the triangle mesh, shared between Bullet shape.
  btTriangleIndexVertexArray *m_triangleIndexVertexArray;
  /// for compound shapes
  std::vector<CcdShapeConstructionInfo *> m_shapeArray;
  /// use gimpact for concave dynamic/moving collision detection
  bool m_forceReInstance;
  /// welding closeby vertices together can improve softbody stability etc.
  float m_weldingThreshold1;
  /// only used for PHY_SHAPE_PROXY, pointer to actual shape info
  CcdShapeConstructionInfo *m_shapeProxy;
};

struct CcdConstructionInfo {

  /** CollisionFilterGroups provides some optional usage of basic collision filtering
   * this is done during broadphase, so very early in the pipeline
   * more advanced collision filtering should be done in btCollisionDispatcher::NeedsCollision
   */
  enum CollisionFilterGroups {
    DynamicFilter = 1,
    StaticFilter = 2,
    KinematicFilter = 4,
    DebrisFilter = 8,
    SensorFilter = 16,
    CharacterFilter = 32,
    AllFilter = DynamicFilter | StaticFilter | KinematicFilter | DebrisFilter | SensorFilter |
                CharacterFilter,
  };

  CcdConstructionInfo()
      : m_localInertiaTensor(1.0f, 1.0f, 1.0f),
        m_gravity(0.0f, 0.0f, 0.0f),
        m_scaling(1.0f, 1.0f, 1.0f),
        m_linearFactor(0.0f, 0.0f, 0.0f),
        m_angularFactor(0.0f, 0.0f, 0.0f),
        m_mass(0.0f),
        m_clamp_vel_min(-1.0f),
        m_clamp_vel_max(-1.0f),
        m_clamp_angvel_min(0.0f),
        m_clamp_angvel_max(0.0f),
        m_restitution(0.1f),
        m_friction(0.5f),
        m_rollingFriction(0.0f),
        m_linearDamping(0.1f),
        m_angularDamping(0.1f),
        m_margin(0.06f),
        m_gamesoftFlag(0),
        m_softBendingDistance(2),
        m_soft_linStiff(1.0f),
        m_soft_angStiff(1.0f),
        m_soft_volume(1.0f),
        m_soft_viterations(0),
        m_soft_piterations(1),
        m_soft_diterations(0),
        m_soft_citerations(4),
        m_soft_kSRHR_CL(0.1f),
        m_soft_kSKHR_CL(1.0f),
        m_soft_kSSHR_CL(0.5f),
        m_soft_kSR_SPLT_CL(0.5f),
        m_soft_kSK_SPLT_CL(0.5f),
        m_soft_kSS_SPLT_CL(0.5f),
        m_soft_kVCF(1.0f),
        m_soft_kDP(0.0f),
        m_soft_kDG(0.0f),
        m_soft_kLF(0.0f),
        m_soft_kPR(0.0f),
        m_soft_kVC(0.0f),
        m_soft_kDF(0.2f),
        m_soft_kMT(0),
        m_soft_kCHR(1.0f),
        m_soft_kKHR(0.1f),
        m_soft_kSHR(1.0f),
        m_soft_kAHR(0.7f),
        m_collisionFlags(0),
        m_bDyna(false),
        m_bRigid(false),
        m_bSoft(false),
        m_bSensor(false),
        m_bCharacter(false),
        m_bGimpact(false),
        m_collisionFilterGroup(DynamicFilter),
        m_collisionFilterMask(AllFilter),
        m_collisionGroup(0xFFFF),
        m_collisionMask(0xFFFF),
        m_collisionShape(nullptr),
        m_MotionState(nullptr),
        m_shapeInfo(nullptr),
        m_physicsEnv(nullptr),
        m_inertiaFactor(1.0f),
        m_do_anisotropic(false),
        m_anisotropicFriction(1.0f, 1.0f, 1.0f),
        m_do_fh(false),
        m_do_rot_fh(false),
        m_fh_spring(0.0f),
        m_fh_damping(0.0f),
        m_fh_distance(1.0f),
        m_fh_normal(false),
        m_ccd_motion_threshold(1.0f),
        m_ccd_swept_sphere_radius(0.9f)
  // m_contactProcessingThreshold(1e10f)
  {
  }

  btVector3 m_localInertiaTensor;
  btVector3 m_gravity;
  btVector3 m_scaling;
  btVector3 m_linearFactor;
  btVector3 m_angularFactor;
  btScalar m_mass;
  btScalar m_clamp_vel_min;
  btScalar m_clamp_vel_max;
  /// Minimum angular velocity, in radians/sec.
  btScalar m_clamp_angvel_min;
  /// Maximum angular velocity, in radians/sec.
  btScalar m_clamp_angvel_max;
  btScalar m_restitution;
  btScalar m_friction;
  btScalar m_rollingFriction;
  btScalar m_linearDamping;
  btScalar m_angularDamping;
  btScalar m_margin;

  float m_stepHeight;
  float m_jumpSpeed;
  float m_fallSpeed;
  float m_maxSlope;
  unsigned char m_maxJumps;

  int m_gamesoftFlag;
  unsigned short m_softBendingDistance;
  /// linear stiffness 0..1
  float m_soft_linStiff;
  /// angular stiffness 0..1
  float m_soft_angStiff;
  ///  volume preservation 0..1
  float m_soft_volume;

  /// Velocities solver iterations
  int m_soft_viterations;
  /// Positions solver iterations
  int m_soft_piterations;
  /// Drift solver iterations
  int m_soft_diterations;
  /// Cluster solver iterations
  int m_soft_citerations;

  /// Soft vs rigid hardness [0,1] (cluster only)
  float m_soft_kSRHR_CL;
  /// Soft vs kinetic hardness [0,1] (cluster only)
  float m_soft_kSKHR_CL;
  /// Soft vs soft hardness [0,1] (cluster only)
  float m_soft_kSSHR_CL;
  /// Soft vs rigid impulse split [0,1] (cluster only)
  float m_soft_kSR_SPLT_CL;
  /// Soft vs rigid impulse split [0,1] (cluster only)
  float m_soft_kSK_SPLT_CL;
  /// Soft vs rigid impulse split [0,1] (cluster only)
  float m_soft_kSS_SPLT_CL;
  /// Velocities correction factor (Baumgarte)
  float m_soft_kVCF;
  /// Damping coefficient [0,1]
  float m_soft_kDP;

  /// Drag coefficient [0,+inf]
  float m_soft_kDG;
  /// Lift coefficient [0,+inf]
  float m_soft_kLF;
  /// Pressure coefficient [-inf,+inf]
  float m_soft_kPR;
  /// Volume conversation coefficient [0,+inf]
  float m_soft_kVC;

  /// Dynamic friction coefficient [0,1]
  float m_soft_kDF;
  /// Pose matching coefficient [0,1]
  float m_soft_kMT;
  /// Rigid contacts hardness [0,1]
  float m_soft_kCHR;
  /// Kinetic contacts hardness [0,1]
  float m_soft_kKHR;

  /// Soft contacts hardness [0,1]
  float m_soft_kSHR;
  /// Anchors hardness [0,1]
  float m_soft_kAHR;
  /// Vertex/Face or Signed Distance Field(SDF) or Clusters, Soft versus Soft or Rigid
  int m_soft_collisionflags;
  /// number of iterations to refine collision clusters
  int m_soft_numclusteriterations;

  int m_collisionFlags;
  bool m_bDyna;
  bool m_bRigid;
  bool m_bSoft;
  bool m_bSensor;
  bool m_bCharacter;
  /// use Gimpact for mesh body
  bool m_bGimpact;

  /** optional use of collision group/mask:
   * only collision with object goups that match the collision mask.
   * this is very basic early out. advanced collision filtering should be
   * done in the btCollisionDispatcher::NeedsCollision and NeedsResponse
   * both values default to 1
   */
  short int m_collisionFilterGroup;
  short int m_collisionFilterMask;

  unsigned short m_collisionGroup;
  unsigned short m_collisionMask;

  /** these pointers are used as argument passing for the CcdPhysicsController constructor
   * and not anymore after that
   */
  class btCollisionShape *m_collisionShape;
  class PHY_IMotionState *m_MotionState;
  class CcdShapeConstructionInfo *m_shapeInfo;

  /// needed for self-replication
  CcdPhysicsEnvironment *m_physicsEnv;
  /// tweak the inertia (hooked up to Blender 'formfactor'
  float m_inertiaFactor;
  bool m_do_anisotropic;
  btVector3 m_anisotropicFriction;

  /// Should the object have a linear Fh spring?
  bool m_do_fh;
  /// Should the object have an angular Fh spring?
  bool m_do_rot_fh;
  /// Spring constant (both linear and angular)
  btScalar m_fh_spring;
  /// Damping factor (linear and angular) in range [0, 1]
  btScalar m_fh_damping;
  /// The range above the surface where Fh is active.
  btScalar m_fh_distance;
  /// Should the object slide off slopes?
  bool m_fh_normal;
  /// for fh backwards compatibility
  float m_radius;

  /// Ccd
  btScalar m_ccd_motion_threshold;
  btScalar m_ccd_swept_sphere_radius;

  /** m_contactProcessingThreshold allows to process contact points with positive distance
   * normally only contacts with negative distance (penetration) are solved
   * however, rigid body stacking is more stable when positive contacts are still passed into the
   * constraint solver this might sometimes lead to collisions with 'internal edges' such as a
   * sliding character controller so disable/set m_contactProcessingThreshold to zero for sliding
   * characters etc.
   */
  // float		m_contactProcessingThreshold;///< Process contacts with positive distance in range
  // [0..INF]
};

class btRigidBody;
class btCollisionObject;
class btSoftBody;
class btPairCachingGhostObject;

class CcdCharacter : public btKinematicCharacterController,
                                         public PHY_ICharacter {
 private:
  CcdPhysicsController *m_ctrl;
  btMotionState *m_motionState;
  unsigned char m_jumps;
  unsigned char m_maxJumps;

 public:
  CcdCharacter(CcdPhysicsController *ctrl,
                                   btMotionState *motionState,
                                   btPairCachingGhostObject *ghost,
                                   btConvexShape *shape,
                                   float stepHeight);

  virtual void updateAction(btCollisionWorld *collisionWorld, btScalar dt);

  unsigned char getMaxJumps() const;

  void setMaxJumps(unsigned char maxJumps);

  unsigned char getJumpCount() const;

  virtual bool canJump() const;

  virtual void jump();

  const btVector3 &getWalkDirection();

  void SetVelocity(const btVector3 &vel, float time, bool local);

  /// Replace current convex shape.
  void ReplaceShape(btConvexShape *shape);

  // PHY_ICharacter interface
  virtual void Jump()
  {
    jump();
  }
  virtual bool OnGround()
  {
    return onGround();
  }
  virtual MT_Vector3 GetGravity()
  {
    return ToMoto(getGravity());
  }
  virtual void SetGravity(const MT_Vector3 &gravity)
  {
    setGravity(ToBullet(gravity));
  }
  virtual unsigned char GetMaxJumps()
  {
    return getMaxJumps();
  }
  virtual void SetMaxJumps(unsigned char maxJumps)
  {
    setMaxJumps(maxJumps);
  }
  virtual unsigned char GetJumpCount()
  {
    return getJumpCount();
  }
  virtual void SetWalkDirection(const MT_Vector3 &dir)
  {
    setWalkDirection(ToBullet(dir));
  }

  virtual MT_Vector3 GetWalkDirection()
  {
    return ToMoto(getWalkDirection());
  }

  virtual float GetFallSpeed() const;
  virtual void SetFallSpeed(float fallSpeed);

  virtual float GetMaxSlope() const;
  virtual void SetMaxSlope(float maxSlope);

  virtual float GetJumpSpeed() const;
  virtual void SetJumpSpeed(float jumpSpeed);

  virtual void SetVelocity(const MT_Vector3 &vel, float time, bool local);

  virtual void Reset();
};

class CleanPairCallback : public btOverlapCallback {
  btBroadphaseProxy *m_cleanProxy;
  btOverlappingPairCache *m_pairCache;
  btDispatcher *m_dispatcher;

 public:
  CleanPairCallback(btBroadphaseProxy *cleanProxy,
                    btOverlappingPairCache *pairCache,
                    btDispatcher *dispatcher)
      : m_cleanProxy(cleanProxy), m_pairCache(pairCache), m_dispatcher(dispatcher)
  {
  }

  virtual bool processOverlap(btBroadphasePair &pair);
};

/// CcdPhysicsController is a physics object that supports continuous collision detection and time
/// of impact based physics resolution.
class CcdPhysicsController : public PHY_IPhysicsController {
 protected:
  btCollisionObject *m_object;
  CcdCharacter *m_characterController;

  class PHY_IMotionState *m_MotionState;
  btMotionState *m_bulletMotionState;
  class btCollisionShape *m_collisionShape;
  class CcdShapeConstructionInfo *m_shapeInfo;
  btCollisionShape *m_bulletChildShape;

  /// keep track of typed constraints referencing this rigid body
  btAlignedObjectArray<btTypedConstraint *> m_ccdConstraintRefs;
  /// needed when updating the controller
  friend class CcdPhysicsEnvironment;

  // some book keeping for replication
  bool m_softbodyMappingDone;
  bool m_softBodyTransformInitialized;
  bool m_prototypeTransformInitialized;
  btTransform m_softbodyStartTrans;

  void *m_newClientInfo;
  int m_registerCount;        // needed when multiple sensors use the same controller
  CcdConstructionInfo m_cci;  // needed for replication

  CcdPhysicsController *m_parentRoot;

  int m_savedCollisionFlags;
  short m_savedCollisionFilterGroup;
  short m_savedCollisionFilterMask;
  MT_Scalar m_savedMass;
  MT_Scalar m_savedFriction;
  bool m_savedDyna;
  bool m_suspended;

  void GetWorldOrientation(btMatrix3x3 &mat);

  void CreateRigidbody();
  bool CreateSoftbody();
  bool CreateCharacterController();

  bool Register()
  {
    return (m_registerCount++ == 0);
  }
  bool Unregister()
  {
    return (--m_registerCount == 0);
  }

  bool Registered() const
  {
    return (m_registerCount != 0);
  }

  void addCcdConstraintRef(btTypedConstraint *c);
  void removeCcdConstraintRef(btTypedConstraint *c);
  btTypedConstraint *getCcdConstraintRef(int index);
  int getNumCcdConstraintRefs() const;

  void SetWorldOrientation(const btMatrix3x3 &mat);
  void ForceWorldTransform(const btMatrix3x3 &mat, const btVector3 &pos);

 public:
  CcdPhysicsController(const CcdConstructionInfo &ci);

  /**
   * Delete the current Bullet shape used in the rigid body.
   */
  bool DeleteControllerShape();

  /**
   * Delete the old Bullet shape and set the new Bullet shape : newShape
   * \param newShape The new Bullet shape to set, if is nullptr we create a new Bullet shape
   */
  bool ReplaceControllerShape(btCollisionShape *newShape);

  virtual ~CcdPhysicsController();

  CcdConstructionInfo &GetConstructionInfo()
  {
    return m_cci;
  }
  const CcdConstructionInfo &GetConstructionInfo() const
  {
    return m_cci;
  }

  btRigidBody *GetRigidBody();
  const btRigidBody *GetRigidBody() const;
  btCollisionObject *GetCollisionObject();
  btSoftBody *GetSoftBody();
  btKinematicCharacterController *GetCharacterController();

  CcdShapeConstructionInfo *GetShapeInfo()
  {
    return m_shapeInfo;
  }

  btCollisionShape *GetCollisionShape()
  {
    return m_object->getCollisionShape();
  }
  ////////////////////////////////////
  // PHY_IPhysicsController interface
  ////////////////////////////////////

  /**
   * SynchronizeMotionStates ynchronizes dynas, kinematic and deformable entities (and do 'late
   * binding')
   */
  virtual bool SynchronizeMotionStates(float time);

  virtual void UpdateSoftBody();
  virtual void SetSoftBodyTransform(const MT_Vector3 &pos, const MT_Matrix3x3 &ori);

  /**
   * Called for every physics simulation step. Use this method for
   * things like limiting linear and angular velocity.
   */
  void SimulationTick(float timestep);

  /**
   * WriteMotionStateToDynamics ynchronizes dynas, kinematic and deformable entities (and do 'late
   * binding')
   */
  virtual void WriteMotionStateToDynamics(bool nondynaonly);
  virtual void WriteDynamicsToMotionState();

  // controller replication
  virtual void PostProcessReplica(class PHY_IMotionState *motionstate,
                                  class PHY_IPhysicsController *parentctrl);
  virtual void SetPhysicsEnvironment(class PHY_IPhysicsEnvironment *env);

  // kinematic methods
  virtual void RelativeTranslate(const MT_Vector3 &dloc, bool local);
  virtual void RelativeRotate(const MT_Matrix3x3 &rotval, bool local);
  virtual MT_Matrix3x3 GetOrientation();
  virtual void SetOrientation(const MT_Matrix3x3 &orn);
  virtual void SetPosition(const MT_Vector3 &pos);
  virtual void GetPosition(MT_Vector3 &pos) const;
  virtual void SetScaling(const MT_Vector3 &scale);
  virtual void SetTransform();

  virtual MT_Scalar GetMass();
  virtual void SetMass(MT_Scalar newmass);

  float GetInertiaFactor() const;

  virtual MT_Scalar GetFriction();
  virtual void SetFriction(MT_Scalar newfriction);

  // physics methods
  virtual void ApplyImpulse(const MT_Vector3 &attach, const MT_Vector3 &impulsein, bool local);
  virtual void ApplyTorque(const MT_Vector3 &torque, bool local);
  virtual void ApplyForce(const MT_Vector3 &force, bool local);
  virtual void SetAngularVelocity(const MT_Vector3 &ang_vel, bool local);
  virtual void SetLinearVelocity(const MT_Vector3 &lin_vel, bool local);
  virtual void Jump();
  virtual void SetActive(bool active);

  virtual unsigned short GetCollisionGroup() const;
  virtual unsigned short GetCollisionMask() const;
  virtual void SetCollisionGroup(unsigned short group);
  virtual void SetCollisionMask(unsigned short mask);

  virtual float GetLinearDamping() const;
  virtual float GetAngularDamping() const;
  virtual void SetLinearDamping(float damping);
  virtual void SetAngularDamping(float damping);
  virtual void SetDamping(float linear, float angular);
  virtual void SetGravity(const MT_Vector3 &gravity);

  // reading out information from physics
  virtual MT_Vector3 GetLinearVelocity();
  virtual MT_Vector3 GetAngularVelocity();
  virtual MT_Vector3 GetVelocity(const MT_Vector3 &posin);
  virtual MT_Vector3 GetLocalInertia();
  virtual MT_Vector3 GetGravity();

  // dyna's that are rigidbody are free in orientation, dyna's with non-rigidbody are restricted
  virtual void SetRigidBody(bool rigid);

  virtual void RefreshCollisions();
  virtual void SuspendPhysics(bool freeConstraints);
  virtual void RestorePhysics();
  virtual void SuspendDynamics(bool ghost);
  virtual void RestoreDynamics();

  // Shape control
  virtual void AddCompoundChild(PHY_IPhysicsController *child);
  virtual void RemoveCompoundChild(PHY_IPhysicsController *child);

  // clientinfo for raycasts for example
  virtual void *GetNewClientInfo();
  virtual void SetNewClientInfo(void *clientinfo);
  virtual PHY_IPhysicsController *GetReplica();
  virtual PHY_IPhysicsController *GetReplicaForSensors();

  /// There should be no 'SetCollisionFilterGroup' method, as changing this during run-time is will
  /// result in errors
  short int GetCollisionFilterGroup() const
  {
    return m_cci.m_collisionFilterGroup;
  }
  /// There should be no 'SetCollisionFilterGroup' method, as changing this during run-time is will
  /// result in errors
  short int GetCollisionFilterMask() const
  {
    return m_cci.m_collisionFilterMask;
  }

  virtual void SetMargin(float margin)
  {
    if (m_collisionShape) {
      m_collisionShape->setMargin(margin);
      // if the shape use a unscaled shape we have also to set the correct margin in it
      if (m_collisionShape->getShapeType() == SCALED_TRIANGLE_MESH_SHAPE_PROXYTYPE)
        ((btScaledBvhTriangleMeshShape *)m_collisionShape)->getChildShape()->setMargin(margin);
    }
  }
  virtual float GetMargin() const
  {
    return (m_collisionShape) ? m_collisionShape->getMargin() : 0.0f;
  }
  virtual float GetRadius() const
  {
    // this is not the actual shape radius, it's only used for Fh support
    return m_cci.m_radius;
  }
  virtual void SetRadius(float margin)
  {
    if (m_collisionShape && m_collisionShape->getShapeType() == SPHERE_SHAPE_PROXYTYPE) {
      btSphereShape *sphereShape = static_cast<btSphereShape *>(m_collisionShape);
      sphereShape->setUnscaledRadius(margin);
    }
    m_cci.m_radius = margin;
  }

  /// velocity clamping
  virtual void SetLinVelocityMin(float val)
  {
    m_cci.m_clamp_vel_min = val;
  }
  virtual float GetLinVelocityMin() const
  {
    return m_cci.m_clamp_vel_min;
  }
  virtual void SetLinVelocityMax(float val)
  {
    m_cci.m_clamp_vel_max = val;
  }
  virtual float GetLinVelocityMax() const
  {
    return m_cci.m_clamp_vel_max;
  }

  virtual void SetAngularVelocityMin(float val)
  {
    m_cci.m_clamp_angvel_min = val;
  }
  virtual float GetAngularVelocityMin() const
  {
    return m_cci.m_clamp_angvel_min;
  }
  virtual void SetAngularVelocityMax(float val)
  {
    m_cci.m_clamp_angvel_max = val;
  }
  virtual float GetAngularVelocityMax() const
  {
    return m_cci.m_clamp_angvel_max;
  }

  bool WantsSleeping();

  void UpdateDeactivation(float timeStep);

  void SetCenterOfMassTransform(btTransform &xform);

  static btTransform GetTransformFromMotionState(PHY_IMotionState *motionState);

  class PHY_IMotionState *GetMotionState()
  {
    return m_MotionState;
  }

  const class PHY_IMotionState *GetMotionState() const
  {
    return m_MotionState;
  }

  class CcdPhysicsEnvironment *GetPhysicsEnvironment()
  {
    return m_cci.m_physicsEnv;
  }

  void SetParentRoot(CcdPhysicsController *parentCtrl)
  {
    m_parentRoot = parentCtrl;
  }

  CcdPhysicsController *GetParentRoot() const
  {
    return m_parentRoot;
  }

  virtual bool IsDynamic()
  {
    return GetConstructionInfo().m_bDyna;
  }

  virtual bool IsDynamicsSuspended() const
  {
    return m_suspended;
  }

  virtual bool IsPhysicsSuspended();

  virtual bool IsCompound()
  {
    return GetConstructionInfo().m_shapeInfo->m_shapeType == PHY_SHAPE_COMPOUND;
  }

  virtual bool ReinstancePhysicsShape(KX_GameObject *from_gameobj,
                                      RAS_MeshObject *from_meshobj,
                                      bool dupli = false,
                                      bool evaluatedMesh = false);

  virtual bool ReplacePhysicsShape(PHY_IPhysicsController *phyctrl);

  /* Method to replicate rigid body joint contraints for group instances. */
  virtual void ReplicateConstraints(KX_GameObject *gameobj, std::vector<KX_GameObject *> constobj);

  // CCD methods
  virtual void SetCcdMotionThreshold(float val);
  virtual void SetCcdSweptSphereRadius(float val);
};

/// DefaultMotionState implements standard motionstate, using btTransform
class DefaultMotionState : public PHY_IMotionState {
 public:
  DefaultMotionState();

  virtual ~DefaultMotionState();

  virtual MT_Vector3 GetWorldPosition() const;
  virtual MT_Vector3 GetWorldScaling() const;
  virtual MT_Matrix3x3 GetWorldOrientation() const;

  virtual void SetWorldPosition(const MT_Vector3 &pos);
  virtual void SetWorldOrientation(const MT_Matrix3x3 &ori);
  virtual void SetWorldOrientation(const MT_Quaternion &quat);

  virtual void CalculateWorldTransformations();

  btTransform m_worldTransform;
  btVector3 m_localScaling;
};
