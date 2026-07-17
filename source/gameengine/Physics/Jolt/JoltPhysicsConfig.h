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
 * Contributor(s): UPBGE Contributors
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file JoltPhysicsConfig.h
 *  \ingroup physjolt
 *  \brief Compile-time configuration shared by all UPBGE Jolt wrapper files.
 */

#pragma once

#ifndef JPH_OBJECT_LAYER_BITS
#  define JPH_OBJECT_LAYER_BITS 32
#elif JPH_OBJECT_LAYER_BITS != 32
#  error "UPBGE Jolt is configured for JPH_OBJECT_LAYER_BITS=32"
#endif
