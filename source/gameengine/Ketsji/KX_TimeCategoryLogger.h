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

/** \file KX_TimeCategoryLogger.h
 *  \ingroup ketsji
 */

#pragma once

#ifdef _MSC_VER
#  pragma warning(disable : 4786) /* suppress stl-MSVC debug info warning */
#endif

#include <map>

#include "CM_Clock.h"
#include "KX_TimeLogger.h"

/**
 * Stores and manages time measurements by category.
 * Categories can be added dynamically.
 * Average measurements can be established for each separate category
 * or for all categories together.
 */
class KX_TimeCategoryLogger {
 public:
  typedef int TimeCategory;
  typedef std::map<TimeCategory, KX_TimeLogger> TimeLoggerMap;

  /**
   * Constructor.
   * \param maxNumMesasurements Maximum number of measurements stored (> 1).
   */
  KX_TimeCategoryLogger(const CM_Clock &clock, unsigned int maxNumMeasurements = 10);

  /**
   * Destructor.
   */
  ~KX_TimeCategoryLogger();

  /**
   * Changes the maximum number of measurements that can be stored.
   */
  void SetMaxNumMeasurements(unsigned int maxNumMeasurements);

  /**
   * Changes the maximum number of measurements that can be stored.
   */
  unsigned int GetMaxNumMeasurements() const;

  /**
   * Adds a category.
   * \param category	The new category.
   */
  void AddCategory(TimeCategory tc);

  /**
   * Starts logging in current measurement for the given category.
   * \param tc					The category to log to.
   * \param now					The current time.
   */
  void StartLog(TimeCategory tc);

  /**
   * End logging in current measurement for the given category.
   * \param tc	The category to log to.
   */
  void EndLog(TimeCategory tc);

  /**
   * End logging in current measurement for all categories.
   */
  void EndLog();

  /**
   * Logs time in next measurement.
   * \param now	The current time.
   */
  void NextMeasurement();

  /**
   * Returns average of all but the current measurement time.
   * \return The average of all but the current measurement.
   */
  double GetAverage(TimeCategory tc);

  /**
   * Returns average for grand total.
   */
  double GetAverage();

 protected:
  const CM_Clock &m_clock;
  /// Storage for the loggers.
  TimeLoggerMap m_loggers;
  /// Maximum number of measurements.
  unsigned int m_maxNumMeasurements;

  TimeCategory m_lastCategory;
};
