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

/** \file gameengine/Converter/BL_ScalarInterpolator.cpp
 *  \ingroup bgeconv
 */

#include "BL_ScalarInterpolator.h"

#include "ANIM_action.hh"
#include "BKE_fcurve.hh"
#include "DNA_anim_types.h"

using namespace blender::animrig;

BL_ScalarInterpolator::BL_ScalarInterpolator(FCurve *fcu)
    :m_fcu(fcu)
{
}

float BL_ScalarInterpolator::GetValue(float currentTime) const
{
  return evaluate_fcurve(m_fcu, currentTime);
}

FCurve *BL_ScalarInterpolator::GetFCurve() const
{
  return m_fcu;
}

BL_InterpolatorList::BL_InterpolatorList(bAction *action) : m_action(action)
{
  if (!action) {
    return;
  }

  /*for (FCurve *fcu = (FCurve *)action->curves.first; fcu; fcu = fcu->next) {
    if (fcu->rna_path) {
      m_interpolators.emplace_back(fcu);
    }
  }*/

  Action &new_action = action->wrap();

  for (Layer *layer : new_action.layers()) {
    for (Strip *strip : layer->strips()) {
      if (strip->type() != Strip::Type::Keyframe) {
        continue;
      }
      for (Channelbag *bag : strip->data<StripKeyframeData>(new_action).channelbags()) {
        for (FCurve *fcu : bag->fcurves()) {
          m_interpolators.emplace_back(fcu);
        }
      }
    }
  }
}

BL_InterpolatorList::~BL_InterpolatorList()
{
}

bAction *BL_InterpolatorList::GetAction() const
{
  return m_action;
}

BL_ScalarInterpolator *BL_InterpolatorList::GetScalarInterpolator(const std::string& rna_path,
                                                                  int array_index)
{
  for (BL_ScalarInterpolator &interp : m_interpolators) {
    FCurve *fcu = interp.GetFCurve();
    if (array_index == fcu->array_index && rna_path == fcu->rna_path) {
      return &interp;
    }
  }
  return nullptr;
}
