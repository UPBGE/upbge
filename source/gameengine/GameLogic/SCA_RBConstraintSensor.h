#pragma once

/** \file SCA_RBConstraintSensor.h
 *  \ingroup gamelogic
 *  \brief Sensor to detect if a rigid body constraint is broken.
 */

#include "SCA_ISensor.h"

#include <string>

class SCA_RBConstraintSensor : public SCA_ISensor {
  Py_Header

  std::string m_targetName;
  bool m_lastResult;

 public:
  SCA_RBConstraintSensor(SCA_EventManager *eventmgr,
                         SCA_IObject *gameobj,
                         const std::string &targetName);
  virtual ~SCA_RBConstraintSensor();

  virtual EXP_Value *GetReplica();
  virtual void Init();
  virtual bool Evaluate();
  virtual bool IsPositiveTrigger();
};
