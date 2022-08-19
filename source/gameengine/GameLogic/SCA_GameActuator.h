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

/** \file SCA_GameActuator.h
 *  \ingroup ketsji
 *  \brief actuator for global game stuff
 */

#pragma once

#include "KX_Scene.h" /* Replace_IScene only */
#include "SCA_IActuator.h"
#include "SCA_IScene.h" /* Replace_IScene only */

class SCA_GameActuator : public SCA_IActuator {
  Py_Header protected : int m_mode;
  bool m_restart;
  std::string m_filename;
  std::string m_loadinganimationname;
  class SCA_IScene *m_scene;
  class KX_KetsjiEngine *m_ketsjiengine;

 public:
  enum SCA_GameActuatorMode {
    KX_GAME_NODEF = 0,
    KX_GAME_LOAD,
    KX_GAME_START,
    KX_GAME_RESTART,
    KX_GAME_QUIT,
    KX_GAME_SAVECFG,
    KX_GAME_LOADCFG,
    KX_GAME_SCREENSHOT,
    KX_GAME_MAX

  };

  SCA_GameActuator(SCA_IObject *gameobj,
                   int mode,
                   const std::string &filename,
                   const std::string &loadinganimationname,
                   SCA_IScene *scene,
                   KX_KetsjiEngine *ketsjiEngine);
  virtual ~SCA_GameActuator();

  virtual EXP_Value *GetReplica();

  virtual bool Update();

  virtual void Replace_IScene(SCA_IScene *val)
  {
    m_scene = val;
  };

  /* --------------------------------------------------------------------- */
  /* Python interface ---------------------------------------------------- */
  /* --------------------------------------------------------------------- */

}; /* end of class SCA_GameActuator */
