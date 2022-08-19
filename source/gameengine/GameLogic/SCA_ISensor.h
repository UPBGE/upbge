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

/** \file SCA_ISensor.h
 *  \ingroup gamelogic
 *  \brief Interface Class for all logic Sensors. Implements
 *   pulsemode and pulsefrequency, and event suppression.
 */

#pragma once

#include "SCA_IController.h"

class SCA_EventManager;

/**
 * Interface Class for all logic Sensors. Implements
 * pulsemode,pulsefrequency
 * Use of SG_DList element: not used
 * Use of SG_QList element: not used
 */
class SCA_ISensor : public SCA_ILogicBrick {
  Py_Header

      protected : SCA_EventManager *m_eventmgr;

  /// Pulse positive  pulses?
  bool m_pos_pulsemode;

  /// Pulse negative pulses?
  bool m_neg_pulsemode;

  /// Number of skipped ticks between two active pulses.
  int m_skipped_ticks;

  /// Number of ticks since the last positive pulse.
  int m_pos_ticks;

  /// Number of ticks since the last negative pulse.
  int m_neg_ticks;

  /// Invert the output signal.
  bool m_invert;

  /// Detect level instead of edge.
  bool m_level;

  /// Tap mode.
  bool m_tap;

  /// Sensor has been reset.
  bool m_reset;

  /// Sensor must ignore updates?
  bool m_suspended;

  /// Number of connections to controller.
  int m_links;

  /// Current sensor state.
  bool m_state;

  /// Previous state (for tap option).
  bool m_prev_state;

  std::vector<SCA_IController *> m_linkedcontrollers;

 public:
  enum sensortype {
    ST_NONE = 0,
    ST_TOUCH,
    ST_NEAR,
    ST_RADAR,
    // to be updated as needed
  };

  SCA_ISensor(SCA_IObject *gameobj, SCA_EventManager *eventmgr);
  virtual ~SCA_ISensor();

  virtual void ReParent(SCA_IObject *parent);

  /** Because we want sensors to share some behavior, the Activate has     */
  /* an implementation on this level. It requires an evaluate on the lower */
  /* level of individual sensors. Mapping the old activate()s is easy.     */
  /* The IsPosTrig() also has to change, to keep things consistent.        */
  void Activate(SCA_LogicManager *logicmgr);
  virtual bool Evaluate() = 0;
  virtual bool IsPositiveTrigger();
  virtual void Init();

  virtual EXP_Value *GetReplica() = 0;

  /** Set parameters for the pulsing behavior.
   * \param posmode Trigger positive pulses?
   * \param negmode Trigger negative pulses?
   * \param freq    Frequency to use when doing pulsing.
   */
  void SetPulseMode(bool posmode, bool negmode, int skippedticks);

  /** Set inversion of pulses on or off. */
  void SetInvert(bool inv);
  /** set the level detection on or off */
  void SetLevel(bool lvl);
  void SetTap(bool tap);

  virtual void RegisterToManager();
  virtual void UnregisterToManager();
  void Replace_EventManager(SCA_LogicManager *logicmgr);
  void LinkToController(SCA_IController *controller);
  void UnlinkController(SCA_IController *controller);
  void UnlinkAllControllers();
  void ActivateControllers(SCA_LogicManager *logicmgr);

  virtual void ProcessReplica();

  virtual double GetNumber();

  virtual sensortype GetSensorType();

  /// Stop sensing for a while.
  void Suspend();

  /// Is this sensor switched off?
  bool IsSuspended();

  /// Get the state of the sensor: positive or negative.
  bool GetState();

  /// Get the previous state of the sensor: positive or negative.
  bool GetPrevState();

  /// Get the number of ticks since the last positive pulse.
  int GetPosTicks();

  /// Get the number of ticks since the last negative pulse.
  int GetNegTicks();

  /// Resume sensing.
  void Resume();

  void ClrLink();
  void IncLink();
  void DecLink();
  bool IsNoLink() const;

#ifdef WITH_PYTHON
  EXP_PYMETHOD_DOC_NOARGS(SCA_ISensor, reset);

  static PyObject *pyattr_get_triggered(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_positive(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_status(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_posTicks(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_negTicks(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_frequency(EXP_PyObjectPlus *self_v,
                                        const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_frequency(EXP_PyObjectPlus *self_v,
                                  const struct EXP_PYATTRIBUTE_DEF *attrdef,
                                  PyObject *value);

  static int pyattr_check_level(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_check_tap(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);

  enum SensorStatus {
    KX_SENSOR_INACTIVE = 0,
    KX_SENSOR_JUST_ACTIVATED,
    KX_SENSOR_ACTIVE,
    KX_SENSOR_JUST_DEACTIVATED

  };
#endif  // WITH_PYTHON
};
