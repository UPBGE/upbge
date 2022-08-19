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

/** \file gameengine/Ketsji/KX_TimeCategoryLogger.cpp
 *  \ingroup ketsji
 */

#include "KX_TimeCategoryLogger.h"

KX_TimeCategoryLogger::KX_TimeCategoryLogger(const CM_Clock &clock,
                                             unsigned int maxNumMeasurements)

    : m_clock(clock), m_maxNumMeasurements(maxNumMeasurements), m_lastCategory(-1)
{
}

KX_TimeCategoryLogger::~KX_TimeCategoryLogger()
{
}

void KX_TimeCategoryLogger::SetMaxNumMeasurements(unsigned int maxNumMeasurements)
{
  for (TimeLoggerMap::value_type &pair : m_loggers) {
    pair.second.SetMaxNumMeasurements(maxNumMeasurements);
  }
  m_maxNumMeasurements = maxNumMeasurements;
}

unsigned int KX_TimeCategoryLogger::GetMaxNumMeasurements() const
{
  return m_maxNumMeasurements;
}

void KX_TimeCategoryLogger::AddCategory(TimeCategory tc)
{
  // Only add if not already present
  if (m_loggers.find(tc) == m_loggers.end()) {
    m_loggers.emplace(TimeLoggerMap::value_type(tc, KX_TimeLogger(m_maxNumMeasurements)));
  }
}

void KX_TimeCategoryLogger::StartLog(TimeCategory tc)
{
  const double now = m_clock.GetTimeSecond();
  if (m_lastCategory != -1) {
    m_loggers[m_lastCategory].EndLog(now);
  }
  m_loggers[tc].StartLog(now);
  m_lastCategory = tc;
}

void KX_TimeCategoryLogger::EndLog(TimeCategory tc)
{
  const double now = m_clock.GetTimeSecond();
  m_loggers[tc].EndLog(now);
}

void KX_TimeCategoryLogger::EndLog()
{
  const double now = m_clock.GetTimeSecond();
  m_loggers[m_lastCategory].EndLog(now);
  m_lastCategory = -1;
}

void KX_TimeCategoryLogger::NextMeasurement()
{
  const double now = m_clock.GetTimeSecond();
  for (TimeLoggerMap::value_type &pair : m_loggers) {
    pair.second.NextMeasurement(now);
  }
}

double KX_TimeCategoryLogger::GetAverage(TimeCategory tc)
{
  return m_loggers[tc].GetAverage();
}

double KX_TimeCategoryLogger::GetAverage()
{
  double time = 0.0;

  for (TimeLoggerMap::value_type &pair : m_loggers) {
    time += pair.second.GetAverage();
  }

  return time;
}
