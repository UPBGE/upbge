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

/** \file KX_TimeLogger.h
 *  \ingroup ketsji
 */

#ifndef __KX_TIMELOGGER_H__
#define __KX_TIMELOGGER_H__

#ifdef _MSC_VER
#  pragma warning (disable:4786)  /* suppress stl-MSVC debug info warning */
#endif

#include <deque>

/**
 * Stores and manages time measurements.
 */
class KX_TimeLogger
{
public:
	enum Measurement {
		MAX_MEASUREMENTS = 25
	};

	/// Categories for profiling display.
	enum Category {
		NONE = -1,
		PHYSICS = 0,
		LOGIC,
		ANIMATIONS,
		NETWORK,
		SCENEGRAPH,
		RASTERIZER,
		SERVICES, // time spent in miscelaneous activities
		OVERHEAD, // profile info drawing overhead
		OUTSIDE, // time spent outside main loop
		LATENCY, // time spent waiting on the gpu
		NUM_CATEGORY
	};

	/**
	 * Constructor.
	 */
	KX_TimeLogger();

	/**
	 * Destructor.
	 */
	~KX_TimeLogger();

	/**
	 * Starts logging in current measurement.
	 * \param now	The current time.
	 */
	void StartLog(double now);

	/**
	 * End logging in current measurement.
	 * \param now	The current time.
	 */
	void EndLog(double now);

	/**
	 * Logs time in next measurement.
	 * \param now	The current time.
	 */
	void NextMeasurement(double now);

	/**
	 * Returns average of all but the current measurement.
	 * \return The average of all but the current measurement.
	 */
	double GetAverage() const;

protected:
	/// Storage for the measurements.
	std::deque<double> m_measurements;

	/// Time at start of logging.
	double m_logStart;

	/// State of logging.
	bool m_logging;
};

#endif  // __KX_TIMELOGGER_H__
