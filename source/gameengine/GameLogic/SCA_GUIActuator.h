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
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file SCA_GUIActuator.h
 *  \ingroup gamelogic
 *  \brief actuator for GUI Mouse
 */

#ifndef __SCA_GUIActuator_H__
#define __SCA_GUIActuator_H__

#include "SCA_IActuator.h"

class SCA_GUIActuator : public SCA_IActuator
{
  Py_Header;

  int m_mode;
  bool m_cursorDefault; // need to set default cursor

  /** The current scene. */
  class KX_Scene *m_scene;
  class KX_KetsjiEngine *m_KetsjiEngine;

  /** The scene to switch to. */
  std::string m_themeName;
  std::string m_cursorName;
  std::string m_layoutName;
  std::string m_prefix;

 public:
  enum SCA_GUIActuatorMode {
    KX_GUI_NODEF = 0,
    KX_GUI_LAYOUT_ADD,
    KX_GUI_LAYOUT_REMOVE,
    KX_GUI_MOUSE_CHANGE,
    KX_GUI_MOUSE_HIDE,
    KX_GUI_MOUSE_SHOW,
    KX_GUI_SCHEME_LOAD,
    KX_GUI_MAX
  };

  SCA_GUIActuator(SCA_IObject *gameobj,
                  int mode,
                  const std::string themename,
                  const std::string cursorname,
                  const std::string layoutname,
                  const std::string prefix,
                  bool cursordefault,
                  KX_Scene *scene,
                  KX_KetsjiEngine *ketsjiEngine);

  virtual ~SCA_GUIActuator();

  virtual CValue *GetReplica();

  virtual bool Update();

  /* --------------------------------------------------------------------- */
  /* Python interface ---------------------------------------------------- */
  /* --------------------------------------------------------------------- */

};

#endif // __SCA_GUIActuator_H__
