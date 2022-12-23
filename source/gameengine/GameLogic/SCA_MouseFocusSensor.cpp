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
 * SCA_MouseFocusSensor determines mouse in/out/over events.
 */

/** \file gameengine/Ketsji/SCA_MouseFocusSensor.cpp
 *  \ingroup ketsji
 */

#ifdef _MSC_VER
/* This warning tells us about truncation of __long__ stl-generated names.
 * It can occasionally cause DevStudio to have internal compiler warnings. */
#  pragma warning(disable : 4786)
#endif

#include "SCA_MouseFocusSensor.h"

#include "DNA_camera_types.h"

#include "CM_Message.h"
#include "KX_Camera.h"
#include "KX_ClientObjectInfo.h"
#include "KX_PyMath.h"
#include "KX_RayCast.h"
#include "RAS_ICanvas.h"
#include "RAS_MeshObject.h"

/* ------------------------------------------------------------------------- */
/* Native functions                                                          */
/* ------------------------------------------------------------------------- */

SCA_MouseFocusSensor::SCA_MouseFocusSensor(SCA_MouseManager *eventmgr,
                                           int startx,
                                           int starty,
                                           short int mousemode,
                                           int focusmode,
                                           bool bCollisionPulse,
                                           const std::string &propname,
                                           bool bFindMaterial,
                                           bool bXRay,
                                           int mask,
                                           KX_Scene *kxscene,
                                           KX_KetsjiEngine *kxengine,
                                           SCA_IObject *gameobj)
    : SCA_MouseSensor(eventmgr, startx, starty, mousemode, gameobj),
      m_focusmode(focusmode),
      m_bCollisionPulse(bCollisionPulse),
      m_bXRay(bXRay),
      m_mask(mask),
      m_bFindMaterial(bFindMaterial),
      m_propertyname(propname),
      m_kxscene(kxscene),
      m_kxengine(kxengine)
{
  Init();
}

void SCA_MouseFocusSensor::Init()
{
  m_mouse_over_in_previous_frame = (m_invert) ? true : false;
  m_positive_event = false;
  m_hitObject = 0;
  m_hitObject_Last = nullptr;
  m_reset = true;

  m_hitPosition.setValue(0, 0, 0);
  m_prevTargetPoint.setValue(0, 0, 0);
  m_prevSourcePoint.setValue(0, 0, 0);
  m_hitNormal.setValue(0, 0, 1);
}

bool SCA_MouseFocusSensor::Evaluate()
{

  KX_Camera *previousCam = m_kxscene->GetActiveCamera();
  /* If scene has an overlay camera,
   * we want mouse events to be computed in overlay camera space.
   * Then we mark the overlay camera as active camera.
   */
  KX_Camera *overlayCam = m_kxscene->GetOverlayCamera();
  bool restorePreviousCam = false;
  if (overlayCam) {
    Object *obcam = overlayCam->GetBlenderObject();
    Camera *blCam = (Camera *)obcam->data;
    if (blCam->gameflag & GAME_CAM_OVERLAY_MOUSE_CONTROL) {
      m_kxscene->SetActiveCamera(m_kxscene->GetOverlayCamera());
      restorePreviousCam = true;
    }
  }

  bool result = false;
  bool obHasFocus = false;
  bool reset = m_reset && m_level;

  m_reset = false;
  if (m_focusmode) {
    /* Focus behavior required. Test mouse-on. The rest is
     * equivalent to handling a key. */
    obHasFocus = ParentObjectHasFocus();

    if (!obHasFocus) {
      m_positive_event = false;
      if (m_mouse_over_in_previous_frame) {
        result = true;
      }
    }
    else {
      m_positive_event = true;
      if (!m_mouse_over_in_previous_frame) {
        result = true;
      }
      else if (m_bCollisionPulse && (m_hitObject != m_hitObject_Last)) {
        result = true;
      }
    }
    if (reset) {
      // force an event
      result = true;
    }
  }
  else {
    /* No focus behavior required: revert to the basic mode. This
     * mode is never used, because the converter never makes this
     * sensor for a mouse-key event. It is here for
     * completeness. */
    result = SCA_MouseSensor::Evaluate();
    m_positive_event = (m_val != 0);
  }

  m_mouse_over_in_previous_frame = obHasFocus;
  m_hitObject_Last = (void *)m_hitObject;

  if (restorePreviousCam) {
    m_kxscene->SetActiveCamera(previousCam);
  }

  return result;
}

bool SCA_MouseFocusSensor::RayHit(KX_ClientObjectInfo *client_info,
                                  KX_RayCast *result,
                                  void */*data*/)
{
  KX_GameObject *hitKXObj = client_info->m_gameobject;

  /* Is this me? In the ray test, there are a lot of extra checks
   * for aliasing artifacts from self-hits. That doesn't happen
   * here, so a simple test suffices. Or does the camera also get
   * self-hits? (No, and the raysensor shouldn't do it either, since
   * self-hits are excluded by setting the correct ignore-object.)
   * Hitspots now become valid. */
  KX_GameObject *thisObj = (KX_GameObject *)GetParent();

  bool bFound = false;

  if ((m_focusmode == 2) || hitKXObj == thisObj) {
    if (m_propertyname.empty()) {
      bFound = true;
    }
    else {
      if (m_bFindMaterial) {
        for (unsigned int i = 0; i < hitKXObj->GetMeshCount(); ++i) {
          RAS_MeshObject *meshObj = hitKXObj->GetMesh(i);
          for (unsigned int j = 0; j < meshObj->NumMaterials(); ++j) {
            bFound = (m_propertyname == std::string(meshObj->GetMaterialName(j), 2));
            if (bFound)
              break;
          }
        }
      }
      else {
        bFound = hitKXObj->GetProperty(m_propertyname) != nullptr;
      }
    }

    if (bFound) {
      m_hitObject = hitKXObj;
      m_hitPosition = result->m_hitPoint;
      m_hitNormal = result->m_hitNormal;
      m_hitUV = result->m_hitUV;
      return true;
    }
  }

  return true;  // object must be visible to trigger
                // return false;  // occluded objects can trigger
}

/* this function is used to pre-filter the object before casting the ray on them.
 * This is useful for "X-Ray" option when we want to see "through" unwanted object.
 */
bool SCA_MouseFocusSensor::NeedRayCast(KX_ClientObjectInfo *client, void */*data*/)
{
  KX_GameObject *hitKXObj = client->m_gameobject;

  if (client->m_type > KX_ClientObjectInfo::ACTOR) {
    // Unknown type of object, skip it.
    // Should not occur as the sensor objects are filtered in RayTest()
    CM_Error("invalid client type " << client->m_type << " found ray casting");
    return false;
  }

  // The current object is not in the proper layer.
  if (!(hitKXObj->GetCollisionGroup() & m_mask)) {
    return false;
  }

  if (m_bXRay && m_propertyname.size() != 0) {
    if (m_bFindMaterial) {
      bool found = false;
      for (unsigned int i = 0; i < hitKXObj->GetMeshCount(); ++i) {
        RAS_MeshObject *meshObj = hitKXObj->GetMesh(i);
        for (unsigned int j = 0; j < meshObj->NumMaterials(); ++j) {
          found = (m_propertyname == std::string(meshObj->GetMaterialName(j), 2));
          if (found)
            break;
        }
      }
      if (!found)
        return false;
    }
    else {
      if (hitKXObj->GetProperty(m_propertyname) == nullptr)
        return false;
    }
  }
  return true;
}

bool SCA_MouseFocusSensor::ParentObjectHasFocusCamera(KX_Camera *cam)
{
  /* All screen handling in the gameengine is done by GL,
   * specifically the model/view and projection parts. The viewport
   * part is in the creator.
   *
   * The theory is this:
   * WCS - world coordinates
   * -> wcs_camcs_trafo ->
   * camCS - camera coordinates
   * -> camcs_clip_trafo ->
   * clipCS - normalized device coordinates?
   * -> normview_win_trafo
   * winCS - window coordinates
   *
   * The first two transforms are respectively the model/view and
   * the projection matrix. These are passed to the rasterizer, and
   * we store them in the camera for easy access.
   *
   * For normalized device coords (xn = x/w, yn = y/w/zw) the
   * windows coords become (lb = left bottom)
   *
   * xwin = [(xn + 1.0) * width]/2 + x_lb
   * ywin = [(yn + 1.0) * height]/2 + y_lb
   *
   * Inverting (blender y is flipped!):
   *
   * xn = 2(xwin - x_lb)/width - 1.0
   * yn = 2(ywin - y_lb)/height - 1.0
   *    = 2(height - y_blender - y_lb)/height - 1.0
   *    = 1.0 - 2(y_blender - y_lb)/height
   *
   * */

  /* Because we don't want to worry about resize events, camera
   * changes and all that crap, we just determine this over and
   * over. Stop whining. We have lots of other calculations to do
   * here as well. These reads are not the main cost. If there is no
   * canvas, the test is irrelevant. The 1.0 makes sure the
   * calculations don't bomb. Maybe we should explicitly guard for
   * division by 0.0...*/

  RAS_Rect area, viewport;
  RAS_ICanvas *canvas = m_kxengine->GetCanvas();
  short m_y_inv = canvas->GetHeight() - m_y;

  const RAS_Rect displayArea = m_kxengine->GetRasterizer()->GetRenderArea(
      canvas, RAS_Rasterizer::RAS_STEREO_LEFTEYE);
  m_kxengine->GetSceneViewport(m_kxscene, cam, displayArea, area, viewport);

  /* Check if the mouse is in the viewport */
  if ((m_x < viewport.GetRight() &&           // less than right
       m_x > viewport.GetLeft() &&            // more than then left
       m_y_inv < viewport.GetTop() &&         // below top
       m_y_inv > viewport.GetBottom()) == 0)  // above bottom
  {
    return false;
  }

  float height = float(viewport.GetTop() - viewport.GetBottom() + 1);
  float width = float(viewport.GetRight() - viewport.GetLeft() + 1);

  float x_lb = float(viewport.GetLeft());
  float y_lb = float(viewport.GetBottom());

  MT_Vector4 frompoint;
  MT_Vector4 topoint;

  /* m_y_inv - inverting for a bounds check is only part of it, now make relative to view bounds */
  m_y_inv = (viewport.GetTop() - m_y_inv) + viewport.GetBottom();

  /* There's some strangeness I don't fully get here... These values
   * _should_ be wrong! - see from point Z values */

  /*	build the from and to point in normalized device coordinates
   *	Normalized device coordinates are [-1,1] in x, y, z
   *
   *	The actual z coordinates used don't have to be exact just infront and
   *	behind of the near and far clip planes.
   */
  frompoint.setValue(
      (2 * (m_x - x_lb) / width) - 1.0f, 1.0f - (2 * (m_y_inv - y_lb) / height), -1.0f, 1.0f);

  topoint.setValue(
      (2 * (m_x - x_lb) / width) - 1.0f, 1.0f - (2 * (m_y_inv - y_lb) / height), 1.0f, 1.0f);

  /* camera to world  */
  MT_Matrix4x4 camcs_wcs_matrix = MT_Matrix4x4(cam->GetCameraToWorld());

  /* badly defined, the first time round.... I wonder why... I might
   * want to guard against floating point errors here.*/
  MT_Matrix4x4 clip_camcs_matrix = MT_Matrix4x4(cam->GetProjectionMatrix());
  clip_camcs_matrix.invert();

  /* shoot-points: clip to cam to wcs . win to clip was already done.*/
  frompoint = clip_camcs_matrix * frompoint;
  topoint = clip_camcs_matrix * topoint;
  /* clipstart = - (frompoint[2] / frompoint[3])
   * clipend = - (topoint[2] / topoint[3]) */
  frompoint = camcs_wcs_matrix * frompoint;
  topoint = camcs_wcs_matrix * topoint;

  /* from hom wcs to 3d wcs: */
  m_prevSourcePoint.setValue(
      frompoint[0] / frompoint[3], frompoint[1] / frompoint[3], frompoint[2] / frompoint[3]);

  m_prevTargetPoint.setValue(
      topoint[0] / topoint[3], topoint[1] / topoint[3], topoint[2] / topoint[3]);

  /* 2. Get the object from PhysicsEnvironment */
  /* Shoot! Beware that the first argument here is an
   * ignore-object. We don't ignore anything... */
  PHY_IPhysicsController *physics_controller = cam->GetPhysicsController();
  PHY_IPhysicsEnvironment *physics_environment = m_kxscene->GetPhysicsEnvironment();

  // get UV mapping
  KX_RayCast::Callback<SCA_MouseFocusSensor, void> callback(
      this, physics_controller, nullptr, false, true);

  KX_RayCast::RayTest(physics_environment, m_prevSourcePoint, m_prevTargetPoint, callback);

  if (m_hitObject)
    return true;

  return false;
}

bool SCA_MouseFocusSensor::ParentObjectHasFocus()
{
  m_hitObject = 0;
  m_hitPosition.setValue(0, 0, 0);
  m_hitNormal.setValue(1, 0, 0);

  KX_Camera *activecam = m_kxscene->GetActiveCamera();

  if (ParentObjectHasFocusCamera(activecam))
    return true;

  EXP_ListValue<KX_Camera> *cameras = m_kxscene->GetCameraList();
  for (KX_Camera *cam : cameras) {
    if ((cam != activecam) && cam->GetViewport())
      if (ParentObjectHasFocusCamera(cam))
        return true;
  }

  return false;
}

const MT_Vector3 &SCA_MouseFocusSensor::RaySource() const
{
  return m_prevSourcePoint;
}

const MT_Vector3 &SCA_MouseFocusSensor::RayTarget() const
{
  return m_prevTargetPoint;
}

const MT_Vector3 &SCA_MouseFocusSensor::HitPosition() const
{
  return m_hitPosition;
}

const MT_Vector3 &SCA_MouseFocusSensor::HitNormal() const
{
  return m_hitNormal;
}

const MT_Vector2 &SCA_MouseFocusSensor::HitUV() const
{
  return m_hitUV;
}

#ifdef WITH_PYTHON

/* ------------------------------------------------------------------------- */
/* Python functions                                                          */
/* ------------------------------------------------------------------------- */

/* Integration hooks ------------------------------------------------------- */
PyTypeObject SCA_MouseFocusSensor::Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) "SCA_MouseFocusSensor",
    sizeof(EXP_PyObjectPlus_Proxy),
    0,
    py_base_dealloc,
    0,
    0,
    0,
    0,
    py_base_repr,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    Methods,
    0,
    0,
    &SCA_MouseSensor::Type,
    0,
    0,
    0,
    0,
    0,
    0,
    py_base_new};

PyMethodDef SCA_MouseFocusSensor::Methods[] = {
    {nullptr, nullptr}  // Sentinel
};

PyAttributeDef SCA_MouseFocusSensor::Attributes[] = {
    EXP_PYATTRIBUTE_RO_FUNCTION("raySource", SCA_MouseFocusSensor, pyattr_get_ray_source),
    EXP_PYATTRIBUTE_RO_FUNCTION("rayTarget", SCA_MouseFocusSensor, pyattr_get_ray_target),
    EXP_PYATTRIBUTE_RO_FUNCTION("rayDirection", SCA_MouseFocusSensor, pyattr_get_ray_direction),
    EXP_PYATTRIBUTE_RO_FUNCTION("hitObject", SCA_MouseFocusSensor, pyattr_get_hit_object),
    EXP_PYATTRIBUTE_RO_FUNCTION("hitPosition", SCA_MouseFocusSensor, pyattr_get_hit_position),
    EXP_PYATTRIBUTE_RO_FUNCTION("hitNormal", SCA_MouseFocusSensor, pyattr_get_hit_normal),
    EXP_PYATTRIBUTE_RO_FUNCTION("hitUV", SCA_MouseFocusSensor, pyattr_get_hit_uv),
    EXP_PYATTRIBUTE_BOOL_RW("usePulseFocus", SCA_MouseFocusSensor, m_bCollisionPulse),
    EXP_PYATTRIBUTE_BOOL_RW("useXRay", SCA_MouseFocusSensor, m_bXRay),
    EXP_PYATTRIBUTE_INT_RW(
        "mask", 1, (1 << OB_MAX_COL_MASKS) - 1, true, SCA_MouseFocusSensor, m_mask),
    EXP_PYATTRIBUTE_BOOL_RW("useMaterial", SCA_MouseFocusSensor, m_bFindMaterial),
    EXP_PYATTRIBUTE_STRING_RW(
        "propName", 0, MAX_PROP_NAME, false, SCA_MouseFocusSensor, m_propertyname),
    EXP_PYATTRIBUTE_NULL  // Sentinel
};

/* Attributes */
PyObject *SCA_MouseFocusSensor::pyattr_get_ray_source(EXP_PyObjectPlus *self_v,
                                                      const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_MouseFocusSensor *self = static_cast<SCA_MouseFocusSensor *>(self_v);
  return PyObjectFrom(self->RaySource());
}

PyObject *SCA_MouseFocusSensor::pyattr_get_ray_target(EXP_PyObjectPlus *self_v,
                                                      const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_MouseFocusSensor *self = static_cast<SCA_MouseFocusSensor *>(self_v);
  return PyObjectFrom(self->RayTarget());
}

PyObject *SCA_MouseFocusSensor::pyattr_get_ray_direction(EXP_PyObjectPlus *self_v,
                                                         const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_MouseFocusSensor *self = static_cast<SCA_MouseFocusSensor *>(self_v);
  MT_Vector3 dir = self->RayTarget() - self->RaySource();
  if (MT_fuzzyZero(dir))
    dir.setValue(0, 0, 0);
  else
    dir.normalize();
  return PyObjectFrom(dir);
}

PyObject *SCA_MouseFocusSensor::pyattr_get_hit_object(EXP_PyObjectPlus *self_v,
                                                      const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_MouseFocusSensor *self = static_cast<SCA_MouseFocusSensor *>(self_v);

  if (self->m_hitObject)
    return self->m_hitObject->GetProxy();

  Py_RETURN_NONE;
}

PyObject *SCA_MouseFocusSensor::pyattr_get_hit_position(EXP_PyObjectPlus *self_v,
                                                        const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_MouseFocusSensor *self = static_cast<SCA_MouseFocusSensor *>(self_v);
  return PyObjectFrom(self->HitPosition());
}

PyObject *SCA_MouseFocusSensor::pyattr_get_hit_normal(EXP_PyObjectPlus *self_v,
                                                      const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_MouseFocusSensor *self = static_cast<SCA_MouseFocusSensor *>(self_v);
  return PyObjectFrom(self->HitNormal());
}

PyObject *SCA_MouseFocusSensor::pyattr_get_hit_uv(EXP_PyObjectPlus *self_v,
                                                  const EXP_PYATTRIBUTE_DEF *attrdef)
{
  SCA_MouseFocusSensor *self = static_cast<SCA_MouseFocusSensor *>(self_v);
  return PyObjectFrom(self->HitUV());
}

#endif  // WITH_PYTHON

/* eof */
