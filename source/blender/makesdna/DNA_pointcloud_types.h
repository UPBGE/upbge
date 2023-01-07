/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_ID.h"
#include "DNA_customdata_types.h"

#ifdef __cplusplus
#  include "BLI_math_vector_types.hh"
#endif

#ifdef __cplusplus
namespace blender {
template<typename T> class Span;
namespace bke {
class AttributeAccessor;
class MutableAttributeAccessor;
struct PointCloudRuntime;
}  // namespace bke
}  // namespace blender
using PointCloudRuntimeHandle = blender::bke::PointCloudRuntime;
#else
typedef struct PointCloudRuntimeHandle PointCloudRuntimeHandle;
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PointCloud {
  ID id;
  struct AnimData *adt; /* animation data (must be immediately after id) */

  int flag;

  /* Geometry */
  int totpoint;

  /* Custom Data */
  struct CustomData pdata;
  int attributes_active_index;
  int _pad4;

  /* Material */
  struct Material **mat;
  short totcol;
  short _pad3[3];

#ifdef __cplusplus
  blender::bke::AttributeAccessor attributes() const;
  blender::bke::MutableAttributeAccessor attributes_for_write();

  void tag_positions_changed();
  void tag_radii_changed();

  bool bounds_min_max(blender::float3 &min, blender::float3 &max) const;
#endif

  PointCloudRuntimeHandle *runtime;

  /* Draw Cache */
  void *batch_cache;
} PointCloud;

/** #PointCloud.flag */
enum {
  PT_DS_EXPAND = (1 << 0),
};

/* Only one material supported currently. */
#define POINTCLOUD_MATERIAL_NR 1

#ifdef __cplusplus
}
#endif
