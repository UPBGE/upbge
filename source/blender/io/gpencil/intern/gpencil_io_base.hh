/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2020 Blender Foundation. All rights reserved. */
#pragma once

/** \file
 * \ingroup bgpencil
 */

#include "BLI_float4x4.hh"
#include "BLI_math_vec_types.hh"
#include "BLI_vector.hh"

#include "DNA_space_types.h" /* for FILE_MAX */

#include "gpencil_io.h"

struct Depsgraph;
struct Main;
struct Object;
struct RegionView3D;
struct Scene;

struct bGPDlayer;
struct bGPDstroke;
struct bGPdata;

using blender::Vector;

namespace blender::io::gpencil {

class GpencilIO {
 public:
  GpencilIO(const GpencilIOParams *iparams);

  void frame_number_set(int value);
  void prepare_camera_params(Scene *scene, const GpencilIOParams *iparams);

 protected:
  GpencilIOParams params_;

  bool invert_axis_[2];
  float4x4 diff_mat_;
  char filepath_[FILE_MAX];

  /* Used for sorting objects. */
  struct ObjectZ {
    float zdepth;
    struct Object *ob;
  };

  /** List of included objects. */
  blender::Vector<ObjectZ> ob_list_;

  /* Data for easy access. */
  struct Depsgraph *depsgraph_;
  struct bGPdata *gpd_;
  struct Main *bmain_;
  struct Scene *scene_;
  struct RegionView3D *rv3d_;

  int winx_, winy_;
  int render_x_, render_y_;
  float camera_ratio_;
  rctf camera_rect_;

  float2 offset_;

  int cfra_;

  float stroke_color_[4], fill_color_[4];

  /* Geometry functions. */
  /** Convert to screenspace. */
  bool gpencil_3D_point_to_screen_space(const float3 co, float2 &r_co);
  /** Convert to render space. */
  float2 gpencil_3D_point_to_render_space(const float3 co);
  /** Convert to 2D. */
  float2 gpencil_3D_point_to_2D(const float3 co);

  /** Get radius of point. */
  float stroke_point_radius_get(struct bGPDlayer *gpl, struct bGPDstroke *gps);
  /** Create a list of selected objects sorted from back to front */
  void create_object_list();

  bool is_camera_mode();

  float stroke_average_opacity_get();

  void prepare_layer_export_matrix(struct Object *ob, struct bGPDlayer *gpl);
  void prepare_stroke_export_colors(struct Object *ob, struct bGPDstroke *gps);

  /* Calculate selected strokes boundbox. */
  void selected_objects_boundbox_calc();
  void selected_objects_boundbox_get(rctf *boundbox);
  /**
   * Set file input_text full path.
   * \param filepath: Path of the file provided by save dialog.
   */
  void filepath_set(const char *filepath);

 private:
  float avg_opacity_;
  bool is_camera_;
  rctf select_boundbox_;

  /* Camera matrix. */
  float persmat_[4][4];
};

}  // namespace blender::io::gpencil
