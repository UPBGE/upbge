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

/** \file RAS_TextUser.cpp
 *  \ingroup bgerast
 */

#include "RAS_TextUser.h"
#include "RAS_DisplayArrayBucket.h"

RAS_TextUser::RAS_TextUser(void *clientobj, RAS_BoundingBox *boundingBox)
	:RAS_MeshUser(clientobj, boundingBox),
	m_fontid(0),
	m_size(0),
	m_dpi(0),
	m_aspect(0.0f),
	m_offset(mt::zero3),
	m_spacing(mt::zero3)
{
}

RAS_TextUser::~RAS_TextUser()
{
}

int RAS_TextUser::GetFontId() const
{
	return m_fontid;
}

int RAS_TextUser::GetSize() const
{
	return m_size;
}

int RAS_TextUser::GetDpi() const
{
	return m_dpi;
}

float RAS_TextUser::GetAspect() const
{
	return m_aspect;
}

const mt::vec3& RAS_TextUser::GetOffset() const
{
	return m_offset;
}

const mt::vec3& RAS_TextUser::GetSpacing() const
{
	return m_spacing;
}

const std::vector<std::string>& RAS_TextUser::GetTexts() const
{
	return m_texts;
}

void RAS_TextUser::SetFontId(int fontid)
{
	m_fontid = fontid;
}

void RAS_TextUser::SetSize(int size)
{
	m_size = size;
}

void RAS_TextUser::SetDpi(int dpi)
{
	m_dpi = dpi;
}

void RAS_TextUser::SetAspect(float aspect)
{
	m_aspect = aspect;
}

void RAS_TextUser::SetOffset(const mt::vec3& offset)
{
	m_offset = offset;
}

void RAS_TextUser::SetSpacing(const mt::vec3& spacing)
{
	m_spacing = spacing;
}

void RAS_TextUser::SetTexts(const std::vector<std::string>& texts)
{
	m_texts = texts;
}
