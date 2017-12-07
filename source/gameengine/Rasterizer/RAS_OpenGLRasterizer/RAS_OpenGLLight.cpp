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
 * Contributor(s): Mitchell Stokes
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#include "GPU_glew.h"
#include "GPU_shader.h"

#include <stdio.h>

#include "RAS_OpenGLLight.h"
#include "RAS_Rasterizer.h"

#include "KX_Light.h"
#include "KX_Camera.h"

#include "BLI_math.h"

extern "C" {
#  include "eevee_private.h"
#  include "DRW_render.h"
}

RAS_OpenGLLight::RAS_OpenGLLight()
{
}

RAS_OpenGLLight::~RAS_OpenGLLight()
{
}

RAS_OpenGLLight *RAS_OpenGLLight::Clone()
{
	return new RAS_OpenGLLight(*this);
}

bool RAS_OpenGLLight::HasShadow() const
{
	return m_hasShadow;
}

bool RAS_OpenGLLight::NeedShadowUpdate()
{
	if (m_staticShadow) {
		return m_requestShadowUpdate;
	}

	return true;
}

int RAS_OpenGLLight::GetShadowBindCode()
{
	return -1;
}

MT_Matrix4x4 RAS_OpenGLLight::GetViewMat()
{
	return MT_Matrix4x4::Identity();
}

MT_Matrix4x4 RAS_OpenGLLight::GetWinMat()
{
	return MT_Matrix4x4::Identity();
}

MT_Matrix4x4 RAS_OpenGLLight::GetShadowMatrix()
{
	return MT_Matrix4x4::Identity();
}

int RAS_OpenGLLight::GetShadowLayer()
{
	return 0;
}

Image *RAS_OpenGLLight::GetTextureImage(short texslot)
{
	return nullptr;
}

