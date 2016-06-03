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

#include "STR_String.h"

class RAS_TextUser : public RAS_MeshUser
{
private:
	std::vector<STR_String> m_texts;
	int m_fontid;
	int m_size;
	int m_dpi;
	float m_aspect;
	float m_resolution;
	MT_Vector3 m_offset;
	MT_Vector3 m_spacing;

public:
	RAS_TextUser(void *clientobj);
	virtual ~RAS_TextUser();

	int GetFontId() const;
	int GetSize() const;
	int GetDpi() const;
	float GetAspect() const;
	const MT_Vector3& GetOffset() const;
	const MT_Vector3& GetSpacing() const;
	const std::vector<STR_String>& GetTexts() const;

	void SetFontId(int fontid);
	void SetSize(int size);
	void SetDpi(int dpi);
	void SetAspect(float aspect);
	void SetOffset(const MT_Vector3& offset);
	void SetSpacing(const MT_Vector3& spacing);
	void SetTexts(const std::vector<STR_String>& texts);
};

#endif  // __RAS_TEXT_USER_H__
