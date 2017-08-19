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
 * Contributor(s): Porteries Tristan.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file RAS_TextUser.h
 *  \ingroup bgerast
 */

#ifndef __RAS_TEXT_USER_H__
#define __RAS_TEXT_USER_H__

#include "RAS_MeshUser.h"

#include <string>

class RAS_TextUser : public RAS_MeshUser
{
private:
	std::vector<std::string> m_texts;
	int m_fontid;
	int m_size;
	int m_dpi;
	float m_aspect;
	float m_resolution;
	mt::vec3 m_offset;
	mt::vec3 m_spacing;

public:
	RAS_TextUser(void *clientobj, RAS_BoundingBox *boundingBox);
	virtual ~RAS_TextUser();

	int GetFontId() const;
	int GetSize() const;
	int GetDpi() const;
	float GetAspect() const;
	const mt::vec3& GetOffset() const;
	const mt::vec3& GetSpacing() const;
	const std::vector<std::string>& GetTexts() const;

	void SetFontId(int fontid);
	void SetSize(int size);
	void SetDpi(int dpi);
	void SetAspect(float aspect);
	void SetOffset(const mt::vec3& offset);
	void SetSpacing(const mt::vec3& spacing);
	void SetTexts(const std::vector<std::string>& texts);
};

#endif  // __RAS_TEXT_USER_H__
