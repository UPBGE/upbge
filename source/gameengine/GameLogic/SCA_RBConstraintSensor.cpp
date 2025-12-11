/** \file gameengine/GameLogic/SCA_RBConstraintSensor.cpp
 *  \ingroup gamelogic
 */

#include "SCA_RBConstraintSensor.h"

#include "KX_GameObject.h"
#include "KX_Scene.h"
#include "PHY_IPhysicsEnvironment.h"

#ifdef WITH_BULLET
#  include "../Physics/Bullet/CcdPhysicsEnvironment.h"
#endif

SCA_RBConstraintSensor::SCA_RBConstraintSensor(SCA_EventManager *eventmgr,
                                               SCA_IObject *gameobj,
                                               const std::string &targetName)
    : SCA_ISensor(gameobj, eventmgr), m_targetName(targetName), m_lastResult(false)
{
  Init();
}

SCA_RBConstraintSensor::~SCA_RBConstraintSensor()
{
  /* intentionally empty */
}

void SCA_RBConstraintSensor::Init()
{
  m_lastResult = false;
  m_reset = true;
}

EXP_Value *SCA_RBConstraintSensor::GetReplica()
{
  EXP_Value *replica = new SCA_RBConstraintSensor(*this);
  replica->ProcessReplica();
  return replica;
}

bool SCA_RBConstraintSensor::Evaluate()
{
  /* During game shutdown, parent may be invalid - just return false */
  SCA_IObject *parent = GetParent();
  if (!parent) {
    return false;
  }

  KX_GameObject *selfObj = static_cast<KX_GameObject *>(parent);

  /* Determine which object's constraints to check:
   * - If m_targetName is empty, check this object (self)
   * - If m_targetName is set, find that object and check its constraints
   */
  KX_GameObject *targetObj = selfObj;

  if (!m_targetName.empty()) {
    /* Find the target object by name */
    KX_Scene *scene = selfObj->GetScene();
    if (!scene) {
      bool reset = m_reset && m_level;
      m_reset = false;
      return reset;
    }
    targetObj = scene->GetObjectList()->FindValue(m_targetName);
    if (!targetObj) {
      /* Target object not found - sensor returns false */
      bool reset = m_reset && m_level;
      m_reset = false;
      if (m_lastResult != false) {
        m_lastResult = false;
        return true;
      }
      return reset;
    }
  }

  /* Check if the target object has any rigid body constraints */
  if (!targetObj->HasRigidBodyConstraints()) {
    /* No constraints on target object - sensor always returns false */
    bool reset = m_reset && m_level;
    m_reset = false;
    if (m_lastResult != false) {
      m_lastResult = false;
      return true;
    }
    return reset;
  }

  /* Get the scene - may be null during shutdown */
  KX_Scene *scene = targetObj->GetScene();
  if (!scene) {
    bool reset = m_reset && m_level;
    m_reset = false;
    return reset;
  }

  /* Get physics environment - may be null during shutdown */
  PHY_IPhysicsEnvironment *physEnv = scene->GetPhysicsEnvironment();
  if (!physEnv) {
    bool reset = m_reset && m_level;
    m_reset = false;
    return reset;
  }

  /* If Bullet world was already destroyed during shutdown, bail out safely. */
#ifdef WITH_BULLET
  if (CcdPhysicsEnvironment *bulletEnv = dynamic_cast<CcdPhysicsEnvironment *>(physEnv)) {
    if (bulletEnv->GetDynamicsWorld() == nullptr) {
      bool reset = m_reset && m_level;
      m_reset = false;
      return reset;
    }
  }
#endif

  /* Check if any constraint on the target object is broken (disabled) */
  bool broken = false;
  const std::vector<KX_GameObject::RigidBodyConstraintData> &constraints =
      targetObj->GetRigidBodyConstraints();

  for (const KX_GameObject::RigidBodyConstraintData &data : constraints) {
    /* Skip invalid constraint IDs */
    if (data.m_constraintId == -1) {
      continue;
    }

    /* Check if this constraint is disabled (broken) */
    if (!physEnv->IsRigidBodyConstraintEnabled(data.m_constraintId)) {
      broken = true;
      break;
    }
  }

  bool reset = m_reset && m_level;
  m_reset = false;

  if (broken != m_lastResult) {
    m_lastResult = broken;
    return true;
  }

  return reset;
}

bool SCA_RBConstraintSensor::IsPositiveTrigger()
{
  return (m_invert ? !m_lastResult : m_lastResult);
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_RBConstraintSensor::Type = {PyVarObject_HEAD_INIT(nullptr, 0) "SCA_RBConstraintSensor",
                                             sizeof(EXP_PyObjectPlus_Proxy),
                                             0,
                                             py_base_dealloc,
                                             0,
                                             0,
                                             0,
                                             0,
                                             py_base_repr,
                                             0,
                                             0,
                                             0,
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
                                             &SCA_ISensor::Type,
                                             0,
                                             0,
                                             0,
                                             0,
                                             0,
                                             0,
                                             py_base_new};

PyMethodDef SCA_RBConstraintSensor::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_RBConstraintSensor::Attributes[] = {
    EXP_PYATTRIBUTE_STRING_RW("target", 0, 64, false, SCA_RBConstraintSensor, m_targetName),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

#endif  // WITH_PYTHON
