/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2019 Blender Foundation. All rights reserved. */
#include "usd.h"

#include "usd_hierarchy_iterator.h"
#include "usd_writer_abstract.h"
#include "usd_writer_camera.h"
#include "usd_writer_hair.h"
#include "usd_writer_light.h"
#include "usd_writer_mesh.h"
#include "usd_writer_metaball.h"
#include "usd_writer_transform.h"
#include "usd_writer_volume.h"

#include <string>

#include <pxr/base/tf/stringUtils.h>

#include "BKE_duplilist.h"

#include "BLI_assert.h"
#include "BLI_utildefines.h"

#include "DEG_depsgraph_query.h"

#include "DNA_ID.h"
#include "DNA_layer_types.h"
#include "DNA_object_types.h"

namespace blender::io::usd {

USDHierarchyIterator::USDHierarchyIterator(Main *bmain,
                                           Depsgraph *depsgraph,
                                           pxr::UsdStageRefPtr stage,
                                           const USDExportParams &params)
    : AbstractHierarchyIterator(bmain, depsgraph), stage_(stage), params_(params)
{
}

bool USDHierarchyIterator::mark_as_weak_export(const Object *object) const
{
  if (params_.selected_objects_only && (object->base_flag & BASE_SELECTED) == 0) {
    return true;
  }
  return false;
}

void USDHierarchyIterator::release_writer(AbstractHierarchyWriter *writer)
{
  delete static_cast<USDAbstractWriter *>(writer);
}

std::string USDHierarchyIterator::make_valid_name(const std::string &name) const
{
  return pxr::TfMakeValidIdentifier(name);
}

void USDHierarchyIterator::set_export_frame(float frame_nr)
{
  /* The USD stage is already set up to have FPS time-codes per frame. */
  export_time_ = pxr::UsdTimeCode(frame_nr);
}

std::string USDHierarchyIterator::get_export_file_path() const
{
  /* Returns the same path that was passed to `stage_` object during it's creation (via
   * `pxr::UsdStage::CreateNew` function). */
  const pxr::SdfLayerHandle root_layer = stage_->GetRootLayer();
  const std::string usd_export_file_path = root_layer->GetRealPath();
  return usd_export_file_path;
}

const pxr::UsdTimeCode &USDHierarchyIterator::get_export_time_code() const
{
  return export_time_;
}

USDExporterContext USDHierarchyIterator::create_usd_export_context(const HierarchyContext *context)
{
  return USDExporterContext{
      bmain_, depsgraph_, stage_, pxr::SdfPath(context->export_path), this, params_};
}

AbstractHierarchyWriter *USDHierarchyIterator::create_transform_writer(
    const HierarchyContext *context)
{
  return new USDTransformWriter(create_usd_export_context(context));
}

AbstractHierarchyWriter *USDHierarchyIterator::create_data_writer(const HierarchyContext *context)
{
  USDExporterContext usd_export_context = create_usd_export_context(context);
  USDAbstractWriter *data_writer = nullptr;

  switch (context->object->type) {
    case OB_MESH:
      data_writer = new USDMeshWriter(usd_export_context);
      break;
    case OB_CAMERA:
      data_writer = new USDCameraWriter(usd_export_context);
      break;
    case OB_LAMP:
      data_writer = new USDLightWriter(usd_export_context);
      break;
    case OB_MBALL:
      data_writer = new USDMetaballWriter(usd_export_context);
      break;
    case OB_VOLUME:
      data_writer = new USDVolumeWriter(usd_export_context);
      break;

    case OB_EMPTY:
    case OB_CURVES_LEGACY:
    case OB_SURF:
    case OB_FONT:
    case OB_SPEAKER:
    case OB_LIGHTPROBE:
    case OB_LATTICE:
    case OB_ARMATURE:
    case OB_GPENCIL:
      return nullptr;
    case OB_TYPE_MAX:
      BLI_assert_msg(0, "OB_TYPE_MAX should not be used");
      return nullptr;
  }

  if (!data_writer->is_supported(context)) {
    delete data_writer;
    return nullptr;
  }

  return data_writer;
}

AbstractHierarchyWriter *USDHierarchyIterator::create_hair_writer(const HierarchyContext *context)
{
  if (!params_.export_hair) {
    return nullptr;
  }
  return new USDHairWriter(create_usd_export_context(context));
}

AbstractHierarchyWriter *USDHierarchyIterator::create_particle_writer(
    const HierarchyContext *UNUSED(context))
{
  return nullptr;
}

}  // namespace blender::io::usd
