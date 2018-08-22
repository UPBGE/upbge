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

/** \file gameengine/Converter/BL_IpoConvert.cpp
 *  \ingroup bgeconv
 */

#ifdef _MSC_VER
/* don't show stl-warnings */
#  pragma warning (disable:4786)
#endif

#include "BKE_material.h" /* give_current_material */

#include "KX_GameObject.h"
#include "BL_IpoConvert.h"
#include "SG_Interpolator.h"

#include "BL_ActionData.h"
#include "BL_Converter.h"
#include "KX_Globals.h"
#include "BL_Material.h"

#include "DNA_object_types.h"
#include "DNA_action_types.h"
#include "DNA_anim_types.h"
#include "DNA_ipo_types.h"
#include "DNA_lamp_types.h"
#include "DNA_world_types.h"
#include "DNA_camera_types.h"
#include "DNA_material_types.h"
/* end of blender include block */

#include "KX_IpoController.h"
#include "KX_LightIpoSGController.h"
#include "KX_CameraIpoSGController.h"
#include "KX_WorldIpoController.h"
#include "KX_ObColorIpoSGController.h"
#include "KX_MaterialIpoController.h"

#include "SG_Node.h"
#include "SG_Interpolator.h"

SG_Controller *BL_CreateIPO(BL_ActionData *action, KX_GameObject *gameobj, KX_Scene *scene)
{
	KX_IpoController *ipocontr = new KX_IpoController();

	Object *blenderobject = gameobj->GetBlenderObject();

	ipocontr->GetIPOTransform().SetPosition(mt::vec3(blenderobject->loc));
	ipocontr->GetIPOTransform().SetEulerAngles(mt::vec3(blenderobject->rot));
	ipocontr->GetIPOTransform().SetScaling(mt::vec3(blenderobject->size));

	const char *rotmode, *drotmode;

	switch (blenderobject->rotmode) {
		case ROT_MODE_AXISANGLE:
		{
			rotmode = "rotation_axis_angle";
			drotmode = "delta_rotation_axis_angle";
			break;
		}
		case ROT_MODE_QUAT: /* XXX, this isn't working, currently only eulers are supported [#28853] */
		{rotmode = "rotation_quaternion";
		 drotmode = "delta_rotation_quaternion";
		 break;}
		default:
		{
			rotmode = "rotation_euler";
			drotmode = "delta_rotation_euler";
			break;
		}
	}

	// For each active channel in the action add an
	// interpolator to the game object.

	BL_ScalarInterpolator *interp;

	for (int i = 0; i < 3; i++) {
		if ((interp = action->GetScalarInterpolator("location", i))) {
			SG_Interpolator interpolator(&(ipocontr->GetIPOTransform().GetPosition()[i]), interp);
			ipocontr->AddInterpolator(interpolator);
			ipocontr->SetIPOChannelActive(OB_LOC_X + i, true);
		}
	}
	for (int i = 0; i < 3; i++) {
		if ((interp = action->GetScalarInterpolator("delta_location", i))) {
			SG_Interpolator interpolator(&(ipocontr->GetIPOTransform().GetDeltaPosition()[i]), interp);
			ipocontr->AddInterpolator(interpolator);
			ipocontr->SetIPOChannelActive(OB_DLOC_X + i, true);
		}
	}
	for (int i = 0; i < 3; i++) {
		if ((interp = action->GetScalarInterpolator(rotmode, i))) {
			SG_Interpolator interpolator(&(ipocontr->GetIPOTransform().GetEulerAngles()[i]), interp);
			ipocontr->AddInterpolator(interpolator);
			ipocontr->SetIPOChannelActive(OB_ROT_X + i, true);
		}
	}
	for (int i = 0; i < 3; i++) {
		if ((interp = action->GetScalarInterpolator(drotmode, i))) {
			SG_Interpolator interpolator(&(ipocontr->GetIPOTransform().GetDeltaEulerAngles()[i]), interp);
			ipocontr->AddInterpolator(interpolator);
			ipocontr->SetIPOChannelActive(OB_DROT_X + i, true);
		}
	}
	for (int i = 0; i < 3; i++) {
		if ((interp = action->GetScalarInterpolator("scale", i))) {
			SG_Interpolator interpolator(&(ipocontr->GetIPOTransform().GetScaling()[i]), interp);
			ipocontr->AddInterpolator(interpolator);
			ipocontr->SetIPOChannelActive(OB_SIZE_X + i, true);
		}
	}
	for (int i = 0; i < 3; i++) {
		if ((interp = action->GetScalarInterpolator("delta_scale", i))) {
			SG_Interpolator interpolator(&(ipocontr->GetIPOTransform().GetDeltaScaling()[i]), interp);
			ipocontr->AddInterpolator(interpolator);
			ipocontr->SetIPOChannelActive(OB_DSIZE_X + i, true);
		}
	}


	return ipocontr;
}


SG_Controller *BL_CreateObColorIPO(BL_ActionData *action, KX_GameObject *gameobj, KX_Scene *scene)
{
	KX_ObColorIpoSGController *ipocontr_obcol = nullptr;
	BL_ScalarInterpolator *interp;

	for (int i = 0; i < 4; i++) {
		if ((interp = action->GetScalarInterpolator("color", i))) {
			if (!ipocontr_obcol) {
				ipocontr_obcol = new KX_ObColorIpoSGController();
			}
			SG_Interpolator interpolator(&ipocontr_obcol->m_rgba[i], interp);
			ipocontr_obcol->AddInterpolator(interpolator);
		}
	}

	return ipocontr_obcol;
}

SG_Controller *BL_CreateLampIPO(BL_ActionData *action, KX_GameObject *lightobj, KX_Scene *scene)
{
	KX_LightIpoSGController *ipocontr = new KX_LightIpoSGController();

	Lamp *blenderlamp = (Lamp *)lightobj->GetBlenderObject()->data;

	ipocontr->m_energy = blenderlamp->energy;
	ipocontr->m_col_rgb[0] = blenderlamp->r;
	ipocontr->m_col_rgb[1] = blenderlamp->g;
	ipocontr->m_col_rgb[2] = blenderlamp->b;
	ipocontr->m_dist = blenderlamp->dist;

	// For each active channel in the action add an
	// interpolator to the game object.

	BL_ScalarInterpolator *interp;

	if ((interp = action->GetScalarInterpolator("energy", 0))) {
		SG_Interpolator interpolator(&ipocontr->m_energy, interp);
		ipocontr->AddInterpolator(interpolator);
		ipocontr->SetModifyEnergy(true);
	}

	if ((interp = action->GetScalarInterpolator("distance", 0))) {
		SG_Interpolator interpolator(&ipocontr->m_dist, interp);
		ipocontr->AddInterpolator(interpolator);
		ipocontr->SetModifyDist(true);
	}

	for (int i = 0; i < 3; i++) {
		if ((interp = action->GetScalarInterpolator("color", i))) {
			SG_Interpolator interpolator(&ipocontr->m_col_rgb[i], interp);
			ipocontr->AddInterpolator(interpolator);
			ipocontr->SetModifyColor(true);
		}
	}

	return ipocontr;
}

SG_Controller *BL_CreateCameraIPO(BL_ActionData *action, KX_GameObject *cameraobj, KX_Scene *scene)
{
	KX_CameraIpoSGController *ipocontr = new KX_CameraIpoSGController();

	Camera *blendercamera = (Camera *)cameraobj->GetBlenderObject()->data;

	ipocontr->m_lens = blendercamera->lens;
	ipocontr->m_clipstart = blendercamera->clipsta;
	ipocontr->m_clipend = blendercamera->clipend;

	// For each active channel in the action add an
	// interpolator to the game object.

	BL_ScalarInterpolator *interp;

	if ((interp = action->GetScalarInterpolator("lens", 0))) {
		SG_Interpolator interpolator(&ipocontr->m_lens, interp);
		ipocontr->AddInterpolator(interpolator);
		ipocontr->SetModifyLens(true);
	}

	if ((interp = action->GetScalarInterpolator("clip_start", 0))) {
		SG_Interpolator interpolator(&ipocontr->m_clipstart, interp);
		ipocontr->AddInterpolator(interpolator);
		ipocontr->SetModifyClipStart(true);
	}

	if ((interp = action->GetScalarInterpolator("clip_end", 0))) {
		SG_Interpolator interpolator(&ipocontr->m_clipend, interp);
		ipocontr->AddInterpolator(interpolator);
		ipocontr->SetModifyClipEnd(true);
	}

	return ipocontr;
}


SG_Controller *BL_CreateWorldIPO(BL_ActionData *action, struct World *blenderworld, KX_Scene *scene)
{
	KX_WorldIpoController *ipocontr = nullptr;

	if (blenderworld) {
		// For each active channel in the action add an interpolator to the game object.
		BL_ScalarInterpolator *interp;

		for (int i = 0; i < 3; i++) {
			if ((interp = action->GetScalarInterpolator("ambient_color", i))) {
				if (!ipocontr) {
					ipocontr = new KX_WorldIpoController();
				}
				SG_Interpolator interpolator(&ipocontr->m_ambi_rgb[i], interp);
				ipocontr->AddInterpolator(interpolator);
				ipocontr->SetModifyAmbientColor(true);
			}
		}

		for (int i = 0; i < 3; i++) {
			if ((interp = action->GetScalarInterpolator("horizon_color", i))) {
				if (!ipocontr) {
					ipocontr = new KX_WorldIpoController();
				}
				SG_Interpolator interpolator(&ipocontr->m_hori_rgb[i], interp);
				ipocontr->AddInterpolator(interpolator);
				ipocontr->SetModifyHorizonColor(true);
			}
		}

		for (int i = 0; i < 3; i++) {
			if ((interp = action->GetScalarInterpolator("zenith_color", i))) {
				if (!ipocontr) {
					ipocontr = new KX_WorldIpoController();
				}
				SG_Interpolator interpolator(&ipocontr->m_zeni_rgb[i], interp);
				ipocontr->AddInterpolator(interpolator);
				ipocontr->SetModifyZenithColor(true);
			}
		}

		if ((interp = action->GetScalarInterpolator("mist_settings.start", 0))) {
			if (!ipocontr) {
				ipocontr = new KX_WorldIpoController();
			}
			SG_Interpolator interpolator(&ipocontr->m_mist_start, interp);
			ipocontr->AddInterpolator(interpolator);
			ipocontr->SetModifyMistStart(true);
		}

		if ((interp = action->GetScalarInterpolator("mist_settings.depth", 0))) {
			if (!ipocontr) {
				ipocontr = new KX_WorldIpoController();
			}
			SG_Interpolator interpolator(&ipocontr->m_mist_dist, interp);
			ipocontr->AddInterpolator(interpolator);
			ipocontr->SetModifyMistDist(true);
		}

		if ((interp = action->GetScalarInterpolator("mist_settings.intensity", 0))) {
			if (!ipocontr) {
				ipocontr = new KX_WorldIpoController();
			}
			SG_Interpolator interpolator(&ipocontr->m_mist_intensity, interp);
			ipocontr->AddInterpolator(interpolator);
			ipocontr->SetModifyMistIntensity(true);
		}

		if (ipocontr) {
			ipocontr->m_mist_start = blenderworld->miststa;
			ipocontr->m_mist_dist = blenderworld->mistdist;
			ipocontr->m_mist_intensity = blenderworld->misi;
			ipocontr->m_hori_rgb[0] = blenderworld->horr;
			ipocontr->m_hori_rgb[1] = blenderworld->horg;
			ipocontr->m_hori_rgb[2] = blenderworld->horb;
			ipocontr->m_ambi_rgb[0] = blenderworld->ambr;
			ipocontr->m_ambi_rgb[1] = blenderworld->ambg;
			ipocontr->m_ambi_rgb[2] = blenderworld->ambb;
		}
	}
	return ipocontr;
}

SG_Controller *BL_CreateMaterialIpo(BL_ActionData *action,
                                    BL_Material *mat,
                                    KX_GameObject *gameobj,
                                    KX_Scene *scene)
{
	KX_MaterialIpoController *ipocontr = nullptr;

	BL_ScalarInterpolator *sinterp;

	for (int i = 0; i < 3; i++) {
		if ((sinterp = action->GetScalarInterpolator("diffuse_color", i))) {
			if (!ipocontr) {
				ipocontr = new KX_MaterialIpoController(mat);
			}
			SG_Interpolator interpolator(&ipocontr->m_rgba[i], sinterp);
			ipocontr->AddInterpolator(interpolator);
		}
	}

	if ((sinterp = action->GetScalarInterpolator("alpha", 0))) {
		if (!ipocontr) {
			ipocontr = new KX_MaterialIpoController(mat);
		}
		SG_Interpolator interpolator(&ipocontr->m_rgba[3], sinterp);
		ipocontr->AddInterpolator(interpolator);
	}

	for (int i = 0; i < 3; i++) {
		if ((sinterp = action->GetScalarInterpolator("specular_color", i))) {
			if (!ipocontr) {
				ipocontr = new KX_MaterialIpoController(mat);
			}
			SG_Interpolator interpolator(&ipocontr->m_specrgb[i], sinterp);
			ipocontr->AddInterpolator(interpolator);
		}
	}

	if ((sinterp = action->GetScalarInterpolator("specular_hardness", 0))) {
		if (!ipocontr) {
			ipocontr = new KX_MaterialIpoController(mat);
		}
		SG_Interpolator interpolator(&ipocontr->m_hard, sinterp);
		ipocontr->AddInterpolator(interpolator);
	}

	if ((sinterp = action->GetScalarInterpolator("specular_intensity", 0))) {
		if (!ipocontr) {
			ipocontr = new KX_MaterialIpoController(mat);
		}
		SG_Interpolator interpolator(&ipocontr->m_spec, sinterp);
		ipocontr->AddInterpolator(interpolator);
	}

	if ((sinterp = action->GetScalarInterpolator("diffuse_intensity", 0))) {
		if (!ipocontr) {
			ipocontr = new KX_MaterialIpoController(mat);
		}
		SG_Interpolator interpolator(&ipocontr->m_ref, sinterp);
		ipocontr->AddInterpolator(interpolator);
	}

	if ((sinterp = action->GetScalarInterpolator("emit", 0))) {
		if (!ipocontr) {
			ipocontr = new KX_MaterialIpoController(mat);
		}
		SG_Interpolator interpolator(&ipocontr->m_emit, sinterp);
		ipocontr->AddInterpolator(interpolator);
	}

	if ((sinterp = action->GetScalarInterpolator("ambient", 0))) {
		if (!ipocontr) {
			ipocontr = new KX_MaterialIpoController(mat);
		}
		SG_Interpolator interpolator(&ipocontr->m_ambient, sinterp);
		ipocontr->AddInterpolator(interpolator);
	}

	if ((sinterp = action->GetScalarInterpolator("specular_alpha", 0))) {
		if (!ipocontr) {
			ipocontr = new KX_MaterialIpoController(mat);
		}
		SG_Interpolator interpolator(&ipocontr->m_specAlpha, sinterp);
		ipocontr->AddInterpolator(interpolator);
	}

	if (ipocontr) {
		Material *blendermaterial = mat->GetBlenderMaterial();
		ipocontr->m_rgba[0] = blendermaterial->r;
		ipocontr->m_rgba[1] = blendermaterial->g;
		ipocontr->m_rgba[2] = blendermaterial->b;
		ipocontr->m_rgba[3] = blendermaterial->alpha;

		ipocontr->m_specrgb[0]  = blendermaterial->specr;
		ipocontr->m_specrgb[1]  = blendermaterial->specg;
		ipocontr->m_specrgb[2]  = blendermaterial->specb;

		ipocontr->m_hard        = blendermaterial->har;
		ipocontr->m_spec        = blendermaterial->spec;
		ipocontr->m_ref         = blendermaterial->ref;
		ipocontr->m_emit        = blendermaterial->emit;
		ipocontr->m_ambient     = blendermaterial->amb;
		ipocontr->m_alpha       = blendermaterial->alpha;
	}

	return ipocontr;
}
