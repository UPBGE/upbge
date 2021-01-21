/*
 * SCA_2DFilterActuator.h
 *
 *
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
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file SCA_2DFilterActuator.h
 *  \ingroup gamelogic
 */

#pragma once

#include "RAS_Rasterizer.h"
#include "SCA_IActuator.h"
#include "SCA_IScene.h"

class RAS_2DFilterManager;

class SCA_2DFilterActuator : public SCA_IActuator {
  Py_Header

      private : std::vector<std::string>
                    m_propNames;
  int m_type;
  short m_disableMotionBlur;
  float m_float_arg;
  int m_int_arg;
  bool m_mipmap;
  std::string m_shaderText;
  RAS_Rasterizer *m_rasterizer;
  RAS_2DFilterManager *m_filterManager;
  SCA_IScene *m_scene;

 public:
  SCA_2DFilterActuator(class SCA_IObject *gameobj,
                       int type,
                       short flag,
                       float float_arg,
                       int int_arg,
                       bool mipmap,
                       RAS_Rasterizer *rasterizer,
                       RAS_2DFilterManager *filterManager,
                       SCA_IScene *scene);

  void SetShaderText(const std::string &text);
  virtual ~SCA_2DFilterActuator();
  virtual bool Update();

  void SetScene(SCA_IScene *scene, RAS_2DFilterManager *filterManager);

  virtual EXP_Value *GetReplica();
};
