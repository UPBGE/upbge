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
 * Contributor(s): lordloki.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file SCA_LedActuator.h
 *  \ingroup GameLogic
 */

#pragma once

#include "SCA_IActuator.h"

/**
 * Sets the light bar or LED of a controller (DualShock 4, DualSense, Switch Pro and others) when
 * activated. One HID write per activation; nothing per tick.
 */
class SCA_LedActuator : public SCA_IActuator {
 Py_Header

     private : int m_joyindex;
  float m_color[3]; /* 0 to 1 */

 public:
  SCA_LedActuator(SCA_IObject *gameobj, int joyindex, const float color[3]);
  virtual ~SCA_LedActuator();

  virtual EXP_Value *GetReplica();

  virtual bool Update();
};
