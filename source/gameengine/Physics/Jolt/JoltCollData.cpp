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

/** \file JoltCollData.cpp
 *  \ingroup physjolt
 */

#include "JoltCollData.h"

JoltCollData::JoltCollData()
{
}

JoltCollData::~JoltCollData()
{
}

void JoltCollData::AddContactPoint(const MT_Vector3 &localA,
                                    const MT_Vector3 &localB,
                                    const MT_Vector3 &worldPt,
                                    const MT_Vector3 &normal,
                                    float friction,
                                    float restitution)
{
  ContactPoint cp;
  cp.localPointA = localA;
  cp.localPointB = localB;
  cp.worldPoint = worldPt;
  cp.normal = normal;
  cp.combinedFriction = friction;
  cp.combinedRestitution = restitution;
  cp.appliedImpulse = 0.0f;
  m_contacts.push_back(cp);
}

unsigned int JoltCollData::GetNumContacts() const
{
  return (unsigned int)m_contacts.size();
}

MT_Vector3 JoltCollData::GetLocalPointA(unsigned int index, bool first) const
{
  if (index >= m_contacts.size()) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  return first ? m_contacts[index].localPointA : m_contacts[index].localPointB;
}

MT_Vector3 JoltCollData::GetLocalPointB(unsigned int index, bool first) const
{
  if (index >= m_contacts.size()) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  return first ? m_contacts[index].localPointB : m_contacts[index].localPointA;
}

MT_Vector3 JoltCollData::GetWorldPoint(unsigned int index, bool first) const
{
  if (index >= m_contacts.size()) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  return m_contacts[index].worldPoint;
}

MT_Vector3 JoltCollData::GetNormal(unsigned int index, bool first) const
{
  if (index >= m_contacts.size()) {
    return MT_Vector3(0.0f, 0.0f, 0.0f);
  }
  /* Flip normal depending on which body is queried. */
  return first ? m_contacts[index].normal : -m_contacts[index].normal;
}

float JoltCollData::GetCombinedFriction(unsigned int index, bool first) const
{
  if (index >= m_contacts.size()) {
    return 0.0f;
  }
  return m_contacts[index].combinedFriction;
}

float JoltCollData::GetCombinedRollingFriction(unsigned int index, bool first) const
{
  /* Jolt has no rolling friction concept. */
  return 0.0f;
}

float JoltCollData::GetCombinedRestitution(unsigned int index, bool first) const
{
  if (index >= m_contacts.size()) {
    return 0.0f;
  }
  return m_contacts[index].combinedRestitution;
}

float JoltCollData::GetAppliedImpulse(unsigned int index, bool first) const
{
  if (index >= m_contacts.size()) {
    return 0.0f;
  }
  return m_contacts[index].appliedImpulse;
}
