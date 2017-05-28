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

#ifndef __KX_TIMECATEGORYLOGGER_H__
#define __KX_TIMECATEGORYLOGGER_H__

#ifdef _MSC_VER
#  pragma warning (disable:4786)  /* suppress stl-MSVC debug info warning */
#endif

#include "KX_TimeLogger.h"
#include <array>

class RAS_DebugDraw;

/**
 * Stores and manages time measurements by category.
 * Categories can be added dynamically.
 * Average measurements can be established for each separate category
 * or for all categories together.
 */
class KX_TimeCategoryLogger
{
public:
	/**
	 * Constructor.
	 */
	KX_TimeCategoryLogger();

	/**
	 * Destructor.
	 */
	~KX_TimeCategoryLogger();

	/**
	 * Starts logging in current measurement for the given category.
	 * \param tc					The category to log to.
	 * \param now					The current time.
	 */
	void StartLog(KX_TimeLogger::Category tc, double now);

	/**
	 * End logging in current measurement for the given category.
	 * \param tc	The category to log to.
	 * \param now	The current time.
	 */
	void EndLog(KX_TimeLogger::Category tc, double now);

	/**
	 * End logging in current measurement for all categories.
	 * \param now	The current time.
	 */
	void EndLog(double now);

	/**
	 * Logs time in next measurement.
	 * \param now	The current time.
	 */
	void NextMeasurement(double now);

	/**
	 * Returns average of all but the current measurement time.
	 * \return The average of all but the current measurement.
	 */
	double GetAverage(KX_TimeLogger::Category tc);

	/**
	 * Returns average for grand total.
	 */
	double GetAverage();

	/// Return last frame times.
	const std::array<double, KX_TimeLogger::NUM_CATEGORY>& GetLastAverages() const;

	void RenderCategories(RAS_DebugDraw& debugDraw, double tottime, int xindent, int ysize, int& xcoord, int& ycoord, int profileIndent);

protected:
	/// Storage for the loggers.
	KX_TimeLogger m_loggers[KX_TimeLogger::NUM_CATEGORY];

	KX_TimeLogger::Category m_lastCategory;

	std::array<double, KX_TimeLogger::NUM_CATEGORY> m_lastAverages;
};

#endif  /* __KX_TIMECATEGORYLOGGER_H__ */
