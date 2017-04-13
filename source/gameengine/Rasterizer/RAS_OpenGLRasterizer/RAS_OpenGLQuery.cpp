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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Rasterizer/RAS_OpenGLRasterizer/RAS_OpenGLQuery.cpp
 *  \ingroup bgerastogl
 */

#include "RAS_OpenGLQuery.h"

RAS_OpenGLQuery::RAS_OpenGLQuery(RAS_Query::QueryType type)
{
	static const GLenum targetTable[] = {
		GL_SAMPLES_PASSED, // SAMPLES
		GL_ANY_SAMPLES_PASSED, // ANY_SAMPLES
		GL_ANY_SAMPLES_PASSED_CONSERVATIVE, // ANY_SAMPLES_CONSERVATIVE
		GL_PRIMITIVES_GENERATED, // PRIMITIVES
		GL_TIME_ELAPSED // TIME
	};

	m_target = targetTable[type];

	glGenQueries(1, &m_id);
}

RAS_OpenGLQuery::~RAS_OpenGLQuery()
{
	glDeleteQueries(1, &m_id);
}

void RAS_OpenGLQuery::Begin()
{
	glBeginQuery(m_target, m_id);
}

void RAS_OpenGLQuery::End()
{
	glEndQuery(m_target);
}

bool RAS_OpenGLQuery::Available()
{
	GLint result;
	glGetQueryObjectiv(m_id, GL_QUERY_RESULT_AVAILABLE, &result);
	return result;
}

int RAS_OpenGLQuery::ResultNoWait()
{
	GLint result;
	glGetQueryObjectiv(m_id, GL_QUERY_RESULT_NO_WAIT, &result);
	return result;
}

int RAS_OpenGLQuery::Result()
{
	GLint result;
	glGetQueryObjectiv(m_id, GL_QUERY_RESULT, &result);
	return result;
}
