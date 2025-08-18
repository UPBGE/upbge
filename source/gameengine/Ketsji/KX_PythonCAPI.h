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

/** \file gameengine/Ketsji/KX_PythonCAPI.h
 *  \ingroup ketsji
 */

#pragma once

#ifdef WITH_PYTHON_C_API

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize enhanced Python C API integration for UPBGE
 * This provides optimized Python bindings with TBB parallel processing
 * and Manifold geometry operations support
 */
int KX_PythonCAPI_Init(void);

/**
 * Finalize and cleanup enhanced Python C API
 */
void KX_PythonCAPI_Finalize(void);

#ifdef __cplusplus
}
#endif

#endif  // WITH_PYTHON_C_API