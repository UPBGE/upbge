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

/** \file SCA_NetworkMessageSensor.h
 *  \ingroup ketsjinet
 *  \brief Ketsji Logic Extension: Network Message Sensor class
 */
#pragma once

#include "SCA_ISensor.h"

class KX_NetworkMessageScene;
class EXP_StringValue;
template<class ItemType> class EXP_ListValue;

class SCA_NetworkMessageSensor : public SCA_ISensor {
  // note: Py_Header MUST BE the first listed here
  Py_Header KX_NetworkMessageScene *m_NetworkScene;

  // The subject we filter on.
  std::string m_subject;

  // The number of messages caught since the last frame.
  int m_frame_message_count;

  bool m_IsUp;

  EXP_ListValue<EXP_StringValue> *m_BodyList;
  EXP_ListValue<EXP_StringValue> *m_SubjectList;

 public:
  SCA_NetworkMessageSensor(SCA_EventManager *eventmgr,            // our eventmanager
                           KX_NetworkMessageScene *NetworkScene,  // our scene
                           SCA_IObject *gameobj,                  // the sensor controlling object
                           const std::string &subject);
  virtual ~SCA_NetworkMessageSensor();

  virtual EXP_Value *GetReplica();
  virtual bool Evaluate();
  virtual bool IsPositiveTrigger();
  virtual void Init();
  void EndFrame();

  virtual void Replace_NetworkScene(KX_NetworkMessageScene *val)
  {
    m_NetworkScene = val;
  };

#ifdef WITH_PYTHON

  /* ------------------------------------------------------------- */
  /* Python interface -------------------------------------------- */
  /* ------------------------------------------------------------- */

  /* attributes */
  static PyObject *pyattr_get_bodies(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
  static PyObject *pyattr_get_subjects(EXP_PyObjectPlus *self_v,
                                       const EXP_PYATTRIBUTE_DEF *attrdef);

#endif /* WITH_PYTHON */
};
