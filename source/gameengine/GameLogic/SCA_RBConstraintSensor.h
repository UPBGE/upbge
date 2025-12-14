#pragma once

/** \file SCA_RBConstraintSensor.h
 *  \ingroup gamelogic
 *  \brief Sensor to detect if a rigid body constraint is broken.
 */

#include "SCA_ISensor.h"

class SCA_RBConstraintSensor : public SCA_ISensor {
  Py_Header

  bool m_lastResult;

 public:
  SCA_RBConstraintSensor(SCA_EventManager *eventmgr, SCA_IObject *gameobj);
  virtual ~SCA_RBConstraintSensor();

  virtual EXP_Value *GetReplica();
  virtual void Init();
  virtual bool Evaluate();
  virtual bool IsPositiveTrigger();
};
