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
#include "RAS_DebugDraw.h"

#include "boost/format.hpp"

const std::string profileLabels[KX_TimeLogger::NUM_CATEGORY] = {
	"Physics", // PHYSICS
	"Logic", // LOGIC
	"Animation", // ANIMATIONS
	"Network", // NETWORK
	"Scenegraph", // SCENEGRAPH
	"Rasterizer", // RASTERIZER
	"Services", // SERVICES
	"Overhead", // OVERHEAD
	"Outside", // OUTSIDE
	"GPU Latency" // LATENCY
};

KX_TimeCategoryLogger::KX_TimeCategoryLogger()
	:m_lastCategory(KX_TimeLogger::NONE)
{
	for (double& avg : m_lastAverages) {
		avg = 0.0;
	}
}

KX_TimeCategoryLogger::~KX_TimeCategoryLogger()
{
}

void KX_TimeCategoryLogger::StartLog(KX_TimeLogger::Category tc, double now)
{
	if (m_lastCategory != KX_TimeLogger::NONE) {
		m_loggers[m_lastCategory].EndLog(now);
	}
	m_loggers[tc].StartLog(now);
	m_lastCategory = tc;
}

void KX_TimeCategoryLogger::EndLog(KX_TimeLogger::Category tc, double now)
{
	m_loggers[tc].EndLog(now);
}

void KX_TimeCategoryLogger::EndLog(double now)
{
	m_loggers[m_lastCategory].EndLog(now);
	m_lastCategory = KX_TimeLogger::NONE;
}

void KX_TimeCategoryLogger::NextMeasurement(double now)
{
	for (KX_TimeLogger& logger : m_loggers) {
		logger.NextMeasurement(now);
	}
}

double KX_TimeCategoryLogger::GetAverage(KX_TimeLogger::Category tc)
{
	return m_loggers[tc].GetAverage();
}

double KX_TimeCategoryLogger::GetAverage()
{
	double time = 0.0;

	for (const KX_TimeLogger& logger : m_loggers) {
		time += logger.GetAverage();
	}

	return time;
}

const std::array<double, KX_TimeLogger::NUM_CATEGORY>& KX_TimeCategoryLogger::GetLastAverages() const
{
	return m_lastAverages;
}

void KX_TimeCategoryLogger::RenderCategories(RAS_DebugDraw& debugDraw, double tottime, int xindent, int ysize,
											 int& xcoord, int& ycoord, int profileIndent)
{
	for (unsigned short tc = 0; tc < KX_TimeLogger::NUM_CATEGORY; ++tc) {
		debugDraw.RenderText2d(profileLabels[tc] + ":", mt::vec2(xcoord + xindent, ycoord), mt::one4);

		double time = m_loggers[tc].GetAverage();
		m_lastAverages[tc] = time;

		const std::string debugtxt = (boost::format("%5.2fms | %d%%") % (time*1000.f) % (int)(time/tottime * 100.f)).str();
		debugDraw.RenderText2d(debugtxt, mt::vec2(xcoord + xindent + profileIndent, ycoord), mt::one4);

		const mt::vec2 boxSize(50 * (time / tottime), 10);
		debugDraw.RenderBox2d(mt::vec2(xcoord + (int)(2.2 * profileIndent), ycoord), boxSize, mt::one4);
		ycoord += ysize;
	}
}
