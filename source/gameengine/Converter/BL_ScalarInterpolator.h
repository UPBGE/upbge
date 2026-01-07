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

/** \file BL_ScalarInterpolator.h
 *  \ingroup bgeconv
 */

#pragma once

#include <string>
#include <vector>

#include "DNA_action_types.h"

#include "KX_IScalarInterpolator.h"

class BL_ScalarInterpolator : public KX_IScalarInterpolator {
 private:
  blender::FCurve *m_fcu;

 public:
  BL_ScalarInterpolator(blender::FCurve *fcu);
  virtual ~BL_ScalarInterpolator() = default;

  virtual float GetValue(float currentTime) const;
  blender::FCurve *GetFCurve() const;
};

class BL_InterpolatorList {
 private:
  blender::bAction *m_action;
  std::vector<BL_ScalarInterpolator> m_interpolators;

 public:
  BL_InterpolatorList(blender::bAction *action);
  ~BL_InterpolatorList();

  blender::bAction *GetAction() const;

  BL_ScalarInterpolator *GetScalarInterpolator(const std::string& rna_path, int array_index);
};
