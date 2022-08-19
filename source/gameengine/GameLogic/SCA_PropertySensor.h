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

/** \file SCA_PropertySensor.h
 *  \ingroup gamelogic
 *  \brief Property sensor
 */

#pragma once

#include "SCA_ISensor.h"

class SCA_PropertySensor : public SCA_ISensor {
  Py_Header
      // class EXP_Expression*	m_rightexpr;
      int m_checktype;
  std::string m_checkpropval;
  std::string m_checkpropmaxval;
  std::string m_checkpropname;
  std::string m_previoustext;
  bool m_lastresult;
  bool m_recentresult;

 protected:
 public:
  enum KX_PROPSENSOR_TYPE {
    KX_PROPSENSOR_NODEF = 0,
    KX_PROPSENSOR_EQUAL,
    KX_PROPSENSOR_NOTEQUAL,
    KX_PROPSENSOR_INTERVAL,
    KX_PROPSENSOR_CHANGED,
    KX_PROPSENSOR_EXPRESSION,
    KX_PROPSENSOR_LESSTHAN,
    KX_PROPSENSOR_GREATERTHAN,
    KX_PROPSENSOR_MAX
  };

  const std::string S_KX_PROPSENSOR_EQ_STRING;

  SCA_PropertySensor(class SCA_EventManager *eventmgr,
                     SCA_IObject *gameobj,
                     const std::string &propname,
                     const std::string &propval,
                     const std::string &propmaxval,
                     KX_PROPSENSOR_TYPE checktype);

  virtual ~SCA_PropertySensor();
  virtual EXP_Value *GetReplica();
  virtual void Init();
  bool CheckPropertyCondition();

  virtual bool Evaluate();
  virtual bool IsPositiveTrigger();
  virtual EXP_Value *FindIdentifier(const std::string &identifiername);

#ifdef WITH_PYTHON

  /* --------------------------------------------------------------------- */
  /* Python interface ---------------------------------------------------- */
  /* --------------------------------------------------------------------- */

  /**
   * Test whether this is a sensible value (type check)
   */
  static int validValueForProperty(EXP_PyObjectPlus *self, const PyAttributeDef *);

#endif
};
