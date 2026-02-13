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
 * Contributor(s): UPBGE Contributors
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file JoltCollData.h
 *  \ingroup physjolt
 *  \brief PHY_ICollData implementation storing Jolt contact manifold data.
 */

#pragma once

#include "PHY_DynamicTypes.h"

#include <vector>

/**
 * Stores contact point data from Jolt ContactListener for use by UPBGE collision callbacks.
 * Thread-safety: instances are created per-event, not shared across threads.
 */
class JoltCollData : public PHY_ICollData {
 public:
  struct ContactPoint {
    MT_Vector3 localPointA;
    MT_Vector3 localPointB;
    MT_Vector3 worldPoint;
    MT_Vector3 normal;
    float combinedFriction;
    float combinedRestitution;
    float appliedImpulse;
  };

  JoltCollData();
  virtual ~JoltCollData();

  void AddContactPoint(const MT_Vector3 &localA,
                        const MT_Vector3 &localB,
                        const MT_Vector3 &worldPt,
                        const MT_Vector3 &normal,
                        float friction,
                        float restitution);

  /* ---- PHY_ICollData interface ---- */

  virtual unsigned int GetNumContacts() const override;
  virtual MT_Vector3 GetLocalPointA(unsigned int index, bool first) const override;
  virtual MT_Vector3 GetLocalPointB(unsigned int index, bool first) const override;
  virtual MT_Vector3 GetWorldPoint(unsigned int index, bool first) const override;
  virtual MT_Vector3 GetNormal(unsigned int index, bool first) const override;
  virtual float GetCombinedFriction(unsigned int index, bool first) const override;
  virtual float GetCombinedRollingFriction(unsigned int index, bool first) const override;
  virtual float GetCombinedRestitution(unsigned int index, bool first) const override;
  virtual float GetAppliedImpulse(unsigned int index, bool first) const override;

 private:
  std::vector<ContactPoint> m_contacts;
};
