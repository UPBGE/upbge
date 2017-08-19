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
#include "KX_IInterpolator.h"
#include "KX_ScalarInterpolator.h"

#include "BL_BlenderScalarInterpolator.h"
#include "BL_BlenderConverter.h"
#include "KX_Globals.h"

#include "RAS_IPolygonMaterial.h"

#include "DNA_object_types.h"
#include "DNA_action_types.h"
#include "DNA_anim_types.h"
#include "DNA_ipo_types.h"
#include "DNA_lamp_types.h"
#include "DNA_world_types.h"
#include "DNA_camera_types.h"
#include "DNA_material_types.h"
/* end of blender include block */

#include "KX_IPO_SGController.h"
#include "KX_LightIpoSGController.h"
#include "KX_CameraIpoSGController.h"
#include "KX_WorldIpoController.h"
#include "KX_ObColorIpoSGController.h"
#include "KX_MaterialIpoController.h"

#include "SG_Node.h"

static BL_InterpolatorList *GetAdtList(struct bAction *for_act, KX_Scene *scene)
{
	BL_BlenderConverter *converter = KX_GetActiveEngine()->GetConverter();
	BL_InterpolatorList *adtList= converter->FindInterpolatorList(scene, for_act);

	if (!adtList) {
		adtList = new BL_InterpolatorList(for_act);
		converter->RegisterInterpolatorList(scene, adtList, for_act);
	}
			
	return adtList;
}

SG_Controller *BL_CreateIPO(struct bAction *action, KX_GameObject* gameobj, KX_Scene *scene)
{
	KX_IpoSGController* ipocontr = new KX_IpoSGController();
	ipocontr->SetGameObject(gameobj);

	Object* blenderobject = gameobj->GetBlenderObject();

	ipocontr->GetIPOTransform().SetPosition(mt::vec3(blenderobject->loc));
	ipocontr->GetIPOTransform().SetEulerAngles(mt::vec3(blenderobject->rot));
	ipocontr->GetIPOTransform().SetScaling(mt::vec3(blenderobject->size));

	const char *rotmode, *drotmode;

	switch (blenderobject->rotmode) {
	case ROT_MODE_AXISANGLE:
		rotmode = "rotation_axis_angle";
		drotmode = "delta_rotation_axis_angle";
		break;
	case ROT_MODE_QUAT: /* XXX, this isn't working, currently only eulers are supported [#28853] */
		rotmode = "rotation_quaternion";
		drotmode = "delta_rotation_quaternion";
		break;
	default:
		rotmode = "rotation_euler";
		drotmode = "delta_rotation_euler";
		break;
	}

	BL_InterpolatorList *adtList= GetAdtList(action, scene);
		
	// For each active channel in the adtList add an
	// interpolator to the game object.
		
	KX_IInterpolator *interpolator;
	BL_ScalarInterpolator *interp;
		
	for (int i=0; i<3; i++) {
		if ((interp = adtList->GetScalarInterpolator("location", i))) {
			interpolator= new KX_ScalarInterpolator(&(ipocontr->GetIPOTransform().GetPosition()[i]), interp);
			ipocontr->AddInterpolator(interpolator);
			ipocontr->SetIPOChannelActive(OB_LOC_X+i, true);
		}
	}
	for (int i=0; i<3; i++) {
		if ((interp = adtList->GetScalarInterpolator("delta_location", i))) {
			interpolator= new KX_ScalarInterpolator(&(ipocontr->GetIPOTransform().GetDeltaPosition()[i]), interp);
			ipocontr->AddInterpolator(interpolator);
			ipocontr->SetIPOChannelActive(OB_DLOC_X+i, true);
		}
	}
	for (int i=0; i<3; i++) {
		if ((interp = adtList->GetScalarInterpolator(rotmode, i))) {
			interpolator= new KX_ScalarInterpolator(&(ipocontr->GetIPOTransform().GetEulerAngles()[i]), interp);
			ipocontr->AddInterpolator(interpolator);
			ipocontr->SetIPOChannelActive(OB_ROT_X+i, true);
		}
	}
	for (int i=0; i<3; i++) {
		if ((interp = adtList->GetScalarInterpolator(drotmode, i))) {
			interpolator= new KX_ScalarInterpolator(&(ipocontr->GetIPOTransform().GetDeltaEulerAngles()[i]), interp);
			ipocontr->AddInterpolator(interpolator);
			ipocontr->SetIPOChannelActive(OB_DROT_X+i, true);
		}
	}
	for (int i=0; i<3; i++) {
		if ((interp = adtList->GetScalarInterpolator("scale", i))) {
			interpolator= new KX_ScalarInterpolator(&(ipocontr->GetIPOTransform().GetScaling()[i]), interp);
			ipocontr->AddInterpolator(interpolator);
			ipocontr->SetIPOChannelActive(OB_SIZE_X+i, true);
		}
	}
	for (int i=0; i<3; i++) {
		if ((interp = adtList->GetScalarInterpolator("delta_scale", i))) {
			interpolator= new KX_ScalarInterpolator(&(ipocontr->GetIPOTransform().GetDeltaScaling()[i]), interp);
			ipocontr->AddInterpolator(interpolator);
			ipocontr->SetIPOChannelActive(OB_DSIZE_X+i, true);
		}
	}
		

	return ipocontr;
}


SG_Controller *BL_CreateObColorIPO(struct bAction *action, KX_GameObject* gameobj, KX_Scene *scene)
{
	KX_ObColorIpoSGController* ipocontr_obcol=nullptr;
	KX_IInterpolator *interpolator;
	BL_ScalarInterpolator *interp;
	BL_InterpolatorList *adtList= GetAdtList(action, scene);

	for (int i=0; i<4; i++) {
		if ((interp = adtList->GetScalarInterpolator("color", i))) {
			if (!ipocontr_obcol) {
				ipocontr_obcol = new KX_ObColorIpoSGController();
			}
			interpolator= new KX_ScalarInterpolator(&ipocontr_obcol->m_rgba[i], interp);
			ipocontr_obcol->AddInterpolator(interpolator);
		}
	}

	return ipocontr_obcol;
}

SG_Controller *BL_CreateLampIPO(struct bAction *action, KX_GameObject*  lightobj, KX_Scene *scene)
{
	KX_LightIpoSGController* ipocontr = new KX_LightIpoSGController();

	Lamp *blenderlamp = (Lamp*)lightobj->GetBlenderObject()->data;

	ipocontr->m_energy = blenderlamp->energy;
	ipocontr->m_col_rgb[0] = blenderlamp->r;
	ipocontr->m_col_rgb[1] = blenderlamp->g;
	ipocontr->m_col_rgb[2] = blenderlamp->b;
	ipocontr->m_dist = blenderlamp->dist;

	BL_InterpolatorList *adtList= GetAdtList(action, scene);

	// For each active channel in the adtList add an
	// interpolator to the game object.
		
	KX_IInterpolator *interpolator;
	BL_ScalarInterpolator *interp;
		
	if ((interp= adtList->GetScalarInterpolator("energy", 0))) {
		interpolator= new KX_ScalarInterpolator(&ipocontr->m_energy, interp);
		ipocontr->AddInterpolator(interpolator);
		ipocontr->SetModifyEnergy(true);
	}

	if ((interp = adtList->GetScalarInterpolator("distance", 0))) {
		interpolator= new KX_ScalarInterpolator(&ipocontr->m_dist, interp);
		ipocontr->AddInterpolator(interpolator);
		ipocontr->SetModifyDist(true);
	}
		
	for (int i=0; i<3; i++) {
		if ((interp = adtList->GetScalarInterpolator("color", i))) {
			interpolator= new KX_ScalarInterpolator(&ipocontr->m_col_rgb[i], interp);
			ipocontr->AddInterpolator(interpolator);
			ipocontr->SetModifyColor(true);
		}
	}

	return ipocontr;
}

SG_Controller *BL_CreateCameraIPO(struct bAction *action, KX_GameObject*  cameraobj, KX_Scene *scene)
{
	KX_CameraIpoSGController* ipocontr = new KX_CameraIpoSGController();

	Camera *blendercamera = (Camera*)cameraobj->GetBlenderObject()->data;

	ipocontr->m_lens = blendercamera->lens;
	ipocontr->m_clipstart = blendercamera->clipsta;
	ipocontr->m_clipend = blendercamera->clipend;

	BL_InterpolatorList *adtList= GetAdtList(action, scene);

	// For each active channel in the adtList add an
	// interpolator to the game object.
		
	KX_IInterpolator *interpolator;
	BL_ScalarInterpolator *interp;
		
	if ((interp = adtList->GetScalarInterpolator("lens", 0))) {
		interpolator= new KX_ScalarInterpolator(&ipocontr->m_lens, interp);
		ipocontr->AddInterpolator(interpolator);
		ipocontr->SetModifyLens(true);
	}

	if ((interp = adtList->GetScalarInterpolator("clip_start", 0))) {
		interpolator= new KX_ScalarInterpolator(&ipocontr->m_clipstart, interp);
		ipocontr->AddInterpolator(interpolator);
		ipocontr->SetModifyClipStart(true);
	}

	if ((interp = adtList->GetScalarInterpolator("clip_end", 0))) {
		interpolator= new KX_ScalarInterpolator(&ipocontr->m_clipend, interp);
		ipocontr->AddInterpolator(interpolator);
		ipocontr->SetModifyClipEnd(true);
	}

	return ipocontr;
}


SG_Controller * BL_CreateWorldIPO( bAction *action, struct World *blenderworld, KX_Scene *scene )
{
	KX_WorldIpoController *ipocontr = nullptr;

	if (blenderworld) {
		BL_InterpolatorList *adtList = GetAdtList(action, scene);

		// For each active channel in the adtList add an interpolator to the game object.
		KX_IInterpolator *interpolator;
		BL_ScalarInterpolator *interp;

		for (int i=0; i<3; i++) {
			if ((interp = adtList->GetScalarInterpolator("ambient_color", i))) {
				if (!ipocontr) {
					ipocontr = new KX_WorldIpoController();
				}
				interpolator = new KX_ScalarInterpolator(&ipocontr->m_ambi_rgb[i], interp);
				ipocontr->AddInterpolator(interpolator);
				ipocontr->SetModifyAmbientColor(true);
			}
		}

		for (int i=0; i<3; i++) {
			if ((interp = adtList->GetScalarInterpolator("horizon_color", i))) {
				if (!ipocontr) {
					ipocontr = new KX_WorldIpoController();
				}
				interpolator = new KX_ScalarInterpolator(&ipocontr->m_hori_rgb[i], interp);
				ipocontr->AddInterpolator(interpolator);
				ipocontr->SetModifyHorizonColor(true);
			}
		}

		for (int i = 0; i<3; i++) {
			if ((interp = adtList->GetScalarInterpolator("zenith_color", i))) {
				if (!ipocontr) {
					ipocontr = new KX_WorldIpoController();
				}
				interpolator = new KX_ScalarInterpolator(&ipocontr->m_zeni_rgb[i], interp);
				ipocontr->AddInterpolator(interpolator);
				ipocontr->SetModifyZenithColor(true);
			}
		}

		if ((interp = adtList->GetScalarInterpolator("mist_settings.start", 0))) {
			if (!ipocontr) {
				ipocontr = new KX_WorldIpoController();
			}
			interpolator = new KX_ScalarInterpolator(&ipocontr->m_mist_start, interp);
			ipocontr->AddInterpolator(interpolator);
			ipocontr->SetModifyMistStart(true);
		}

		if ((interp = adtList->GetScalarInterpolator("mist_settings.depth", 0))) {
			if (!ipocontr) {
				ipocontr = new KX_WorldIpoController();
			}
			interpolator = new KX_ScalarInterpolator(&ipocontr->m_mist_dist, interp);
			ipocontr->AddInterpolator(interpolator);
			ipocontr->SetModifyMistDist(true);
		}

		if ((interp = adtList->GetScalarInterpolator("mist_settings.intensity", 0))) {
			if (!ipocontr) {
				ipocontr = new KX_WorldIpoController();
			}
			interpolator = new KX_ScalarInterpolator(&ipocontr->m_mist_intensity, interp);
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

SG_Controller *BL_CreateMaterialIpo(
	struct bAction *action,
	RAS_IPolyMaterial *polymat,
	KX_GameObject* gameobj,  
	KX_Scene *scene
	)
{
	KX_MaterialIpoController* ipocontr = nullptr;

	BL_InterpolatorList *adtList= GetAdtList(action, scene);
	KX_IInterpolator *interpolator;
	BL_ScalarInterpolator *sinterp;

	// --
	for (int i=0; i<3; i++) {
		if ((sinterp = adtList->GetScalarInterpolator("diffuse_color", i))) {
			if (!ipocontr) {
				ipocontr = new KX_MaterialIpoController(polymat);
			}
			interpolator= new KX_ScalarInterpolator(&ipocontr->m_rgba[i], sinterp);
			ipocontr->AddInterpolator(interpolator);
		}
	}

	if ((sinterp = adtList->GetScalarInterpolator("alpha", 0))) {
		if (!ipocontr) {
			ipocontr = new KX_MaterialIpoController(polymat);
		}
		interpolator= new KX_ScalarInterpolator(&ipocontr->m_rgba[3], sinterp);
		ipocontr->AddInterpolator(interpolator);
	}

	for (int i=0; i<3; i++) {
		if ((sinterp = adtList->GetScalarInterpolator("specular_color", i))) {
			if (!ipocontr) {
				ipocontr = new KX_MaterialIpoController(polymat);
			}
			interpolator= new KX_ScalarInterpolator(&ipocontr->m_specrgb[i], sinterp);
			ipocontr->AddInterpolator(interpolator);
		}
	}

	if ((sinterp = adtList->GetScalarInterpolator("specular_hardness", 0))) {
		if (!ipocontr) {
			ipocontr = new KX_MaterialIpoController(polymat);
		}
		interpolator= new KX_ScalarInterpolator(&ipocontr->m_hard, sinterp);
		ipocontr->AddInterpolator(interpolator);
	}

	if ((sinterp = adtList->GetScalarInterpolator("specular_intensity", 0))) {
		if (!ipocontr) {
			ipocontr = new KX_MaterialIpoController(polymat);
		}
		interpolator= new KX_ScalarInterpolator(&ipocontr->m_spec, sinterp);
		ipocontr->AddInterpolator(interpolator);
	}

	if ((sinterp = adtList->GetScalarInterpolator("diffuse_intensity", 0))) {
		if (!ipocontr) {
			ipocontr = new KX_MaterialIpoController(polymat);
		}
		interpolator= new KX_ScalarInterpolator(&ipocontr->m_ref, sinterp);
		ipocontr->AddInterpolator(interpolator);
	}

	if ((sinterp = adtList->GetScalarInterpolator("emit", 0))) {
		if (!ipocontr) {
			ipocontr = new KX_MaterialIpoController(polymat);
		}
		interpolator= new KX_ScalarInterpolator(&ipocontr->m_emit, sinterp);
		ipocontr->AddInterpolator(interpolator);
	}

	if ((sinterp = adtList->GetScalarInterpolator("ambient", 0))) {
		if (!ipocontr) {
			ipocontr = new KX_MaterialIpoController(polymat);
		}
		interpolator = new KX_ScalarInterpolator(&ipocontr->m_ambient, sinterp);
		ipocontr->AddInterpolator(interpolator);
	}

	if ((sinterp = adtList->GetScalarInterpolator("specular_alpha", 0))) {
		if (!ipocontr) {
			ipocontr = new KX_MaterialIpoController(polymat);
		}
		interpolator = new KX_ScalarInterpolator(&ipocontr->m_specAlpha, sinterp);
		ipocontr->AddInterpolator(interpolator);
	}

	if (ipocontr) {
		Material *blendermaterial = polymat->GetBlenderMaterial();
		ipocontr->m_rgba[0]	= blendermaterial->r;
		ipocontr->m_rgba[1]	= blendermaterial->g;
		ipocontr->m_rgba[2]	= blendermaterial->b;
		ipocontr->m_rgba[3]	= blendermaterial->alpha;

		ipocontr->m_specrgb[0]	= blendermaterial->specr;
		ipocontr->m_specrgb[1]	= blendermaterial->specg;
		ipocontr->m_specrgb[2]	= blendermaterial->specb;

		ipocontr->m_hard		= blendermaterial->har;
		ipocontr->m_spec		= blendermaterial->spec;
		ipocontr->m_ref			= blendermaterial->ref;
		ipocontr->m_emit		= blendermaterial->emit;
		ipocontr->m_ambient		= blendermaterial->amb;
		ipocontr->m_alpha		= blendermaterial->alpha;
	}

	return ipocontr;
}
