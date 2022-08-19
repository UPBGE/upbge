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

/** \file SCA_NetworkMessageActuator.h
 *  \ingroup ketsjinet
 *  \brief Ketsji Logic Extension: Network Message Actuator class
 */

#pragma once

#include "SCA_IActuator.h"

#include <string>

class SCA_NetworkMessageActuator : public SCA_IActuator {
  Py_Header bool m_lastEvent;
  class KX_NetworkMessageScene *m_networkscene;  // needed for replication
  std::string m_toPropName;
  std::string m_subject;
  bool m_bPropBody;
  std::string m_body;

 public:
  SCA_NetworkMessageActuator(SCA_IObject *gameobj,
                             KX_NetworkMessageScene *networkscene,
                             const std::string &toPropName,
                             const std::string &subject,
                             int bodyType,
                             const std::string &body);
  virtual ~SCA_NetworkMessageActuator();

  virtual bool Update();
  virtual EXP_Value *GetReplica();
  virtual void Replace_NetworkScene(KX_NetworkMessageScene *val)
  {
    m_networkscene = val;
  };
};
