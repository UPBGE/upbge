/* SPDX-License-Identifier: GPL-2.0-or-later
 * Adapted from the Blender Alembic importer implementation.
 * Modifications Copyright 2021 Tangent Animation. All rights reserved. */

#include "usd_reader_xform.h"

#include "BKE_constraint.h"
#include "BKE_lib_id.h"
#include "BKE_library.h"
#include "BKE_modifier.h"
#include "BKE_object.h"

#include "BLI_math_geom.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "DNA_cachefile_types.h"
#include "DNA_constraint_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_space_types.h" /* for FILE_MAX */

#include <pxr/base/gf/math.h>
#include <pxr/base/gf/matrix4f.h>

#include <pxr/usd/usdGeom/xform.h>

namespace blender::io::usd {

void USDXformReader::create_object(Main *bmain, const double /* motionSampleTime */)
{
  object_ = BKE_object_add_only_object(bmain, OB_EMPTY, name_.c_str());
  object_->empty_drawsize = 0.1f;
  object_->data = nullptr;
}

void USDXformReader::read_object_data(Main * /* bmain */, const double motionSampleTime)
{
  bool is_constant;
  float transform_from_usd[4][4];

  read_matrix(transform_from_usd, motionSampleTime, import_params_.scale, &is_constant);

  if (!is_constant) {
    bConstraint *con = BKE_constraint_add_for_object(
        object_, nullptr, CONSTRAINT_TYPE_TRANSFORM_CACHE);
    bTransformCacheConstraint *data = static_cast<bTransformCacheConstraint *>(con->data);

    std::string prim_path = use_parent_xform_ ? prim_.GetParent().GetPath().GetAsString() :
                                                prim_path_;

    BLI_strncpy(data->object_path, prim_path.c_str(), FILE_MAX);

    data->cache_file = settings_->cache_file;
    id_us_plus(&data->cache_file->id);
  }

  BKE_object_apply_mat4(object_, transform_from_usd, true, false);
}

void USDXformReader::read_matrix(float r_mat[4][4] /* local matrix */,
                                 const float time,
                                 const float scale,
                                 bool *r_is_constant)
{
  if (r_is_constant) {
    *r_is_constant = true;
  }

  unit_m4(r_mat);

  pxr::UsdGeomXformable xformable;

  if (use_parent_xform_) {
    xformable = pxr::UsdGeomXformable(prim_.GetParent());
  }
  else {
    xformable = pxr::UsdGeomXformable(prim_);
  }

  if (!xformable) {
    /* This might happen if the prim is a Scope. */
    return;
  }

  if (r_is_constant) {
    *r_is_constant = !xformable.TransformMightBeTimeVarying();
  }

  pxr::GfMatrix4d usd_local_xf;
  bool reset_xform_stack;
  xformable.GetLocalTransformation(&usd_local_xf, &reset_xform_stack, time);

  /* Convert the result to a float matrix. */
  pxr::GfMatrix4f mat4f = pxr::GfMatrix4f(usd_local_xf);
  mat4f.Get(r_mat);

  /* Apply global scaling and rotation only to root objects, parenting
   * will propagate it. */
  if ((scale != 1.0 || settings_->do_convert_mat) && is_root_xform_) {

    if (scale != 1.0f) {
      float scale_mat[4][4];
      scale_m4_fl(scale_mat, scale);
      mul_m4_m4m4(r_mat, scale_mat, r_mat);
    }

    if (settings_->do_convert_mat) {
      mul_m4_m4m4(r_mat, settings_->conversion_mat, r_mat);
    }
  }
}

bool USDXformReader::prim_has_xform_ops() const
{
  pxr::UsdGeomXformable xformable(prim_);

  if (!xformable) {
    /* This might happen if the prim is a Scope. */
    return false;
  }

  bool reset_xform_stack = false;

  return !xformable.GetOrderedXformOps(&reset_xform_stack).empty();
}

bool USDXformReader::is_root_xform_prim() const
{
  if (!prim_.IsValid()) {
    return false;
  }

  if (prim_.IsInPrototype()) {
    /* We don't consider prototypes to be root prims,
     * because we never want to apply global scaling
     * or rotations to the prototypes themselves. */
    return false;
  }

  if (prim_.IsA<pxr::UsdGeomXformable>()) {
    /* If this prim doesn't have an ancestor that's a
     * UsdGeomXformable, then it's a root prim.  Note
     * that it's not sufficient to only check the immediate
     * parent prim, since the immediate parent could be a
     * UsdGeomScope that has an xformable ancestor. */
    pxr::UsdPrim cur_parent = prim_.GetParent();

    if (use_parent_xform_) {
      cur_parent = cur_parent.GetParent();
    }

    while (cur_parent && !cur_parent.IsPseudoRoot()) {
      if (cur_parent.IsA<pxr::UsdGeomXformable>()) {
        return false;
      }
      cur_parent = cur_parent.GetParent();
    }

    /* We didn't find an xformable ancestor. */
    return true;
  }

  return false;
}

}  // namespace blender::io::usd
