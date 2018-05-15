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
 * The Original Code is: all of this file.
 *
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file RAS_Query.cpp
 *  \ingroup bgerast
 */

#include "RAS_Query.h"
#include "RAS_OpenGLQuery.h"

RAS_Query::RAS_Query()
{
}


RAS_Query::RAS_Query(QueryType type)
	:m_impl(new RAS_OpenGLQuery(type))
{
}

RAS_Query::~RAS_Query() = default;

RAS_Query::RAS_Query(RAS_Query&& other)
	:m_impl(std::move(other.m_impl))
{
}

void RAS_Query::Begin()
{
	m_impl->Begin();
}

void RAS_Query::End()
{
	m_impl->End();
}

bool RAS_Query::Available()
{
	return m_impl->Available();
}

int RAS_Query::ResultNoWait()
{
	return m_impl->ResultNoWait();
}

int RAS_Query::Result()
{
	return m_impl->Result();
}
