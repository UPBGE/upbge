/* SPDX-FileCopyrightText: 2012 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <cstddef>
#include <cstring>
#include <optional>

#include "CLG_log.h"

#include "MEM_guardedalloc.h"

#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.hh"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "DNA_defaults.h"
#include "DNA_mask_types.h"
#include "DNA_movieclip_types.h"
#include "DNA_object_types.h"

#include "BKE_animsys.h"
#include "BKE_curve.hh"
#include "BKE_idtype.hh"

#include "BKE_anim_data.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_main.hh"
#include "BKE_mask.h"
#include "BKE_movieclip.h"
#include "BKE_tracking.h"

#include "DEG_depsgraph_build.hh"

#include "DRW_engine.hh"

#include "BLO_read_write.hh"

static CLG_LogRef LOG = {"mask"};

/** Reset runtime mask fields when data-block is being initialized. */
static void mask_runtime_reset(Mask *mask)
{
  mask->runtime.last_update = 0;
}

static void mask_copy_data(Main * /*bmain*/,
                           std::optional<Library *> /*owner_library*/,
                           ID *id_dst,
                           const ID *id_src,
                           const int /*flag*/)
{
  Mask *mask_dst = (Mask *)id_dst;
  const Mask *mask_src = (const Mask *)id_src;

  BLI_listbase_clear(&mask_dst->masklayers);

  /* TODO: add unused flag to those as well. */
  BKE_mask_layer_copy_list(&mask_dst->masklayers, &mask_src->masklayers);

  /* enable fake user by default */
  id_fake_user_set(&mask_dst->id);
}

static void mask_free_data(ID *id)
{
  Mask *mask = (Mask *)id;

  /* free mask data */
  BKE_mask_layer_free_list(&mask->masklayers);
}

static void mask_foreach_id(ID *id, LibraryForeachIDData *data)
{
  Mask *mask = (Mask *)id;

  LISTBASE_FOREACH (MaskLayer *, mask_layer, &mask->masklayers) {
    LISTBASE_FOREACH (MaskSpline *, mask_spline, &mask_layer->splines) {
      BKE_LIB_FOREACHID_PROCESS_ID(data, mask_spline->parent.id, IDWALK_CB_USER);
      for (int i = 0; i < mask_spline->tot_point; i++) {
        MaskSplinePoint *point = &mask_spline->points[i];
        BKE_LIB_FOREACHID_PROCESS_ID(data, point->parent.id, IDWALK_CB_USER);
      }
    }
  }
}

static void mask_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  Mask *mask = (Mask *)id;

  BLO_write_id_struct(writer, Mask, id_address, &mask->id);
  BKE_id_blend_write(writer, &mask->id);

  LISTBASE_FOREACH (MaskLayer *, masklay, &mask->masklayers) {
    BLO_write_struct(writer, MaskLayer, masklay);

    LISTBASE_FOREACH (MaskSpline *, spline, &masklay->splines) {
      int i;

      MaskSplinePoint *points_deform = spline->points_deform;
      spline->points_deform = nullptr;

      BLO_write_struct(writer, MaskSpline, spline);
      BLO_write_struct_array(writer, MaskSplinePoint, spline->tot_point, spline->points);

      spline->points_deform = points_deform;

      for (i = 0; i < spline->tot_point; i++) {
        MaskSplinePoint *point = &spline->points[i];

        if (point->tot_uw) {
          BLO_write_struct_array(writer, MaskSplinePointUW, point->tot_uw, point->uw);
        }
      }
    }

    LISTBASE_FOREACH (MaskLayerShape *, masklay_shape, &masklay->splines_shapes) {
      BLO_write_struct(writer, MaskLayerShape, masklay_shape);
      BLO_write_float_array(
          writer, masklay_shape->tot_vert * MASK_OBJECT_SHAPE_ELEM_SIZE, masklay_shape->data);
    }
  }
}

static void mask_blend_read_data(BlendDataReader *reader, ID *id)
{
  Mask *mask = (Mask *)id;

  BLO_read_struct_list(reader, MaskLayer, &mask->masklayers);

  LISTBASE_FOREACH (MaskLayer *, masklay, &mask->masklayers) {
    /* Can't use #newdataadr since it's a pointer within an array. */
    MaskSplinePoint *act_point_search = nullptr;

    BLO_read_struct_list(reader, MaskSpline, &masklay->splines);

    LISTBASE_FOREACH (MaskSpline *, spline, &masklay->splines) {
      MaskSplinePoint *points_old = spline->points;

      BLO_read_struct_array(reader, MaskSplinePoint, spline->tot_point, &spline->points);

      for (int i = 0; i < spline->tot_point; i++) {
        MaskSplinePoint *point = &spline->points[i];

        if (point->tot_uw) {
          BLO_read_struct_array(reader, MaskSplinePointUW, point->tot_uw, &point->uw);
        }
      }

      /* detect active point */
      if ((act_point_search == nullptr) && (masklay->act_point >= points_old) &&
          (masklay->act_point < points_old + spline->tot_point))
      {
        act_point_search = &spline->points[masklay->act_point - points_old];
      }
    }

    BLO_read_struct_list(reader, MaskLayerShape, &masklay->splines_shapes);

    LISTBASE_FOREACH (MaskLayerShape *, masklay_shape, &masklay->splines_shapes) {
      BLO_read_float_array(
          reader, masklay_shape->tot_vert * MASK_OBJECT_SHAPE_ELEM_SIZE, &masklay_shape->data);
    }

    BLO_read_struct(reader, MaskSpline, &masklay->act_spline);
    masklay->act_point = act_point_search;
  }

  mask_runtime_reset(mask);
}

IDTypeInfo IDType_ID_MSK = {
    /*id_code*/ Mask::id_type,
    /*id_filter*/ FILTER_ID_MSK,
    /*dependencies_id_types*/ FILTER_ID_MC, /* WARNING! mask->parent.id, not typed. */
    /*main_listbase_index*/ INDEX_ID_MSK,
    /*struct_size*/ sizeof(Mask),
    /*name*/ "Mask",
    /*name_plural*/ N_("masks"),
    /*translation_context*/ BLT_I18NCONTEXT_ID_MASK,
    /*flags*/ IDTYPE_FLAGS_APPEND_IS_REUSABLE,
    /*asset_type_info*/ nullptr,

    /*init_data*/ nullptr,
    /*copy_data*/ mask_copy_data,
    /*free_data*/ mask_free_data,
    /*make_local*/ nullptr,
    /*foreach_id*/ mask_foreach_id,
    /*foreach_cache*/ nullptr,
    /*foreach_path*/ nullptr,
    /*owner_pointer_get*/ nullptr,

    /*blend_write*/ mask_blend_write,
    /*blend_read_data*/ mask_blend_read_data,
    /*blend_read_after_liblink*/ nullptr,

    /*blend_read_undo_preserve*/ nullptr,

    /*lib_override_apply_post*/ nullptr,
};

static struct {
  ListBase splines;
  GHash *id_hash;
} mask_clipboard = {{nullptr}};

static MaskSplinePoint *mask_spline_point_next(MaskSpline *spline,
                                               MaskSplinePoint *points_array,
                                               MaskSplinePoint *point)
{
  if (point == &points_array[spline->tot_point - 1]) {
    if (spline->flag & MASK_SPLINE_CYCLIC) {
      return &points_array[0];
    }

    return nullptr;
  }

  return point + 1;
}

static MaskSplinePoint *mask_spline_point_prev(MaskSpline *spline,
                                               MaskSplinePoint *points_array,
                                               MaskSplinePoint *point)
{
  if (point == points_array) {
    if (spline->flag & MASK_SPLINE_CYCLIC) {
      return &points_array[spline->tot_point - 1];
    }

    return nullptr;
  }

  return point - 1;
}

BezTriple *BKE_mask_spline_point_next_bezt(MaskSpline *spline,
                                           MaskSplinePoint *points_array,
                                           MaskSplinePoint *point)
{
  if (point == &points_array[spline->tot_point - 1]) {
    if (spline->flag & MASK_SPLINE_CYCLIC) {
      return &(points_array[0].bezt);
    }

    return nullptr;
  }

  return &(point + 1)->bezt;
}

MaskSplinePoint *BKE_mask_spline_point_array(MaskSpline *spline)
{
  return spline->points_deform ? spline->points_deform : spline->points;
}

MaskSplinePoint *BKE_mask_spline_point_array_from_point(MaskSpline *spline,
                                                        const MaskSplinePoint *point_ref)
{
  if ((point_ref >= spline->points) && (point_ref < &spline->points[spline->tot_point])) {
    return spline->points;
  }

  if ((point_ref >= spline->points_deform) &&
      (point_ref < &spline->points_deform[spline->tot_point]))
  {
    return spline->points_deform;
  }

  BLI_assert_msg(0, "wrong array");
  return nullptr;
}

/* mask layers */

MaskLayer *BKE_mask_layer_new(Mask *mask, const char *name)
{
  MaskLayer *masklay = MEM_callocN<MaskLayer>(__func__);

  STRNCPY_UTF8(masklay->name, name && name[0] ? name : DATA_("MaskLayer"));

  BLI_addtail(&mask->masklayers, masklay);

  BKE_mask_layer_unique_name(mask, masklay);

  mask->masklay_tot++;

  masklay->blend = MASK_BLEND_MERGE_ADD;
  masklay->alpha = 1.0f;
  masklay->flag = MASK_LAYERFLAG_FILL_DISCRETE | MASK_LAYERFLAG_FILL_OVERLAP;

  return masklay;
}

MaskLayer *BKE_mask_layer_active(Mask *mask)
{
  return static_cast<MaskLayer *>(BLI_findlink(&mask->masklayers, mask->masklay_act));
}

void BKE_mask_layer_active_set(Mask *mask, MaskLayer *masklay)
{
  mask->masklay_act = BLI_findindex(&mask->masklayers, masklay);
}

void BKE_mask_layer_remove(Mask *mask, MaskLayer *masklay)
{
  BLI_remlink(&mask->masklayers, masklay);
  BKE_mask_layer_free(masklay);

  mask->masklay_tot--;

  if (mask->masklay_act >= mask->masklay_tot) {
    mask->masklay_act = mask->masklay_tot - 1;
  }
}

void BKE_mask_layer_unique_name(Mask *mask, MaskLayer *masklay)
{
  BLI_uniquename(&mask->masklayers,
                 masklay,
                 DATA_("MaskLayer"),
                 '.',
                 offsetof(MaskLayer, name),
                 sizeof(masklay->name));
}

void BKE_mask_layer_rename(Mask *mask,
                           MaskLayer *masklay,
                           const char *oldname,
                           const char *newname)
{
  STRNCPY_UTF8(masklay->name, newname);

  BKE_mask_layer_unique_name(mask, masklay);

  /* now fix animation paths */
  BKE_animdata_fix_paths_rename_all(&mask->id, "layers", oldname, masklay->name);
}

MaskLayer *BKE_mask_layer_copy(const MaskLayer *masklay)
{
  MaskLayer *masklay_new = MEM_callocN<MaskLayer>("new mask layer");

  STRNCPY_UTF8(masklay_new->name, masklay->name);

  masklay_new->alpha = masklay->alpha;
  masklay_new->blend = masklay->blend;
  masklay_new->blend_flag = masklay->blend_flag;
  masklay_new->flag = masklay->flag;
  masklay_new->falloff = masklay->falloff;
  masklay_new->visibility_flag = masklay->visibility_flag;

  LISTBASE_FOREACH (MaskSpline *, spline, &masklay->splines) {
    MaskSpline *spline_new = BKE_mask_spline_copy(spline);

    BLI_addtail(&masklay_new->splines, spline_new);

    if (spline == masklay->act_spline) {
      masklay_new->act_spline = spline_new;
    }

    if (masklay->act_point >= spline->points &&
        masklay->act_point < spline->points + spline->tot_point)
    {
      const size_t point_index = masklay->act_point - spline->points;
      masklay_new->act_point = spline_new->points + point_index;
    }
  }

  /* correct animation */
  if (masklay->splines_shapes.first) {
    LISTBASE_FOREACH (MaskLayerShape *, masklay_shape, &masklay->splines_shapes) {
      MaskLayerShape *masklay_shape_new = MEM_callocN<MaskLayerShape>("new mask layer shape");

      masklay_shape_new->data = static_cast<float *>(MEM_dupallocN(masklay_shape->data));
      masklay_shape_new->tot_vert = masklay_shape->tot_vert;
      masklay_shape_new->flag = masklay_shape->flag;
      masklay_shape_new->frame = masklay_shape->frame;

      BLI_addtail(&masklay_new->splines_shapes, masklay_shape_new);
    }
  }

  return masklay_new;
}

void BKE_mask_layer_copy_list(ListBase *masklayers_new, const ListBase *masklayers)
{
  LISTBASE_FOREACH (MaskLayer *, layer, masklayers) {
    MaskLayer *layer_new = BKE_mask_layer_copy(layer);

    BLI_addtail(masklayers_new, layer_new);
  }
}

/* splines */

MaskSpline *BKE_mask_spline_add(MaskLayer *masklay)
{
  MaskSpline *spline = MEM_callocN<MaskSpline>("new mask spline");

  BLI_addtail(&masklay->splines, spline);

  /* spline shall have one point at least */
  spline->points = MEM_callocN<MaskSplinePoint>("new mask spline point");
  spline->tot_point = 1;

  /* cyclic shapes are more usually used */
  /* Disable because its not so nice for drawing. could be done differently. */
#if 0
  spline->flag |= MASK_SPLINE_CYCLIC;
#endif

  spline->weight_interp = MASK_SPLINE_INTERP_EASE;

  BKE_mask_parent_init(&spline->parent);

  return spline;
}

bool BKE_mask_spline_remove(MaskLayer *mask_layer, MaskSpline *spline)
{
  if (BLI_remlink_safe(&mask_layer->splines, spline) == false) {
    return false;
  }

  BKE_mask_spline_free(spline);

  return true;
}

void BKE_mask_point_direction_switch(MaskSplinePoint *point)
{
  const int tot_uw = point->tot_uw;
  const int tot_uw_half = tot_uw / 2;

  float co_tmp[2];

  /* swap handles */
  copy_v2_v2(co_tmp, point->bezt.vec[0]);
  copy_v2_v2(point->bezt.vec[0], point->bezt.vec[2]);
  copy_v2_v2(point->bezt.vec[2], co_tmp);
  /* in this case the flags are unlikely to be different but swap anyway */
  std::swap(point->bezt.f1, point->bezt.f3);
  std::swap(point->bezt.h1, point->bezt.h2);

  /* swap UW's */
  if (tot_uw > 1) {
    /* count */
    for (int i = 0; i < tot_uw_half; i++) {
      MaskSplinePointUW *uw_a = &point->uw[i];
      MaskSplinePointUW *uw_b = &point->uw[tot_uw - (i + 1)];
      std::swap(*uw_a, *uw_b);
    }
  }

  for (int i = 0; i < tot_uw; i++) {
    MaskSplinePointUW *uw = &point->uw[i];
    uw->u = 1.0f - uw->u;
  }
}

void BKE_mask_spline_direction_switch(MaskLayer *masklay, MaskSpline *spline)
{
  const int tot_point = spline->tot_point;
  const int tot_point_half = tot_point / 2;
  int i, i_prev;

  if (tot_point < 2) {
    return;
  }

  /* count */
  for (i = 0; i < tot_point_half; i++) {
    MaskSplinePoint *point_a = &spline->points[i];
    MaskSplinePoint *point_b = &spline->points[tot_point - (i + 1)];
    std::swap(*point_a, *point_b);
  }

  /* correct UW's */
  i_prev = tot_point - 1;
  for (i = 0; i < tot_point; i++) {

    BKE_mask_point_direction_switch(&spline->points[i]);

    std::swap(spline->points[i].uw, spline->points[i_prev].uw);
    std::swap(spline->points[i].tot_uw, spline->points[i_prev].tot_uw);

    i_prev = i;
  }

  /* correct animation */
  if (masklay->splines_shapes.first) {
    const int spline_index = BKE_mask_layer_shape_spline_to_index(masklay, spline);

    LISTBASE_FOREACH (MaskLayerShape *, masklay_shape, &masklay->splines_shapes) {
      MaskLayerShapeElem *fp_arr = (MaskLayerShapeElem *)masklay_shape->data;

      for (i = 0; i < tot_point_half; i++) {
        MaskLayerShapeElem *fp_a = &fp_arr[spline_index + (i)];
        MaskLayerShapeElem *fp_b = &fp_arr[spline_index + (tot_point - (i + 1))];
        std::swap(*fp_a, *fp_b);
      }
    }
  }
}

float BKE_mask_spline_project_co(MaskSpline *spline,
                                 MaskSplinePoint *point,
                                 float start_u,
                                 const float co[2],
                                 const eMaskSign sign)
{
  const float proj_eps = 1e-3;
  const float proj_eps_sq = proj_eps * proj_eps;
  const int N = 1000;
  float u = -1.0f, du = 1.0f / N, u1 = start_u, u2 = start_u;
  float ang = -1.0f;

  BLI_assert(abs(sign) <= 1); /* (-1, 0, 1) */

  while (u1 > 0.0f || u2 < 1.0f) {
    float n1[2], n2[2], co1[2], co2[2];
    float v1[2], v2[2];
    float ang1, ang2;

    if (u1 >= 0.0f) {
      BKE_mask_point_segment_co(spline, point, u1, co1);
      BKE_mask_point_normal(spline, point, u1, n1);
      sub_v2_v2v2(v1, co, co1);

      if ((sign == MASK_PROJ_ANY) || ((sign == MASK_PROJ_NEG) && (dot_v2v2(v1, n1) <= 0.0f)) ||
          ((sign == MASK_PROJ_POS) && (dot_v2v2(v1, n1) >= 0.0f)))
      {

        if (len_squared_v2(v1) > proj_eps_sq) {
          ang1 = angle_v2v2(v1, n1);
          if (ang1 > float(M_PI_2)) {
            ang1 = float(M_PI) - ang1;
          }

          if (ang < 0.0f || ang1 < ang) {
            ang = ang1;
            u = u1;
          }
        }
        else {
          u = u1;
          break;
        }
      }
    }

    if (u2 <= 1.0f) {
      BKE_mask_point_segment_co(spline, point, u2, co2);
      BKE_mask_point_normal(spline, point, u2, n2);
      sub_v2_v2v2(v2, co, co2);

      if ((sign == MASK_PROJ_ANY) || ((sign == MASK_PROJ_NEG) && (dot_v2v2(v2, n2) <= 0.0f)) ||
          ((sign == MASK_PROJ_POS) && (dot_v2v2(v2, n2) >= 0.0f)))
      {

        if (len_squared_v2(v2) > proj_eps_sq) {
          ang2 = angle_v2v2(v2, n2);
          if (ang2 > float(M_PI_2)) {
            ang2 = float(M_PI) - ang2;
          }

          if (ang2 < ang) {
            ang = ang2;
            u = u2;
          }
        }
        else {
          u = u2;
          break;
        }
      }
    }

    u1 -= du;
    u2 += du;
  }

  return u;
}

/* point */

eMaskhandleMode BKE_mask_point_handles_mode_get(const MaskSplinePoint *point)
{
  const BezTriple *bezt = &point->bezt;

  if (bezt->h1 == bezt->h2 && bezt->h1 == HD_ALIGN) {
    return MASK_HANDLE_MODE_STICK;
  }

  return MASK_HANDLE_MODE_INDIVIDUAL_HANDLES;
}

void BKE_mask_point_handle(const MaskSplinePoint *point,
                           eMaskWhichHandle which_handle,
                           float r_handle[2])
{
  const BezTriple *bezt = &point->bezt;

  if (which_handle == MASK_WHICH_HANDLE_STICK) {
    float vec[2];

    sub_v2_v2v2(vec, bezt->vec[0], bezt->vec[1]);

    r_handle[0] = (bezt->vec[1][0] + vec[1]);
    r_handle[1] = (bezt->vec[1][1] - vec[0]);
  }
  else if (which_handle == MASK_WHICH_HANDLE_LEFT) {
    copy_v2_v2(r_handle, bezt->vec[0]);
  }
  else if (which_handle == MASK_WHICH_HANDLE_RIGHT) {
    copy_v2_v2(r_handle, bezt->vec[2]);
  }
  else {
    BLI_assert_msg(0, "Unknown handle passed to BKE_mask_point_handle");
  }
}

void BKE_mask_point_set_handle(MaskSplinePoint *point,
                               eMaskWhichHandle which_handle,
                               float loc[2],
                               bool keep_direction,
                               float orig_handle[2],
                               float orig_vec[3][3])
{
  BezTriple *bezt = &point->bezt;

  if (which_handle == MASK_WHICH_HANDLE_STICK) {
    float v1[2], v2[2], vec[2];
    if (keep_direction) {
      sub_v2_v2v2(v1, loc, orig_vec[1]);
      sub_v2_v2v2(v2, orig_handle, orig_vec[1]);

      project_v2_v2v2(vec, v1, v2);

      if (dot_v2v2(v2, vec) > 0) {
        float len = len_v2(vec);

        sub_v2_v2v2(v1, orig_vec[0], orig_vec[1]);

        mul_v2_fl(v1, len / len_v2(v1));

        add_v2_v2v2(bezt->vec[0], bezt->vec[1], v1);
        sub_v2_v2v2(bezt->vec[2], bezt->vec[1], v1);
      }
      else {
        copy_v3_v3(bezt->vec[0], bezt->vec[1]);
        copy_v3_v3(bezt->vec[2], bezt->vec[1]);
      }
    }
    else {
      sub_v2_v2v2(v1, loc, bezt->vec[1]);

      v2[0] = -v1[1];
      v2[1] = v1[0];

      add_v2_v2v2(bezt->vec[0], bezt->vec[1], v2);
      sub_v2_v2v2(bezt->vec[2], bezt->vec[1], v2);
    }
  }
  else if (which_handle == MASK_WHICH_HANDLE_LEFT) {
    copy_v2_v2(bezt->vec[0], loc);
  }
  else if (which_handle == MASK_WHICH_HANDLE_RIGHT) {
    copy_v2_v2(bezt->vec[2], loc);
  }
  else {
    BLI_assert_msg(0, "unknown handle passed to BKE_mask_point_set_handle");
  }
}

void BKE_mask_point_segment_co(MaskSpline *spline, MaskSplinePoint *point, float u, float co[2])
{
  MaskSplinePoint *points_array = BKE_mask_spline_point_array_from_point(spline, point);

  BezTriple *bezt = &point->bezt, *bezt_next;

  bezt_next = BKE_mask_spline_point_next_bezt(spline, points_array, point);

  if (!bezt_next) {
    copy_v2_v2(co, bezt->vec[1]);
    return;
  }

  interp_v2_v2v2v2v2_cubic(
      co, bezt->vec[1], bezt->vec[2], bezt_next->vec[0], bezt_next->vec[1], u);
}

BLI_INLINE void orthogonal_direction_get(const float vec[2], float result[2])
{
  result[0] = -vec[1];
  result[1] = vec[0];
  normalize_v2(result);
}

void BKE_mask_point_normal(MaskSpline *spline, MaskSplinePoint *point, float u, float n[2])
{
  /* TODO(sergey): This function will re-calculate loads of stuff again and again
   *               when differentiating feather points. This might be easily cached
   *               in the callee function for this case. */

  MaskSplinePoint *point_prev, *point_next;

  /* TODO(sergey): This actually depends on a resolution. */
  const float du = 0.05f;

  BKE_mask_get_handle_point_adjacent(spline, point, &point_prev, &point_next);

  if (u - du < 0.0f && point_prev == nullptr) {
    float co[2], dir[2];
    BKE_mask_point_segment_co(spline, point, u + du, co);
    sub_v2_v2v2(dir, co, point->bezt.vec[1]);
    orthogonal_direction_get(dir, n);
  }
  else if (u + du > 1.0f && point_next == nullptr) {
    float co[2], dir[2];
    BKE_mask_point_segment_co(spline, point, u - du, co);
    sub_v2_v2v2(dir, point->bezt.vec[1], co);
    orthogonal_direction_get(dir, n);
  }
  else {
    float prev_co[2], next_co[2], co[2];
    float dir1[2], dir2[2], dir[2];

    if (u - du < 0.0f) {
      BKE_mask_point_segment_co(spline, point_prev, 1.0f + (u - du), prev_co);
    }
    else {
      BKE_mask_point_segment_co(spline, point, u - du, prev_co);
    }

    BKE_mask_point_segment_co(spline, point, u, co);

    if (u + du > 1.0f) {
      BKE_mask_point_segment_co(spline, point_next, u + du - 1.0f, next_co);
    }
    else {
      BKE_mask_point_segment_co(spline, point, u + du, next_co);
    }

    sub_v2_v2v2(dir1, co, prev_co);
    sub_v2_v2v2(dir2, next_co, co);

    normalize_v2(dir1);
    normalize_v2(dir2);
    add_v2_v2v2(dir, dir1, dir2);

    orthogonal_direction_get(dir, n);
  }
}

static float mask_point_interp_weight(BezTriple *bezt, BezTriple *bezt_next, const float u)
{
  return (bezt->weight * (1.0f - u)) + (bezt_next->weight * u);
}

float BKE_mask_point_weight_scalar(MaskSpline *spline, MaskSplinePoint *point, const float u)
{
  MaskSplinePoint *points_array = BKE_mask_spline_point_array_from_point(spline, point);
  BezTriple *bezt = &point->bezt, *bezt_next;

  bezt_next = BKE_mask_spline_point_next_bezt(spline, points_array, point);

  if (!bezt_next) {
    return bezt->weight;
  }
  if (u <= 0.0f) {
    return bezt->weight;
  }
  if (u >= 1.0f) {
    return bezt_next->weight;
  }

  return mask_point_interp_weight(bezt, bezt_next, u);
}

float BKE_mask_point_weight(MaskSpline *spline, MaskSplinePoint *point, const float u)
{
  MaskSplinePoint *points_array = BKE_mask_spline_point_array_from_point(spline, point);
  BezTriple *bezt = &point->bezt, *bezt_next;

  bezt_next = BKE_mask_spline_point_next_bezt(spline, points_array, point);

  if (!bezt_next) {
    return bezt->weight;
  }
  if (u <= 0.0f) {
    return bezt->weight;
  }
  if (u >= 1.0f) {
    return bezt_next->weight;
  }

  float cur_u = 0.0f, cur_w = 0.0f, next_u = 0.0f, next_w = 0.0f, fac; /* Quite warnings */

  for (int i = 0; i <= point->tot_uw; i++) {
    if (i == 0) {
      cur_u = 0.0f;
      cur_w = 1.0f; /* mask_point_interp_weight will scale it */
    }
    else {
      cur_u = point->uw[i - 1].u;
      cur_w = point->uw[i - 1].w;
    }

    if (i == point->tot_uw) {
      next_u = 1.0f;
      next_w = 1.0f; /* mask_point_interp_weight will scale it */
    }
    else {
      next_u = point->uw[i].u;
      next_w = point->uw[i].w;
    }

    if (u >= cur_u && u <= next_u) {
      break;
    }
  }

  fac = (u - cur_u) / (next_u - cur_u);

  cur_w *= mask_point_interp_weight(bezt, bezt_next, cur_u);
  next_w *= mask_point_interp_weight(bezt, bezt_next, next_u);

  if (spline->weight_interp == MASK_SPLINE_INTERP_EASE) {
    return cur_w + (next_w - cur_w) * (3.0f * fac * fac - 2.0f * fac * fac * fac);
  }

  return (1.0f - fac) * cur_w + fac * next_w;
}

MaskSplinePointUW *BKE_mask_point_sort_uw(MaskSplinePoint *point, MaskSplinePointUW *uw)
{
  if (point->tot_uw > 1) {
    int idx = uw - point->uw;

    if (idx > 0 && point->uw[idx - 1].u > uw->u) {
      while (idx > 0 && point->uw[idx - 1].u > point->uw[idx].u) {
        std::swap(point->uw[idx - 1], point->uw[idx]);
        idx--;
      }
    }

    if (idx < point->tot_uw - 1 && point->uw[idx + 1].u < uw->u) {
      while (idx < point->tot_uw - 1 && point->uw[idx + 1].u < point->uw[idx].u) {
        std::swap(point->uw[idx + 1], point->uw[idx]);
        idx++;
      }
    }

    return &point->uw[idx];
  }

  return uw;
}

void BKE_mask_point_add_uw(MaskSplinePoint *point, float u, float w)
{
  if (!point->uw) {
    point->uw = MEM_callocN<MaskSplinePointUW>("mask point uw");
  }
  else {
    point->uw = static_cast<MaskSplinePointUW *>(
        MEM_reallocN(point->uw, (point->tot_uw + 1) * sizeof(*point->uw)));
  }

  point->uw[point->tot_uw].u = u;
  point->uw[point->tot_uw].w = w;
  point->uw[point->tot_uw].flag = 0;

  point->tot_uw++;

  BKE_mask_point_sort_uw(point, &point->uw[point->tot_uw - 1]);
}

void BKE_mask_point_select_set(MaskSplinePoint *point, const bool do_select)
{
  if (do_select) {
    MASKPOINT_SEL_ALL(point);
  }
  else {
    MASKPOINT_DESEL_ALL(point);
  }

  for (int i = 0; i < point->tot_uw; i++) {
    if (do_select) {
      point->uw[i].flag |= SELECT;
    }
    else {
      point->uw[i].flag &= ~SELECT;
    }
  }
}

void BKE_mask_point_select_set_handle(MaskSplinePoint *point,
                                      const eMaskWhichHandle which_handle,
                                      const bool do_select)
{
  if (do_select) {
    if (ELEM(which_handle, MASK_WHICH_HANDLE_STICK, MASK_WHICH_HANDLE_BOTH)) {
      point->bezt.f1 |= SELECT;
      point->bezt.f3 |= SELECT;
    }
    else if (which_handle == MASK_WHICH_HANDLE_LEFT) {
      point->bezt.f1 |= SELECT;
    }
    else if (which_handle == MASK_WHICH_HANDLE_RIGHT) {
      point->bezt.f3 |= SELECT;
    }
    else {
      BLI_assert_msg(0, "Wrong which_handle passed to BKE_mask_point_select_set_handle");
    }
  }
  else {
    if (ELEM(which_handle, MASK_WHICH_HANDLE_STICK, MASK_WHICH_HANDLE_BOTH)) {
      point->bezt.f1 &= ~SELECT;
      point->bezt.f3 &= ~SELECT;
    }
    else if (which_handle == MASK_WHICH_HANDLE_LEFT) {
      point->bezt.f1 &= ~SELECT;
    }
    else if (which_handle == MASK_WHICH_HANDLE_RIGHT) {
      point->bezt.f3 &= ~SELECT;
    }
    else {
      BLI_assert_msg(0, "Wrong which_handle passed to BKE_mask_point_select_set_handle");
    }
  }
}

/* only mask block itself */
static Mask *mask_alloc(Main *bmain, const char *name)
{
  Mask *mask = static_cast<Mask *>(BKE_libblock_alloc(bmain, ID_MSK, name, 0));

  id_fake_user_set(&mask->id);

  return mask;
}

Mask *BKE_mask_new(Main *bmain, const char *name)
{
  Mask *mask;
  char mask_name[MAX_ID_NAME - 2];

  STRNCPY_UTF8(mask_name, (name && name[0]) ? name : DATA_("Mask"));

  mask = mask_alloc(bmain, mask_name);

  /* arbitrary defaults */
  mask->sfra = 1;
  mask->efra = 100;

  DEG_relations_tag_update(bmain);

  return mask;
}

void BKE_mask_point_free(MaskSplinePoint *point)
{
  if (point->uw) {
    MEM_freeN(point->uw);
  }
}

void BKE_mask_spline_free(MaskSpline *spline)
{
  int i = 0;

  for (i = 0; i < spline->tot_point; i++) {
    MaskSplinePoint *point;
    point = &spline->points[i];
    BKE_mask_point_free(point);

    if (spline->points_deform) {
      point = &spline->points_deform[i];
      BKE_mask_point_free(point);
    }
  }

  MEM_freeN(spline->points);

  if (spline->points_deform) {
    MEM_freeN(spline->points_deform);
  }

  MEM_freeN(spline);
}

void BKE_mask_spline_free_list(ListBase *splines)
{
  MaskSpline *spline = static_cast<MaskSpline *>(splines->first);
  while (spline) {
    MaskSpline *next_spline = spline->next;

    BLI_remlink(splines, spline);
    BKE_mask_spline_free(spline);

    spline = next_spline;
  }
}

static MaskSplinePoint *mask_spline_points_copy(const MaskSplinePoint *points, int tot_point)
{
  MaskSplinePoint *npoints = static_cast<MaskSplinePoint *>(MEM_dupallocN(points));

  for (int i = 0; i < tot_point; i++) {
    MaskSplinePoint *point = &npoints[i];

    if (point->uw) {
      point->uw = static_cast<MaskSplinePointUW *>(MEM_dupallocN(point->uw));
    }
  }

  return npoints;
}

MaskSpline *BKE_mask_spline_copy(const MaskSpline *spline)
{
  MaskSpline *nspline = MEM_callocN<MaskSpline>("new spline");

  *nspline = *spline;

  nspline->points_deform = nullptr;
  nspline->points = mask_spline_points_copy(spline->points, spline->tot_point);

  if (spline->points_deform) {
    nspline->points_deform = mask_spline_points_copy(spline->points_deform, spline->tot_point);
  }

  return nspline;
}

MaskLayerShape *BKE_mask_layer_shape_alloc(MaskLayer *masklay, const int frame)
{
  MaskLayerShape *masklay_shape;
  int tot_vert = BKE_mask_layer_shape_totvert(masklay);

  masklay_shape = MEM_callocN<MaskLayerShape>(__func__);
  masklay_shape->frame = frame;
  masklay_shape->tot_vert = tot_vert;
  masklay_shape->data = MEM_calloc_arrayN<float>(tot_vert * MASK_OBJECT_SHAPE_ELEM_SIZE, __func__);

  return masklay_shape;
}

void BKE_mask_layer_shape_free(MaskLayerShape *masklay_shape)
{
  if (masklay_shape->data) {
    MEM_freeN(masklay_shape->data);
  }

  MEM_freeN(masklay_shape);
}

void BKE_mask_layer_free_shapes(MaskLayer *masklay)
{
  MaskLayerShape *masklay_shape;

  /* free animation data */
  masklay_shape = static_cast<MaskLayerShape *>(masklay->splines_shapes.first);
  while (masklay_shape) {
    MaskLayerShape *next_masklay_shape = masklay_shape->next;

    BLI_remlink(&masklay->splines_shapes, masklay_shape);
    BKE_mask_layer_shape_free(masklay_shape);

    masklay_shape = next_masklay_shape;
  }
}

void BKE_mask_layer_free(MaskLayer *masklay)
{
  /* free splines */
  BKE_mask_spline_free_list(&masklay->splines);

  /* free animation data */
  BKE_mask_layer_free_shapes(masklay);

  MEM_freeN(masklay);
}

void BKE_mask_layer_free_list(ListBase *masklayers)
{
  MaskLayer *masklay = static_cast<MaskLayer *>(masklayers->first);

  while (masklay) {
    MaskLayer *masklay_next = masklay->next;

    BLI_remlink(masklayers, masklay);
    BKE_mask_layer_free(masklay);

    masklay = masklay_next;
  }
}

void BKE_mask_coord_from_frame(float r_co[2], const float co[2], const float frame_size[2])
{
  if (frame_size[0] == frame_size[1]) {
    r_co[0] = co[0];
    r_co[1] = co[1];
  }
  else if (frame_size[0] < frame_size[1]) {
    r_co[0] = ((co[0] - 0.5f) * (frame_size[0] / frame_size[1])) + 0.5f;
    r_co[1] = co[1];
  }
  else { /* (frame_size[0] > frame_size[1]) */
    r_co[0] = co[0];
    r_co[1] = ((co[1] - 0.5f) * (frame_size[1] / frame_size[0])) + 0.5f;
  }
}

void BKE_mask_coord_from_movieclip(MovieClip *clip,
                                   MovieClipUser *user,
                                   float r_co[2],
                                   const float co[2])
{
  float aspx, aspy;
  float frame_size[2];

  /* scaling for the clip */
  BKE_movieclip_get_size_fl(clip, user, frame_size);
  BKE_movieclip_get_aspect(clip, &aspx, &aspy);

  frame_size[1] *= (aspy / aspx);

  BKE_mask_coord_from_frame(r_co, co, frame_size);
}

void BKE_mask_coord_from_image(Image *image, ImageUser *iuser, float r_co[2], const float co[2])
{
  float aspx, aspy;
  float frame_size[2];

  BKE_image_get_size_fl(image, iuser, frame_size);
  BKE_image_get_aspect(image, &aspx, &aspy);

  frame_size[1] *= (aspy / aspx);

  BKE_mask_coord_from_frame(r_co, co, frame_size);
}

void BKE_mask_coord_to_frame(float r_co[2], const float co[2], const float frame_size[2])
{
  if (frame_size[0] == frame_size[1]) {
    r_co[0] = co[0];
    r_co[1] = co[1];
  }
  else if (frame_size[0] < frame_size[1]) {
    r_co[0] = ((co[0] - 0.5f) / (frame_size[0] / frame_size[1])) + 0.5f;
    r_co[1] = co[1];
  }
  else { /* (frame_size[0] > frame_size[1]) */
    r_co[0] = co[0];
    r_co[1] = ((co[1] - 0.5f) / (frame_size[1] / frame_size[0])) + 0.5f;
  }
}

void BKE_mask_coord_to_movieclip(MovieClip *clip,
                                 MovieClipUser *user,
                                 float r_co[2],
                                 const float co[2])
{
  float aspx, aspy;
  float frame_size[2];

  /* scaling for the clip */
  BKE_movieclip_get_size_fl(clip, user, frame_size);
  BKE_movieclip_get_aspect(clip, &aspx, &aspy);

  frame_size[1] *= (aspy / aspx);

  BKE_mask_coord_to_frame(r_co, co, frame_size);
}

void BKE_mask_coord_to_image(Image *image, ImageUser *iuser, float r_co[2], const float co[2])
{
  float aspx, aspy;
  float frame_size[2];

  /* scaling for the clip */
  BKE_image_get_size_fl(image, iuser, frame_size);
  BKE_image_get_aspect(image, &aspx, &aspy);

  frame_size[1] *= (aspy / aspx);

  BKE_mask_coord_to_frame(r_co, co, frame_size);
}

void BKE_mask_point_parent_matrix_get(MaskSplinePoint *point,
                                      float ctime,
                                      float parent_matrix[3][3])
{
  MaskParent *parent = &point->parent;

  unit_m3(parent_matrix);

  if (!parent) {
    return;
  }

  if (parent->id_type == ID_MC) {
    if (parent->id) {
      MovieClip *clip = (MovieClip *)parent->id;
      MovieTracking *tracking = (MovieTracking *)&clip->tracking;
      MovieTrackingObject *ob = BKE_tracking_object_get_named(tracking, parent->parent);

      if (ob) {
        MovieClipUser user = *DNA_struct_default_get(MovieClipUser);
        float clip_framenr = BKE_movieclip_remap_scene_to_clip_frame(clip, ctime);
        BKE_movieclip_user_set_frame(&user, ctime);

        if (parent->type == MASK_PARENT_POINT_TRACK) {
          MovieTrackingTrack *track = BKE_tracking_object_find_track_with_name(ob,
                                                                               parent->sub_parent);

          if (track) {
            float marker_position[2], parent_co[2];
            BKE_tracking_marker_get_subframe_position(track, clip_framenr, marker_position);
            BKE_mask_coord_from_movieclip(clip, &user, parent_co, marker_position);
            sub_v2_v2v2(parent_matrix[2], parent_co, parent->parent_orig);
          }
        }
        else /* if (parent->type == MASK_PARENT_PLANE_TRACK) */ {
          MovieTrackingPlaneTrack *plane_track = BKE_tracking_object_find_plane_track_with_name(
              ob, parent->sub_parent);

          if (plane_track) {
            float corners[4][2];
            float aspx, aspy;
            float frame_size[2], H[3][3], mask_from_clip_matrix[3][3], mask_to_clip_matrix[3][3];

            BKE_tracking_plane_marker_get_subframe_corners(plane_track, ctime, corners);
            BKE_tracking_homography_between_two_quads(parent->parent_corners_orig, corners, H);

            unit_m3(mask_from_clip_matrix);

            BKE_movieclip_get_size_fl(clip, &user, frame_size);
            BKE_movieclip_get_aspect(clip, &aspx, &aspy);

            frame_size[1] *= (aspy / aspx);
            if (frame_size[0] == frame_size[1]) {
              /* pass */
            }
            else if (frame_size[0] < frame_size[1]) {
              mask_from_clip_matrix[0][0] = frame_size[1] / frame_size[0];
              mask_from_clip_matrix[2][0] = -0.5f * (frame_size[1] / frame_size[0]) + 0.5f;
            }
            else { /* (frame_size[0] > frame_size[1]) */
              mask_from_clip_matrix[1][1] = frame_size[1] / frame_size[0];
              mask_from_clip_matrix[2][1] = -0.5f * (frame_size[1] / frame_size[0]) + 0.5f;
            }

            invert_m3_m3(mask_to_clip_matrix, mask_from_clip_matrix);
            mul_m3_series(parent_matrix, mask_from_clip_matrix, H, mask_to_clip_matrix);
          }
        }
      }
    }
  }
}

static void mask_calc_point_handle(MaskSplinePoint *point,
                                   MaskSplinePoint *point_prev,
                                   MaskSplinePoint *point_next)
{
  BezTriple *bezt = &point->bezt;
  BezTriple *bezt_prev = nullptr, *bezt_next = nullptr;
  // int handle_type = bezt->h1;

  if (point_prev) {
    bezt_prev = &point_prev->bezt;
  }

  if (point_next) {
    bezt_next = &point_next->bezt;
  }

#if 1
  if (bezt_prev || bezt_next) {
    BKE_nurb_handle_calc(bezt, bezt_prev, bezt_next, false, 0);
  }
#else
  if (handle_type == HD_VECT) {
    BKE_nurb_handle_calc(bezt, bezt_prev, bezt_next, 0, 0);
  }
  else if (handle_type == HD_AUTO) {
    BKE_nurb_handle_calc(bezt, bezt_prev, bezt_next, 0, 0);
  }
  else if (ELEM(handle_type, HD_ALIGN, HD_ALIGN_DOUBLESIDE)) {
    float v1[3], v2[3];
    float vec[3], h[3];

    sub_v3_v3v3(v1, bezt->vec[0], bezt->vec[1]);
    sub_v3_v3v3(v2, bezt->vec[2], bezt->vec[1]);
    add_v3_v3v3(vec, v1, v2);

    if (len_squared_v3(vec) > (1e-3f * 1e-3f)) {
      h[0] = vec[1];
      h[1] = -vec[0];
      h[2] = 0.0f;
    }
    else {
      copy_v3_v3(h, v1);
    }

    add_v3_v3v3(bezt->vec[0], bezt->vec[1], h);
    sub_v3_v3v3(bezt->vec[2], bezt->vec[1], h);
  }
#endif
}

void BKE_mask_get_handle_point_adjacent(MaskSpline *spline,
                                        MaskSplinePoint *point,
                                        MaskSplinePoint **r_point_prev,
                                        MaskSplinePoint **r_point_next)
{
  /* TODO: could avoid calling this at such low level. */
  MaskSplinePoint *points_array = BKE_mask_spline_point_array_from_point(spline, point);

  *r_point_prev = mask_spline_point_prev(spline, points_array, point);
  *r_point_next = mask_spline_point_next(spline, points_array, point);
}

void BKE_mask_calc_tangent_polyline(MaskSpline *spline, MaskSplinePoint *point, float t[2])
{
  float tvec_a[2], tvec_b[2];

  MaskSplinePoint *point_prev, *point_next;

  BKE_mask_get_handle_point_adjacent(spline, point, &point_prev, &point_next);

  if (point_prev) {
    sub_v2_v2v2(tvec_a, point->bezt.vec[1], point_prev->bezt.vec[1]);
    normalize_v2(tvec_a);
  }
  else {
    zero_v2(tvec_a);
  }

  if (point_next) {
    sub_v2_v2v2(tvec_b, point_next->bezt.vec[1], point->bezt.vec[1]);
    normalize_v2(tvec_b);
  }
  else {
    zero_v2(tvec_b);
  }

  add_v2_v2v2(t, tvec_a, tvec_b);
  normalize_v2(t);
}

void BKE_mask_calc_handle_point(MaskSpline *spline, MaskSplinePoint *point)
{
  MaskSplinePoint *point_prev, *point_next;

  BKE_mask_get_handle_point_adjacent(spline, point, &point_prev, &point_next);

  mask_calc_point_handle(point, point_prev, point_next);
}

void BKE_mask_calc_handle_adjacent_interp(MaskSpline *spline,
                                          MaskSplinePoint *point,
                                          const float u)
{
  /* TODO: make this interpolate between siblings - not always midpoint! */
  int length_tot = 0;
  float length_average = 0.0f;
  float weight_average = 0.0f;

  MaskSplinePoint *point_prev, *point_next;

  BLI_assert(u >= 0.0f && u <= 1.0f);

  BKE_mask_get_handle_point_adjacent(spline, point, &point_prev, &point_next);

  if (point_prev && point_next) {
    length_average = ((len_v2v2(point_prev->bezt.vec[0], point_prev->bezt.vec[1]) * (1.0f - u)) +
                      (len_v2v2(point_next->bezt.vec[2], point_next->bezt.vec[1]) * u));

    weight_average = (point_prev->bezt.weight * (1.0f - u) + point_next->bezt.weight * u);
    length_tot = 1;
  }
  else {
    if (point_prev) {
      length_average += len_v2v2(point_prev->bezt.vec[0], point_prev->bezt.vec[1]);
      weight_average += point_prev->bezt.weight;
      length_tot++;
    }

    if (point_next) {
      length_average += len_v2v2(point_next->bezt.vec[2], point_next->bezt.vec[1]);
      weight_average += point_next->bezt.weight;
      length_tot++;
    }
  }

  if (length_tot) {
    length_average /= float(length_tot);
    weight_average /= float(length_tot);

    dist_ensure_v2_v2fl(point->bezt.vec[0], point->bezt.vec[1], length_average);
    dist_ensure_v2_v2fl(point->bezt.vec[2], point->bezt.vec[1], length_average);
    point->bezt.weight = weight_average;
  }
}

void BKE_mask_calc_handle_point_auto(MaskSpline *spline,
                                     MaskSplinePoint *point,
                                     const bool do_recalc_length)
{
  MaskSplinePoint *point_prev, *point_next;
  const char h_back[2] = {point->bezt.h1, point->bezt.h2};
  const float length_average = (do_recalc_length) ?
                                   0.0f /* dummy value */ :
                                   (len_v3v3(point->bezt.vec[0], point->bezt.vec[1]) +
                                    len_v3v3(point->bezt.vec[1], point->bezt.vec[2])) /
                                       2.0f;

  BKE_mask_get_handle_point_adjacent(spline, point, &point_prev, &point_next);

  point->bezt.h1 = HD_AUTO;
  point->bezt.h2 = HD_AUTO;
  mask_calc_point_handle(point, point_prev, point_next);

  point->bezt.h1 = h_back[0];
  point->bezt.h2 = h_back[1];

  /* preserve length by applying it back */
  if (do_recalc_length == false) {
    dist_ensure_v2_v2fl(point->bezt.vec[0], point->bezt.vec[1], length_average);
    dist_ensure_v2_v2fl(point->bezt.vec[2], point->bezt.vec[1], length_average);
  }
}

void BKE_mask_layer_calc_handles(MaskLayer *masklay)
{
  LISTBASE_FOREACH (MaskSpline *, spline, &masklay->splines) {
    for (int i = 0; i < spline->tot_point; i++) {
      BKE_mask_calc_handle_point(spline, &spline->points[i]);
    }
  }
}

void BKE_mask_spline_ensure_deform(MaskSpline *spline)
{
  int allocated_points = (MEM_allocN_len(spline->points_deform) / sizeof(*spline->points_deform));
  // printf("SPLINE ALLOC %p %d\n", spline->points_deform, allocated_points);

  if (spline->points_deform == nullptr || allocated_points != spline->tot_point) {
    // printf("alloc new deform spline\n");

    if (spline->points_deform) {
      for (int i = 0; i < allocated_points; i++) {
        MaskSplinePoint *point = &spline->points_deform[i];
        BKE_mask_point_free(point);
      }

      MEM_freeN(spline->points_deform);
    }

    spline->points_deform = MEM_calloc_arrayN<MaskSplinePoint>(spline->tot_point, __func__);
  }
  else {
    // printf("alloc spline done\n");
  }
}

void BKE_mask_layer_evaluate(MaskLayer *masklay, const float ctime, const bool do_newframe)
{
  /* Animation if available. */
  if (do_newframe) {
    BKE_mask_layer_evaluate_animation(masklay, ctime);
  }
  /* Update deform. */
  BKE_mask_layer_evaluate_deform(masklay, ctime);
}

void BKE_mask_evaluate(Mask *mask, const float ctime, const bool do_newframe)
{
  LISTBASE_FOREACH (MaskLayer *, masklay, &mask->masklayers) {
    BKE_mask_layer_evaluate(masklay, ctime, do_newframe);
  }
}

void BKE_mask_parent_init(MaskParent *parent)
{
  parent->id_type = ID_MC;
}

/* *** animation/shape-key implementation ***
 * BKE_mask_layer_shape_XXX */

int BKE_mask_layer_shape_totvert(MaskLayer *masklay)
{
  int tot = 0;

  LISTBASE_FOREACH (MaskSpline *, spline, &masklay->splines) {
    tot += spline->tot_point;
  }

  return tot;
}

static void mask_layer_shape_from_mask_point(BezTriple *bezt,
                                             float fp[MASK_OBJECT_SHAPE_ELEM_SIZE])
{
  copy_v2_v2(&fp[0], bezt->vec[0]);
  copy_v2_v2(&fp[2], bezt->vec[1]);
  copy_v2_v2(&fp[4], bezt->vec[2]);
  fp[6] = bezt->weight;
  fp[7] = bezt->radius;
}

static void mask_layer_shape_to_mask_point(BezTriple *bezt,
                                           const float fp[MASK_OBJECT_SHAPE_ELEM_SIZE])
{
  copy_v2_v2(bezt->vec[0], &fp[0]);
  copy_v2_v2(bezt->vec[1], &fp[2]);
  copy_v2_v2(bezt->vec[2], &fp[4]);
  bezt->weight = fp[6];
  bezt->radius = fp[7];
}

void BKE_mask_layer_shape_from_mask(MaskLayer *masklay, MaskLayerShape *masklay_shape)
{
  int tot = BKE_mask_layer_shape_totvert(masklay);

  if (masklay_shape->tot_vert == tot) {
    float *fp = masklay_shape->data;

    LISTBASE_FOREACH (MaskSpline *, spline, &masklay->splines) {
      for (int i = 0; i < spline->tot_point; i++) {
        mask_layer_shape_from_mask_point(&spline->points[i].bezt, fp);
        fp += MASK_OBJECT_SHAPE_ELEM_SIZE;
      }
    }
  }
  else {
    CLOG_ERROR(&LOG,
               "vert mismatch %d != %d (frame %d)",
               masklay_shape->tot_vert,
               tot,
               masklay_shape->frame);
  }
}

void BKE_mask_layer_shape_to_mask(MaskLayer *masklay, MaskLayerShape *masklay_shape)
{
  int tot = BKE_mask_layer_shape_totvert(masklay);

  if (masklay_shape->tot_vert == tot) {
    float *fp = masklay_shape->data;

    LISTBASE_FOREACH (MaskSpline *, spline, &masklay->splines) {
      for (int i = 0; i < spline->tot_point; i++) {
        mask_layer_shape_to_mask_point(&spline->points[i].bezt, fp);
        fp += MASK_OBJECT_SHAPE_ELEM_SIZE;
      }
    }
  }
  else {
    CLOG_ERROR(&LOG,
               "vert mismatch %d != %d (frame %d)",
               masklay_shape->tot_vert,
               tot,
               masklay_shape->frame);
  }
}

BLI_INLINE void interp_v2_v2v2_flfl(
    float target[2], const float a[2], const float b[2], const float t, const float s)
{
  target[0] = s * a[0] + t * b[0];
  target[1] = s * a[1] + t * b[1];
}

void BKE_mask_layer_shape_to_mask_interp(MaskLayer *masklay,
                                         MaskLayerShape *masklay_shape_a,
                                         MaskLayerShape *masklay_shape_b,
                                         const float fac)
{
  int tot = BKE_mask_layer_shape_totvert(masklay);
  if (masklay_shape_a->tot_vert == tot && masklay_shape_b->tot_vert == tot) {
    const float *fp_a = masklay_shape_a->data;
    const float *fp_b = masklay_shape_b->data;
    const float ifac = 1.0f - fac;

    LISTBASE_FOREACH (MaskSpline *, spline, &masklay->splines) {
      for (int i = 0; i < spline->tot_point; i++) {
        BezTriple *bezt = &spline->points[i].bezt;
        /* *** BKE_mask_layer_shape_from_mask - swapped *** */
        interp_v2_v2v2_flfl(bezt->vec[0], fp_a, fp_b, fac, ifac);
        fp_a += 2;
        fp_b += 2;
        interp_v2_v2v2_flfl(bezt->vec[1], fp_a, fp_b, fac, ifac);
        fp_a += 2;
        fp_b += 2;
        interp_v2_v2v2_flfl(bezt->vec[2], fp_a, fp_b, fac, ifac);
        fp_a += 2;
        fp_b += 2;
        bezt->weight = (fp_a[0] * ifac) + (fp_b[0] * fac);
        bezt->radius = (fp_a[1] * ifac) + (fp_b[1] * fac);
        fp_a += 2;
        fp_b += 2;
      }
    }
  }
  else {
    CLOG_ERROR(&LOG,
               "vert mismatch %d != %d != %d (frame %d - %d)",
               masklay_shape_a->tot_vert,
               masklay_shape_b->tot_vert,
               tot,
               masklay_shape_a->frame,
               masklay_shape_b->frame);
  }
}

MaskLayerShape *BKE_mask_layer_shape_find_frame(MaskLayer *masklay, const int frame)
{
  LISTBASE_FOREACH (MaskLayerShape *, masklay_shape, &masklay->splines_shapes) {
    if (frame == masklay_shape->frame) {
      return masklay_shape;
    }
    if (frame < masklay_shape->frame) {
      break;
    }
  }

  return nullptr;
}

int BKE_mask_layer_shape_find_frame_range(MaskLayer *masklay,
                                          const float frame,
                                          MaskLayerShape **r_masklay_shape_a,
                                          MaskLayerShape **r_masklay_shape_b)
{
  MaskLayerShape *masklay_shape;

  for (masklay_shape = static_cast<MaskLayerShape *>(masklay->splines_shapes.first); masklay_shape;
       masklay_shape = masklay_shape->next)
  {
    if (frame == masklay_shape->frame) {
      *r_masklay_shape_a = masklay_shape;
      *r_masklay_shape_b = nullptr;
      return 1;
    }
    if (frame < masklay_shape->frame) {
      if (masklay_shape->prev) {
        *r_masklay_shape_a = masklay_shape->prev;
        *r_masklay_shape_b = masklay_shape;
        return 2;
      }

      *r_masklay_shape_a = masklay_shape;
      *r_masklay_shape_b = nullptr;
      return 1;
    }
  }

  masklay_shape = static_cast<MaskLayerShape *>(masklay->splines_shapes.last);
  if (masklay_shape) {
    *r_masklay_shape_a = masklay_shape;
    *r_masklay_shape_b = nullptr;
    return 1;
  }

  *r_masklay_shape_a = nullptr;
  *r_masklay_shape_b = nullptr;

  return 0;
}

MaskLayerShape *BKE_mask_layer_shape_verify_frame(MaskLayer *masklay, const int frame)
{
  MaskLayerShape *masklay_shape;

  masklay_shape = BKE_mask_layer_shape_find_frame(masklay, frame);

  if (masklay_shape == nullptr) {
    masklay_shape = BKE_mask_layer_shape_alloc(masklay, frame);
    BLI_addtail(&masklay->splines_shapes, masklay_shape);
    BKE_mask_layer_shape_sort(masklay);
  }

  return masklay_shape;
}

MaskLayerShape *BKE_mask_layer_shape_duplicate(MaskLayerShape *masklay_shape)
{
  MaskLayerShape *masklay_shape_copy = static_cast<MaskLayerShape *>(MEM_dupallocN(masklay_shape));

  if (LIKELY(masklay_shape_copy->data)) {
    masklay_shape_copy->data = static_cast<float *>(MEM_dupallocN(masklay_shape_copy->data));
  }

  return masklay_shape_copy;
}

void BKE_mask_layer_shape_unlink(MaskLayer *masklay, MaskLayerShape *masklay_shape)
{
  BLI_remlink(&masklay->splines_shapes, masklay_shape);

  BKE_mask_layer_shape_free(masklay_shape);
}

static int mask_layer_shape_sort_cb(const void *masklay_shape_a_ptr,
                                    const void *masklay_shape_b_ptr)
{
  const MaskLayerShape *masklay_shape_a = static_cast<const MaskLayerShape *>(masklay_shape_a_ptr);
  const MaskLayerShape *masklay_shape_b = static_cast<const MaskLayerShape *>(masklay_shape_b_ptr);

  if (masklay_shape_a->frame < masklay_shape_b->frame) {
    return -1;
  }
  if (masklay_shape_a->frame > masklay_shape_b->frame) {
    return 1;
  }

  return 0;
}

void BKE_mask_layer_shape_sort(MaskLayer *masklay)
{
  BLI_listbase_sort(&masklay->splines_shapes, mask_layer_shape_sort_cb);
}

bool BKE_mask_layer_shape_spline_from_index(MaskLayer *masklay,
                                            int index,
                                            MaskSpline **r_masklay_shape,
                                            int *r_index)
{
  LISTBASE_FOREACH (MaskSpline *, spline, &masklay->splines) {
    if (index < spline->tot_point) {
      *r_masklay_shape = spline;
      *r_index = index;
      return true;
    }
    index -= spline->tot_point;
  }

  return false;
}

int BKE_mask_layer_shape_spline_to_index(MaskLayer *masklay, MaskSpline *spline)
{
  MaskSpline *spline_iter;
  int i_abs = 0;
  for (spline_iter = static_cast<MaskSpline *>(masklay->splines.first);
       spline_iter && spline_iter != spline;
       i_abs += spline_iter->tot_point, spline_iter = spline_iter->next)
  {
    /* pass */
  }

  return i_abs;
}

/* basic 2D interpolation functions, could make more comprehensive later */
static void interp_weights_uv_v2_calc(float r_uv[2],
                                      const float pt[2],
                                      const float pt_a[2],
                                      const float pt_b[2])
{
  const float segment_len = len_v2v2(pt_a, pt_b);
  if (segment_len == 0.0f) {
    r_uv[0] = 1.0f;
    r_uv[1] = 0.0f;
    return;
  }

  float pt_on_line[2];
  r_uv[0] = closest_to_line_v2(pt_on_line, pt, pt_a, pt_b);

  r_uv[1] = (len_v2v2(pt_on_line, pt) / segment_len) *
            /* This line only sets the sign. */
            ((line_point_side_v2(pt_a, pt_b, pt) < 0.0f) ? -1.0f : 1.0f);
}

static void interp_weights_uv_v2_apply(const float uv[2],
                                       float r_pt[2],
                                       const float pt_a[2],
                                       const float pt_b[2])
{
  const float dvec[2] = {pt_b[0] - pt_a[0], pt_b[1] - pt_a[1]};

  /* u */
  madd_v2_v2v2fl(r_pt, pt_a, dvec, uv[0]);

  /* v */
  r_pt[0] += -dvec[1] * uv[1];
  r_pt[1] += dvec[0] * uv[1];
}

void BKE_mask_layer_shape_changed_add(MaskLayer *masklay,
                                      int index,
                                      bool do_init,
                                      bool do_init_interpolate)
{
  /* spline index from masklay */
  MaskSpline *spline;
  int spline_point_index;

  if (BKE_mask_layer_shape_spline_from_index(masklay, index, &spline, &spline_point_index)) {
    /* sanity check */
    /* The point has already been removed in this array
     * so subtract one when comparing with the shapes. */
    int tot = BKE_mask_layer_shape_totvert(masklay) - 1;

    /* for interpolation */
    /* TODO: assumes closed curve for now. */
    float uv[3][2]; /* 3x 2D handles */
    const int pi_curr = spline_point_index;
    const int pi_prev = ((spline_point_index - 1) + spline->tot_point) % spline->tot_point;
    const int pi_next = (spline_point_index + 1) % spline->tot_point;

    const int index_offset = index - spline_point_index;
    // const int pi_curr_abs = index;
    const int pi_prev_abs = pi_prev + index_offset;
    const int pi_next_abs = pi_next + index_offset;

    if (do_init_interpolate) {
      for (int i = 0; i < 3; i++) {
        interp_weights_uv_v2_calc(uv[i],
                                  spline->points[pi_curr].bezt.vec[i],
                                  spline->points[pi_prev].bezt.vec[i],
                                  spline->points[pi_next].bezt.vec[i]);
      }
    }

    LISTBASE_FOREACH (MaskLayerShape *, masklay_shape, &masklay->splines_shapes) {
      if (tot == masklay_shape->tot_vert) {
        float *data_resized;

        masklay_shape->tot_vert++;
        data_resized = MEM_calloc_arrayN<float>(
            masklay_shape->tot_vert * MASK_OBJECT_SHAPE_ELEM_SIZE, __func__);
        if (index > 0) {
          memcpy(data_resized,
                 masklay_shape->data,
                 index * sizeof(float) * MASK_OBJECT_SHAPE_ELEM_SIZE);
        }

        if (index != masklay_shape->tot_vert - 1) {
          memcpy(&data_resized[(index + 1) * MASK_OBJECT_SHAPE_ELEM_SIZE],
                 masklay_shape->data + (index * MASK_OBJECT_SHAPE_ELEM_SIZE),
                 (masklay_shape->tot_vert - (index + 1)) * sizeof(float) *
                     MASK_OBJECT_SHAPE_ELEM_SIZE);
        }

        if (do_init) {
          float *fp = &data_resized[index * MASK_OBJECT_SHAPE_ELEM_SIZE];

          mask_layer_shape_from_mask_point(&spline->points[spline_point_index].bezt, fp);

          if (do_init_interpolate && spline->tot_point > 2) {
            for (int i = 0; i < 3; i++) {
              interp_weights_uv_v2_apply(
                  uv[i],
                  &fp[i * 2],
                  &data_resized[(pi_prev_abs * MASK_OBJECT_SHAPE_ELEM_SIZE) + (i * 2)],
                  &data_resized[(pi_next_abs * MASK_OBJECT_SHAPE_ELEM_SIZE) + (i * 2)]);
            }
          }
        }
        else {
          memset(&data_resized[index * MASK_OBJECT_SHAPE_ELEM_SIZE],
                 0,
                 sizeof(float) * MASK_OBJECT_SHAPE_ELEM_SIZE);
        }

        MEM_freeN(masklay_shape->data);
        masklay_shape->data = data_resized;
      }
      else {
        CLOG_ERROR(&LOG,
                   "vert mismatch %d != %d (frame %d)",
                   masklay_shape->tot_vert,
                   tot,
                   masklay_shape->frame);
      }
    }
  }
}

void BKE_mask_layer_shape_changed_remove(MaskLayer *masklay, int index, int count)
{
  /* the point has already been removed in this array so add one when comparing with the shapes */
  int tot = BKE_mask_layer_shape_totvert(masklay);

  LISTBASE_FOREACH (MaskLayerShape *, masklay_shape, &masklay->splines_shapes) {
    if (tot == masklay_shape->tot_vert - count) {
      float *data_resized;

      masklay_shape->tot_vert -= count;
      data_resized = MEM_calloc_arrayN<float>(
          masklay_shape->tot_vert * MASK_OBJECT_SHAPE_ELEM_SIZE, __func__);
      if (index > 0) {
        memcpy(data_resized,
               masklay_shape->data,
               index * sizeof(float) * MASK_OBJECT_SHAPE_ELEM_SIZE);
      }

      if (index != masklay_shape->tot_vert) {
        memcpy(&data_resized[index * MASK_OBJECT_SHAPE_ELEM_SIZE],
               masklay_shape->data + ((index + count) * MASK_OBJECT_SHAPE_ELEM_SIZE),
               (masklay_shape->tot_vert - index) * sizeof(float) * MASK_OBJECT_SHAPE_ELEM_SIZE);
      }

      MEM_freeN(masklay_shape->data);
      masklay_shape->data = data_resized;
    }
    else {
      CLOG_ERROR(&LOG,
                 "vert mismatch %d != %d (frame %d)",
                 masklay_shape->tot_vert - count,
                 tot,
                 masklay_shape->frame);
    }
  }
}

int BKE_mask_get_duration(Mask *mask)
{
  return max_ii(1, mask->efra - mask->sfra);
}

/*********************** clipboard *************************/

static void mask_clipboard_free_ex(bool final_free)
{
  BKE_mask_spline_free_list(&mask_clipboard.splines);
  BLI_listbase_clear(&mask_clipboard.splines);
  if (mask_clipboard.id_hash) {
    if (final_free) {
      BLI_ghash_free(mask_clipboard.id_hash, nullptr, MEM_freeN);
    }
    else {
      BLI_ghash_clear(mask_clipboard.id_hash, nullptr, MEM_freeN);
    }
  }
}

void BKE_mask_clipboard_free()
{
  mask_clipboard_free_ex(true);
}

void BKE_mask_clipboard_copy_from_layer(MaskLayer *mask_layer)
{
  /* Nothing to do if selection if disabled for the given layer. */
  if (mask_layer->visibility_flag & MASK_HIDE_SELECT) {
    return;
  }

  mask_clipboard_free_ex(false);
  if (mask_clipboard.id_hash == nullptr) {
    mask_clipboard.id_hash = BLI_ghash_ptr_new("mask clipboard ID hash");
  }

  LISTBASE_FOREACH (MaskSpline *, spline, &mask_layer->splines) {
    if (spline->flag & SELECT) {
      MaskSpline *spline_new = BKE_mask_spline_copy(spline);
      for (int i = 0; i < spline_new->tot_point; i++) {
        MaskSplinePoint *point = &spline_new->points[i];
        if (point->parent.id) {
          if (!BLI_ghash_lookup(mask_clipboard.id_hash, point->parent.id)) {
            int len = strlen(point->parent.id->name);
            char *name_copy = MEM_malloc_arrayN<char>(size_t(len) + 1, "mask clipboard ID name");
            memcpy(name_copy, point->parent.id->name, len + 1);
            BLI_ghash_insert(mask_clipboard.id_hash, point->parent.id, name_copy);
          }
        }
      }

      BLI_addtail(&mask_clipboard.splines, spline_new);
    }
  }
}

bool BKE_mask_clipboard_is_empty()
{
  return BLI_listbase_is_empty(&mask_clipboard.splines);
}

void BKE_mask_clipboard_paste_to_layer(Main *bmain, MaskLayer *mask_layer)
{
  LISTBASE_FOREACH (MaskSpline *, spline, &mask_clipboard.splines) {
    MaskSpline *spline_new = BKE_mask_spline_copy(spline);

    for (int i = 0; i < spline_new->tot_point; i++) {
      MaskSplinePoint *point = &spline_new->points[i];
      if (point->parent.id) {
        const char *id_name = static_cast<const char *>(
            BLI_ghash_lookup(mask_clipboard.id_hash, point->parent.id));
        ListBase *listbase;

        BLI_assert(id_name != nullptr);

        listbase = which_libbase(bmain, GS(id_name));
        point->parent.id = static_cast<ID *>(
            BLI_findstring(listbase, id_name + 2, offsetof(ID, name) + 2));
      }
    }

    BLI_addtail(&mask_layer->splines, spline_new);
  }
}
