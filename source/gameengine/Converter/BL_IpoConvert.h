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

#pragma once

/** \file gameengine/Converter/BL_IpoConvert.h
 *  \ingroup bgeconv
 */

#ifdef _MSC_VER
/* don't show stl-warnings */
#  pragma warning(disable : 4786)
#endif

#include "DNA_action_types.h"
#include "DNA_anim_types.h"
#include "DNA_camera_types.h"
#include "DNA_ipo_types.h"
#include "DNA_light_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_world_types.h"

#include "BL_Converter.h"
#include "BL_ScalarInterpolator.h"
#include "KX_CameraIpoSGController.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"
#include "KX_IInterpolator.h"
#include "KX_IpoController.h"
#include "KX_LightIpoSGController.h"
#include "KX_ObColorIpoSGController.h"
#include "KX_ScalarInterpolator.h"
#include "RAS_IPolygonMaterial.h"
#include "SG_Node.h"

/* Prototypes (No .cpp for this file due to linking issues) */
SG_Controller *BL_CreateIPO(struct bAction *action, KX_GameObject *gameobj, KX_Scene *scene);
SG_Controller *BL_CreateObColorIPO(struct bAction *action,
                                   KX_GameObject *gameobj,
                                   KX_Scene *scene);
SG_Controller *BL_CreateLampIPO(struct bAction *action, KX_GameObject *lightobj, KX_Scene *scene);
SG_Controller *BL_CreateCameraIPO(struct bAction *action,
                                  KX_GameObject *cameraobj,
                                  KX_Scene *scene);

/* Definitions */
static BL_InterpolatorList *GetAdtList(struct bAction *for_act, KX_Scene *scene)
{
  BL_Converter *converter = KX_GetActiveEngine()->GetConverter();
  BL_InterpolatorList *adtList = converter->FindInterpolatorList(scene, for_act);

  if (!adtList) {
    adtList = new BL_InterpolatorList(for_act);
    converter->RegisterInterpolatorList(scene, adtList, for_act);
  }

  return adtList;
}

SG_Controller *BL_CreateIPO(struct bAction *action, KX_GameObject *gameobj, KX_Scene *scene)
{
  KX_IpoController *ipocontr = new KX_IpoController();
  ipocontr->SetGameObject(gameobj);

  Object *blenderobject = gameobj->GetBlenderObject();

  ipocontr->GetIPOTransform().SetPosition(MT_Vector3(blenderobject->loc));
  ipocontr->GetIPOTransform().SetEulerAngles(MT_Vector3(blenderobject->rot));
  ipocontr->GetIPOTransform().SetScaling(MT_Vector3(blenderobject->scale));

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

  BL_InterpolatorList *adtList = GetAdtList(action, scene);

  // For each active channel in the adtList add an
  // interpolator to the game object.

  KX_IInterpolator *interpolator;
  BL_ScalarInterpolator *interp;

  for (int i = 0; i < 3; i++) {
    if ((interp = adtList->GetScalarInterpolator("location", i))) {
      interpolator = new KX_ScalarInterpolator(&(ipocontr->GetIPOTransform().GetPosition()[i]),
                                               interp);
      ipocontr->AddInterpolator(interpolator);
      ipocontr->SetIPOChannelActive(OB_LOC_X + i, true);
    }
  }
  for (int i = 0; i < 3; i++) {
    if ((interp = adtList->GetScalarInterpolator("delta_location", i))) {
      interpolator = new KX_ScalarInterpolator(
          &(ipocontr->GetIPOTransform().GetDeltaPosition()[i]), interp);
      ipocontr->AddInterpolator(interpolator);
      ipocontr->SetIPOChannelActive(OB_DLOC_X + i, true);
    }
  }
  for (int i = 0; i < 3; i++) {
    if ((interp = adtList->GetScalarInterpolator(rotmode, i))) {
      interpolator = new KX_ScalarInterpolator(&(ipocontr->GetIPOTransform().GetEulerAngles()[i]),
                                               interp);
      ipocontr->AddInterpolator(interpolator);
      ipocontr->SetIPOChannelActive(OB_ROT_X + i, true);
    }
  }
  for (int i = 0; i < 3; i++) {
    if ((interp = adtList->GetScalarInterpolator(drotmode, i))) {
      interpolator = new KX_ScalarInterpolator(
          &(ipocontr->GetIPOTransform().GetDeltaEulerAngles()[i]), interp);
      ipocontr->AddInterpolator(interpolator);
      ipocontr->SetIPOChannelActive(OB_DROT_X + i, true);
    }
  }
  for (int i = 0; i < 3; i++) {
    if ((interp = adtList->GetScalarInterpolator("scale", i))) {
      interpolator = new KX_ScalarInterpolator(&(ipocontr->GetIPOTransform().GetScaling()[i]),
                                               interp);
      ipocontr->AddInterpolator(interpolator);
      ipocontr->SetIPOChannelActive(OB_SIZE_X + i, true);
    }
  }
  for (int i = 0; i < 3; i++) {
    if ((interp = adtList->GetScalarInterpolator("delta_scale", i))) {
      interpolator = new KX_ScalarInterpolator(&(ipocontr->GetIPOTransform().GetDeltaScaling()[i]),
                                               interp);
      ipocontr->AddInterpolator(interpolator);
      ipocontr->SetIPOChannelActive(OB_DSIZE_X + i, true);
    }
  }

  return ipocontr;
}

SG_Controller *BL_CreateObColorIPO(struct bAction *action, KX_GameObject *gameobj, KX_Scene *scene)
{
  KX_ObColorIpoSGController *ipocontr_obcol = nullptr;
  KX_IInterpolator *interpolator;
  BL_ScalarInterpolator *interp;
  BL_InterpolatorList *adtList = GetAdtList(action, scene);

  for (int i = 0; i < 4; i++) {
    if ((interp = adtList->GetScalarInterpolator("color", i))) {
      if (!ipocontr_obcol) {
        ipocontr_obcol = new KX_ObColorIpoSGController();
      }
      interpolator = new KX_ScalarInterpolator(&ipocontr_obcol->m_rgba[i], interp);
      ipocontr_obcol->AddInterpolator(interpolator);
    }
  }

  return ipocontr_obcol;
}

SG_Controller *BL_CreateLampIPO(struct bAction *action, KX_GameObject *lightobj, KX_Scene *scene)
{
  KX_LightIpoSGController *ipocontr = new KX_LightIpoSGController();

  Light *blenderlamp = (Light *)lightobj->GetBlenderObject()->data;

  ipocontr->m_energy = blenderlamp->energy;
  ipocontr->m_col_rgb[0] = blenderlamp->r;
  ipocontr->m_col_rgb[1] = blenderlamp->g;
  ipocontr->m_col_rgb[2] = blenderlamp->b;
  //ipocontr->m_dist = blenderlamp->dist;

  BL_InterpolatorList *adtList = GetAdtList(action, scene);

  // For each active channel in the adtList add an
  // interpolator to the game object.

  KX_IInterpolator *interpolator;
  BL_ScalarInterpolator *interp;

  if ((interp = adtList->GetScalarInterpolator("energy", 0))) {
    interpolator = new KX_ScalarInterpolator(&ipocontr->m_energy, interp);
    ipocontr->AddInterpolator(interpolator);
    ipocontr->SetModifyEnergy(true);
  }

  /*if ((interp = adtList->GetScalarInterpolator("distance", 0))) {
    interpolator = new KX_ScalarInterpolator(&ipocontr->m_dist, interp);
    ipocontr->AddInterpolator(interpolator);
    ipocontr->SetModifyDist(true);
  }*/

  for (int i = 0; i < 3; i++) {
    if ((interp = adtList->GetScalarInterpolator("color", i))) {
      interpolator = new KX_ScalarInterpolator(&ipocontr->m_col_rgb[i], interp);
      ipocontr->AddInterpolator(interpolator);
      ipocontr->SetModifyColor(true);
    }
  }

  return ipocontr;
}

SG_Controller *BL_CreateCameraIPO(struct bAction *action,
                                  KX_GameObject *cameraobj,
                                  KX_Scene *scene)
{
  KX_CameraIpoSGController *ipocontr = new KX_CameraIpoSGController();

  Camera *blendercamera = (Camera *)cameraobj->GetBlenderObject()->data;

  ipocontr->m_lens = blendercamera->lens;
  ipocontr->m_clipstart = blendercamera->clip_start;
  ipocontr->m_clipend = blendercamera->clip_end;

  BL_InterpolatorList *adtList = GetAdtList(action, scene);

  // For each active channel in the adtList add an
  // interpolator to the game object.

  KX_IInterpolator *interpolator;
  BL_ScalarInterpolator *interp;

  if ((interp = adtList->GetScalarInterpolator("lens", 0))) {
    interpolator = new KX_ScalarInterpolator(&ipocontr->m_lens, interp);
    ipocontr->AddInterpolator(interpolator);
    ipocontr->SetModifyLens(true);
  }

  if ((interp = adtList->GetScalarInterpolator("clip_start", 0))) {
    interpolator = new KX_ScalarInterpolator(&ipocontr->m_clipstart, interp);
    ipocontr->AddInterpolator(interpolator);
    ipocontr->SetModifyClipStart(true);
  }

  if ((interp = adtList->GetScalarInterpolator("clip_end", 0))) {
    interpolator = new KX_ScalarInterpolator(&ipocontr->m_clipend, interp);
    ipocontr->AddInterpolator(interpolator);
    ipocontr->SetModifyClipEnd(true);
  }

  return ipocontr;
}
