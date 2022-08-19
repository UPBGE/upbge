/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup balembic
 */

#include "abc_reader_camera.h"
#include "abc_reader_transform.h"
#include "abc_util.h"

#include "DNA_camera_types.h"
#include "DNA_object_types.h"

#include "BKE_camera.h"
#include "BKE_object.h"

#include "BLI_math.h"

using Alembic::AbcGeom::CameraSample;
using Alembic::AbcGeom::ICamera;
using Alembic::AbcGeom::ICompoundProperty;
using Alembic::AbcGeom::IFloatProperty;
using Alembic::AbcGeom::ISampleSelector;
using Alembic::AbcGeom::kWrapExisting;

namespace blender::io::alembic {

AbcCameraReader::AbcCameraReader(const Alembic::Abc::IObject &object, ImportSettings &settings)
    : AbcObjectReader(object, settings)
{
  ICamera abc_cam(m_iobject, kWrapExisting);
  m_schema = abc_cam.getSchema();

  get_min_max_time(m_iobject, m_schema, m_min_time, m_max_time);
}

bool AbcCameraReader::valid() const
{
  return m_schema.valid();
}

bool AbcCameraReader::accepts_object_type(
    const Alembic::AbcCoreAbstract::ObjectHeader &alembic_header,
    const Object *const ob,
    const char **err_str) const
{
  if (!Alembic::AbcGeom::ICamera::matches(alembic_header)) {
    *err_str =
        "Object type mismatch, Alembic object path pointed to Camera when importing, but not any "
        "more.";
    return false;
  }

  if (ob->type != OB_CAMERA) {
    *err_str = "Object type mismatch, Alembic object path points to Camera.";
    return false;
  }

  return true;
}

void AbcCameraReader::readObjectData(Main *bmain, const ISampleSelector &sample_sel)
{
  Camera *bcam = static_cast<Camera *>(BKE_camera_add(bmain, m_data_name.c_str()));

  CameraSample cam_sample;
  m_schema.get(cam_sample, sample_sel);

  ICompoundProperty customDataContainer = m_schema.getUserProperties();

  if (customDataContainer.valid() && customDataContainer.getPropertyHeader("stereoDistance") &&
      customDataContainer.getPropertyHeader("eyeSeparation")) {
    IFloatProperty convergence_plane(customDataContainer, "stereoDistance");
    IFloatProperty eye_separation(customDataContainer, "eyeSeparation");

    bcam->stereo.interocular_distance = eye_separation.getValue(sample_sel);
    bcam->stereo.convergence_distance = convergence_plane.getValue(sample_sel);
  }

  const float lens = static_cast<float>(cam_sample.getFocalLength());
  const float apperture_x = static_cast<float>(cam_sample.getHorizontalAperture());
  const float apperture_y = static_cast<float>(cam_sample.getVerticalAperture());
  const float h_film_offset = static_cast<float>(cam_sample.getHorizontalFilmOffset());
  const float v_film_offset = static_cast<float>(cam_sample.getVerticalFilmOffset());
  const float film_aspect = apperture_x / apperture_y;

  bcam->lens = lens;
  bcam->sensor_x = apperture_x * 10;
  bcam->sensor_y = apperture_y * 10;
  bcam->shiftx = h_film_offset / apperture_x;
  bcam->shifty = v_film_offset / apperture_y / film_aspect;
  bcam->clip_start = max_ff(0.1f, static_cast<float>(cam_sample.getNearClippingPlane()));
  bcam->clip_end = static_cast<float>(cam_sample.getFarClippingPlane());
  bcam->dof.focus_distance = static_cast<float>(cam_sample.getFocusDistance());
  bcam->dof.aperture_fstop = static_cast<float>(cam_sample.getFStop());

  m_object = BKE_object_add_only_object(bmain, OB_CAMERA, m_object_name.c_str());
  m_object->data = bcam;
}

}  // namespace blender::io::alembic
