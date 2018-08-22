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

/** \file BL_IpoConvert.h
 *  \ingroup bgeconv
 */

#ifndef __KX_IPOCONVERT_H__
#define __KX_IPOCONVERT_H__

class SG_Controller;
class BL_ActionData;
class KX_GameObject;
class KX_Scene;
class BL_Material;

SG_Controller *BL_CreateIPO(BL_ActionData *action,
	KX_GameObject* gameobj,
	KX_Scene *scene);

SG_Controller *BL_CreateObColorIPO(BL_ActionData *action,
	KX_GameObject* gameobj,
	KX_Scene *scene);

SG_Controller *BL_CreateLampIPO(BL_ActionData *action,
	KX_GameObject* lightobj,
	KX_Scene *scene);

SG_Controller *BL_CreateWorldIPO(BL_ActionData *action,
	struct World *blenderworld,
	KX_Scene *scene);

SG_Controller *BL_CreateCameraIPO(BL_ActionData *action,
	KX_GameObject* cameraobj,
	KX_Scene *scene);

SG_Controller *BL_CreateMaterialIpo(
	BL_ActionData *action,
	BL_Material *mat,
	KX_GameObject* gameobj,
	KX_Scene *scene);


#endif  /* __KX_IPOCONVERT_H__ */
