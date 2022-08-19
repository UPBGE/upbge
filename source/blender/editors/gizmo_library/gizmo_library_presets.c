/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgizmolib
 *
 * \name Gizmo Lib Presets
 *
 * \brief Preset shapes that can be drawn from any gizmo type.
 */

#include "BLI_math.h"

#include "DNA_object_types.h"

#include "BKE_context.h"

#include "GPU_matrix.h"
#include "GPU_select.h"

#include "DEG_depsgraph.h"

#include "WM_types.h"

#include "ED_view3d.h"

/* own includes */
#include "ED_gizmo_library.h"     /* own include */
#include "gizmo_library_intern.h" /* own include */

/* TODO: this is to be used by RNA. might move to ED_gizmo_library. */

/**
 * Given a single axis, orient the matrix to a different direction.
 */
static void single_axis_convert(int src_axis,
                                const float src_mat[4][4],
                                int dst_axis,
                                float dst_mat[4][4])
{
  copy_m4_m4(dst_mat, src_mat);
  if (src_axis == dst_axis) {
    return;
  }

  float rotmat[3][3];
  mat3_from_axis_conversion_single(src_axis, dst_axis, rotmat);
  transpose_m3(rotmat);
  mul_m4_m4m3(dst_mat, src_mat, rotmat);
}

/**
 * Use for all geometry.
 */
static void ed_gizmo_draw_preset_geometry(const struct wmGizmo *gz,
                                          const float mat[4][4],
                                          int select_id,
                                          const GizmoGeomInfo *info)
{
  const bool is_select = (select_id != -1);
  const bool is_highlight = is_select && (gz->state & WM_GIZMO_STATE_HIGHLIGHT) != 0;

  float color[4];
  gizmo_color_get(gz, is_highlight, color);

  if (is_select) {
    GPU_select_load_id(select_id);
  }

  GPU_matrix_push();
  GPU_matrix_mul(mat);
  wm_gizmo_geometryinfo_draw(info, is_select, color);
  GPU_matrix_pop();

  if (is_select) {
    GPU_select_load_id(-1);
  }
}

void ED_gizmo_draw_preset_box(const struct wmGizmo *gz, float mat[4][4], int select_id)
{
  ed_gizmo_draw_preset_geometry(gz, mat, select_id, &wm_gizmo_geom_data_cube);
}

void ED_gizmo_draw_preset_arrow(const struct wmGizmo *gz, float mat[4][4], int axis, int select_id)
{
  float mat_rotate[4][4];
  single_axis_convert(OB_POSZ, mat, axis, mat_rotate);
  ed_gizmo_draw_preset_geometry(gz, mat_rotate, select_id, &wm_gizmo_geom_data_arrow);
}

void ED_gizmo_draw_preset_circle(const struct wmGizmo *gz,
                                 float mat[4][4],
                                 int axis,
                                 int select_id)
{
  float mat_rotate[4][4];
  single_axis_convert(OB_POSZ, mat, axis, mat_rotate);
  ed_gizmo_draw_preset_geometry(gz, mat_rotate, select_id, &wm_gizmo_geom_data_dial);
}

void ED_gizmo_draw_preset_facemap(
    const bContext *C, const struct wmGizmo *gz, Object *ob, const int facemap, int select_id)
{
  /* Dependency graph is supposed to be evaluated prior to draw. */
  Depsgraph *depsgraph = CTX_data_expect_evaluated_depsgraph(C);
  const bool is_select = (select_id != -1);
  const bool is_highlight = is_select && (gz->state & WM_GIZMO_STATE_HIGHLIGHT) != 0;

  float color[4];
  gizmo_color_get(gz, is_highlight, color);

  if (is_select) {
    GPU_select_load_id(select_id);
  }

  GPU_matrix_push();
  GPU_matrix_mul(ob->obmat);
  ED_draw_object_facemap(depsgraph, ob, color, facemap);
  GPU_matrix_pop();

  if (is_select) {
    GPU_select_load_id(-1);
  }
}
