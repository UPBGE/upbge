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

/** \file KX_IpoController.h
 *  \ingroup ketsji
 */

#pragma once

#include "KX_IInterpolator.h"
#include "KX_IpoTransform.h"
#include "SG_Controller.h"
#include "SG_Node.h"

#include "DNA_object_force_types.h"

#define KX_MAX_IPO_CHANNELS 19  // note- [0] is not used

/**
 * The X, Y and Z angles an action keeps for the rotation channels it does not key (UPBGE fork,
 * 2026-11-17). Blender keeps them at the numbers typed in the Rotation fields; the engine took
 * them from the turn (MT_Matrix3x3::getEuler: Y in [-90, 90], Z 0 when pointing straight up or
 * down), another set of numbers for the same turn when the typed Y is beyond +-90 or the turn
 * points straight up or down with both X and Z typed: an action keying only Z then played 180
 * (or 45) degrees off. Now: getEuler's angles where they already are the typed ones (each within
 * 1e-4 of it, whole turns apart counting as the same: the old bits), else the typed ones where
 * they give this turn (an object the game has not turned), else the set giving this turn
 * closest to the typed ones (wrapped to them).
 * \param typed: the object's Rotation fields in XYZ Euler order, or null (another rotation mode:
 * getEuler's angles).
 */
MT_Vector3 KX_IpoStartEuler(const MT_Matrix3x3 &orientation, const float typed[3]);

class KX_IpoController : public SG_Controller {
  KX_IpoTransform m_ipo_xform;
  T_InterpolatorList m_interpolators;

  /** Flag for each IPO channel that can be applied to a game object */
  bool m_ipo_channels_active[KX_MAX_IPO_CHANNELS];

  /** Interpret the ipo as a force rather than a displacement? */
  bool m_ipo_as_force;

  /** Add Ipo curve to current loc/rot/scale */
  bool m_ipo_add;

  /** Ipo must be applied in local coordinate rather than in global coordinates (used for force and
   * Add mode)*/
  bool m_ipo_local;

  /** Were settings altered since the last update? */
  bool m_modified;

  /** Local time of this ipo.*/
  double m_ipotime;

  /** Location of the object when the IPO is first fired (for local transformations) */
  MT_Vector3 m_ipo_start_point;

  /** Orientation of the object when the IPO is first fired (for local transformations) */
  MT_Matrix3x3 m_ipo_start_orient;

  /** Scale of the object when the IPO is first fired (for local transformations) */
  MT_Vector3 m_ipo_start_scale;

  /** if IPO initial position has been set for local normal IPO */
  bool m_ipo_start_initialized;

  /** Euler angles at the start of the game, needed for incomplete ROT Ipo curves */
  MT_Vector3 m_ipo_start_euler;

  /** true is m_ipo_start_euler has been initialized */
  bool m_ipo_euler_initialized;

  /** A reference to the original game object. */
  class KX_GameObject *m_game_object;

  /* Force field animated channels (RNA: Object.field.*). */
  float m_field_strength;
  float m_field_flow;
  float m_field_noise;
  float m_field_falloff_power;
  float m_field_distance_min;
  float m_field_distance_max;

  bool m_field_strength_active;
  bool m_field_flow_active;
  bool m_field_noise_active;
  bool m_field_falloff_power_active;
  bool m_field_distance_min_active;
  bool m_field_distance_max_active;

 public:
  KX_IpoController();

  virtual ~KX_IpoController();

  virtual SG_Controller *GetReplica(class SG_Node *destnode);

  void SetOption(int option, int value);

  /** Set reference to the corresponding game object. */
  void SetGameObject(class KX_GameObject *go);

  void SetIPOChannelActive(int index, bool value)
  {
    // indexes found in makesdna\DNA_ipo_types.h
    m_ipo_channels_active[index] = value;
  }

  KX_IpoTransform &GetIPOTransform()
  {
    return m_ipo_xform;
  }

  /* Field channel helpers */
  void SetFieldInitial(const blender::PartDeflect *pd);
  void SetFieldStrengthActive(bool value);
  void SetFieldFlowActive(bool value);
  void SetFieldNoiseActive(bool value);
  void SetFieldFalloffPowerActive(bool value);
  void SetFieldDistanceMinActive(bool value);
  void SetFieldDistanceMaxActive(bool value);

  float *GetFieldStrengthPtr()
  {
    return &m_field_strength;
  }
  float *GetFieldFlowPtr()
  {
    return &m_field_flow;
  }
  float *GetFieldNoisePtr()
  {
    return &m_field_noise;
  }
  float *GetFieldFalloffPowerPtr()
  {
    return &m_field_falloff_power;
  }
  float *GetFieldDistanceMinPtr()
  {
    return &m_field_distance_min;
  }
  float *GetFieldDistanceMaxPtr()
  {
    return &m_field_distance_max;
  }

  void AddInterpolator(KX_IInterpolator *interp);
  virtual bool Update(double time);
  virtual void SetSimulatedTime(double time)
  {
    m_ipotime = time;
    m_modified = true;
  }
};
