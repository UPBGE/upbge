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

/** \file SCA_IScene.h
 *  \ingroup gamelogic
 */

#pragma once

#include <string>
#include <vector>

class SCA_IObject;

struct SCA_DebugProp {
  SCA_IObject *m_obj;
  std::string m_name;

  SCA_DebugProp(SCA_IObject *gameobj, const std::string &name);
  ~SCA_DebugProp();
};

class SCA_IScene {
 private:
  std::vector<SCA_DebugProp> m_debugList;

 public:
  SCA_IScene();
  virtual ~SCA_IScene();

  const std::vector<SCA_DebugProp> &GetDebugProperties() const;
  bool PropertyInDebugList(SCA_IObject *gameobj, const std::string &name);
  bool ObjectInDebugList(SCA_IObject *gameobj);
  void RemoveAllDebugProperties();
  void AddDebugProperty(SCA_IObject *gameobj, const std::string &name);
  void RemoveDebugProperty(SCA_IObject *gameobj, const std::string &name);
  void RemoveObjectDebugProperties(SCA_IObject *gameobj);
};
