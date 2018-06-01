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

/** \file gameengine/Ketsji/KX_CameraIpoSGController.cpp
 *  \ingroup ketsji
 */


#include "KX_CameraIpoSGController.h"
#include "KX_Camera.h"
#include "RAS_CameraData.h"

bool KX_CameraIpoSGController::Update(SG_Node *node)
{
	if (!SG_Controller::Update(node)) {
		return false;
	}

	KX_Camera *kxcamera = static_cast<KX_Camera *>(node->GetObject());
	RAS_CameraData *camdata = kxcamera->GetCameraData();

	if (m_modify_lens) {
		camdata->m_lens = m_lens;
	}

	if (m_modify_clipstart) {
		camdata->m_clipstart = m_clipstart;
	}

	if (m_modify_clipend) {
		camdata->m_clipend = m_clipend;
	}

	if (m_modify_lens || m_modify_clipstart || m_modify_clipend) {
		kxcamera->InvalidateProjectionMatrix();
	}

	return true;
}
