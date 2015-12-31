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
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Rasterizer/RAS_OpenGLRasterizer/RAS_ListRasterizer.cpp
 *  \ingroup bgerastogl
 */

#include <iostream>

#include "RAS_ListRasterizer.h"

#ifdef WIN32
#include <windows.h>
#endif // WIN32

#include "glew-mx.h"

#include "RAS_MaterialBucket.h"
#include "RAS_MeshObject.h"
#include "RAS_TexVert.h"
#include "MT_assert.h"

//#if defined(DEBUG)
//#ifdef WIN32
//#define spit(x) std::cout << x << std::endl;
//#endif //WIN32
//#else
#define spit(x)
//#endif

RAS_ListSlot::RAS_ListSlot(RAS_ListRasterizer *rasty)
	:KX_ListSlot(),
	m_list(0),
	m_flag(LIST_MODIFY | LIST_CREATE),
	m_rasty(rasty)
{
}

int RAS_ListSlot::Release()
{
	if (--m_refcount > 0)
		return m_refcount;
	m_rasty->RemoveListSlot(this);
	delete this;
	return 0;
}

RAS_ListSlot::~RAS_ListSlot()
{
	RemoveList();
}

void RAS_ListSlot::RemoveList()
{
	if (m_list != 0) {
		spit("Releasing display list (" << m_list << ")");
		glDeleteLists((GLuint)m_list, 1);
		m_list = 0;
	}
}

void RAS_ListSlot::DrawList()
{
	if (m_flag & LIST_MODIFY) {
		if (m_flag & LIST_CREATE) {
			if (m_list == 0) {
				m_list = (unsigned int)glGenLists(1);
				m_flag =  m_flag & ~LIST_CREATE;
				spit("Created display list (" << m_list << ")");
			}
		}
		if (m_list != 0)
			glNewList((GLuint)m_list, GL_COMPILE);

		m_flag |= LIST_BEGIN;
		return;
	}
	glCallList(m_list);
}

void RAS_ListSlot::EndList()
{
	if (m_flag & LIST_BEGIN) {
		glEndList();
		m_flag = m_flag & ~(LIST_BEGIN | LIST_MODIFY);
		m_flag |= LIST_END;
		glCallList(m_list);
	}
}

void RAS_ListSlot::SetModified(bool mod)
{
	if (mod && !(m_flag & LIST_MODIFY)) {
		spit("Modifying list (" << m_list << ")");
		m_flag = m_flag & ~LIST_END;
		m_flag |= LIST_MODIFY;
	}
}

bool RAS_ListSlot::End()
{
	return (m_flag & LIST_END) != 0;
}

RAS_ListRasterizer::RAS_ListRasterizer(RAS_ICanvas* canvas, bool lock, RAS_STORAGE_TYPE storage)
	:RAS_OpenGLRasterizer(canvas, storage)
{
}

RAS_ListRasterizer::~RAS_ListRasterizer()
{
	ReleaseAlloc();
}

void RAS_ListRasterizer::RemoveListSlot(RAS_ListSlot *list)
{
	RAS_ArrayLists::iterator it = mArrayLists.begin();
	while (it != mArrayLists.end()) {
		if (it->second == list) {
			mArrayLists.erase(it);
			break;
		}
		it++;
	}
}

RAS_ListSlot *RAS_ListRasterizer::FindOrAdd(RAS_MeshSlot *ms)
{
	/*
	 * Keep a copy of constant lists submitted for rendering,
	 * this guards against (replicated)new...delete every frame,
	 * and we can reuse lists!
	 * :: sorted by mesh slot
	 */
	RAS_ListSlot *localSlot = (RAS_ListSlot *)ms->m_DisplayList;
	if (!localSlot) {
		RAS_ArrayLists::iterator it = mArrayLists.find(ms->m_displayArray);
		if (it == mArrayLists.end()) {
			localSlot = new RAS_ListSlot(this);
			mArrayLists.insert(std::pair<RAS_DisplayArray *, RAS_ListSlot *>(ms->m_displayArray, localSlot));
		}
		else {
			localSlot = static_cast<RAS_ListSlot *>(it->second->AddRef());
		}
	}
	MT_assert(localSlot);
	return localSlot;
}

void RAS_ListRasterizer::ReleaseAlloc()
{
	for (RAS_ArrayLists::iterator it = mArrayLists.begin(); it != mArrayLists.end(); ++it)
		delete it->second;
	mArrayLists.clear();
}

void RAS_ListRasterizer::BindPrimitives(RAS_DisplayArray *array)
{
	// Set all vertex array attributs outside the display list is slower than recall it for each display list.
}

void RAS_ListRasterizer::UnbindPrimitives(RAS_DisplayArray *array)
{
	// Set all vertex array attributs outside the display list is slower than recall it for each display list.
}

void RAS_ListRasterizer::IndexPrimitives(RAS_MeshSlot *ms)
{
	RAS_ListSlot *localSlot = NULL;

	if (ms->m_bDisplayList) {
		localSlot = FindOrAdd(ms);
		localSlot->DrawList();

		if (localSlot->End()) {
			// save slot here too, needed for replicas and object using same mesh
			// => they have the same vertexarray but different mesh slot
			ms->m_DisplayList = localSlot;
			return;
		}
	}

	RAS_OpenGLRasterizer::BindPrimitives(ms->GetDisplayArray());
	RAS_OpenGLRasterizer::IndexPrimitives(ms);
	RAS_OpenGLRasterizer::UnbindPrimitives(ms->GetDisplayArray());

	if (ms->m_bDisplayList) {
		localSlot->EndList();
		ms->m_DisplayList = localSlot;
	}
}

bool RAS_ListRasterizer::Init(void)
{
	return RAS_OpenGLRasterizer::Init();
}

void RAS_ListRasterizer::SetDrawingMode(int drawingmode)
{
	RAS_OpenGLRasterizer::SetDrawingMode(drawingmode);
}

void RAS_ListRasterizer::Exit()
{
	RAS_OpenGLRasterizer::Exit();
}

