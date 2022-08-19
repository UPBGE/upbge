/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2008 Blender Foundation. */

/** \file
 * \ingroup bke
 */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CLG_log.h"

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_math_vector.h"
#include "BLI_string_utils.h"

#include "BLT_translation.h"

#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

/* Allow using deprecated functionality for .blend file I/O. */
#define DNA_DEPRECATED_ALLOW

#include "DNA_gpencil_types.h"
#include "DNA_material_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_space_types.h"

#include "BKE_action.h"
#include "BKE_anim_data.h"
#include "BKE_collection.h"
#include "BKE_colortools.h"
#include "BKE_deform.h"
#include "BKE_gpencil.h"
#include "BKE_gpencil_geom.h"
#include "BKE_gpencil_update_cache.h"
#include "BKE_icons.h"
#include "BKE_idtype.h"
#include "BKE_image.h"
#include "BKE_lib_id.h"
#include "BKE_lib_query.h"
#include "BKE_main.h"
#include "BKE_material.h"
#include "BKE_paint.h"

#include "BLI_math_color.h"

#include "DEG_depsgraph_query.h"

#include "BLO_read_write.h"

static CLG_LogRef LOG = {"bke.gpencil"};

static void greasepencil_copy_data(Main *UNUSED(bmain),
                                   ID *id_dst,
                                   const ID *id_src,
                                   const int UNUSED(flag))
{
  bGPdata *gpd_dst = (bGPdata *)id_dst;
  const bGPdata *gpd_src = (const bGPdata *)id_src;

  /* duplicate material array */
  if (gpd_src->mat) {
    gpd_dst->mat = MEM_dupallocN(gpd_src->mat);
  }

  BKE_defgroup_copy_list(&gpd_dst->vertex_group_names, &gpd_src->vertex_group_names);

  /* copy layers */
  BLI_listbase_clear(&gpd_dst->layers);
  LISTBASE_FOREACH (bGPDlayer *, gpl_src, &gpd_src->layers) {
    /* make a copy of source layer and its data */

    /* TODO: here too could add unused flags... */
    bGPDlayer *gpl_dst = BKE_gpencil_layer_duplicate(gpl_src, true, true);

    /* Apply local layer transform to all frames. Calc the active frame is not enough
     * because onion skin can use more frames. This is more slow but required here. */
    if (gpl_dst->actframe != NULL) {
      bool transformed = ((!is_zero_v3(gpl_dst->location)) || (!is_zero_v3(gpl_dst->rotation)) ||
                          (!is_one_v3(gpl_dst->scale)));
      if (transformed) {
        loc_eul_size_to_mat4(
            gpl_dst->layer_mat, gpl_dst->location, gpl_dst->rotation, gpl_dst->scale);
        bool do_onion = ((gpl_dst->onion_flag & GP_LAYER_ONIONSKIN) != 0);
        bGPDframe *init_gpf = (do_onion) ? gpl_dst->frames.first : gpl_dst->actframe;
        for (bGPDframe *gpf = init_gpf; gpf; gpf = gpf->next) {
          LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
            bGPDspoint *pt;
            int i;
            for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
              mul_m4_v3(gpl_dst->layer_mat, &pt->x);
            }
          }
          /* if not onion, exit loop. */
          if (!do_onion) {
            break;
          }
        }
      }
    }

    BLI_addtail(&gpd_dst->layers, gpl_dst);
  }
}

static void greasepencil_free_data(ID *id)
{
  /* Really not ideal, but for now will do... In theory custom behaviors like not freeing cache
   * should be handled through specific API, and not be part of the generic one. */
  BKE_gpencil_free_data((bGPdata *)id, true);
}

static void greasepencil_foreach_id(ID *id, LibraryForeachIDData *data)
{
  bGPdata *gpencil = (bGPdata *)id;
  /* materials */
  for (int i = 0; i < gpencil->totcol; i++) {
    BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, gpencil->mat[i], IDWALK_CB_USER);
  }

  LISTBASE_FOREACH (bGPDlayer *, gplayer, &gpencil->layers) {
    BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, gplayer->parent, IDWALK_CB_NOP);
  }
}

static void greasepencil_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  bGPdata *gpd = (bGPdata *)id;

  /* Clean up, important in undo case to reduce false detection of changed data-blocks. */
  /* XXX not sure why the whole run-time data is not cleared in reading code,
   * for now mimicking it here. */
  gpd->runtime.sbuffer = NULL;
  gpd->runtime.sbuffer_used = 0;
  gpd->runtime.sbuffer_size = 0;
  gpd->runtime.tot_cp_points = 0;
  gpd->runtime.update_cache = NULL;

  /* write gpd data block to file */
  BLO_write_id_struct(writer, bGPdata, id_address, &gpd->id);
  BKE_id_blend_write(writer, &gpd->id);

  if (gpd->adt) {
    BKE_animdata_blend_write(writer, gpd->adt);
  }

  BKE_defbase_blend_write(writer, &gpd->vertex_group_names);

  BLO_write_pointer_array(writer, gpd->totcol, gpd->mat);

  /* write grease-pencil layers to file */
  BLO_write_struct_list(writer, bGPDlayer, &gpd->layers);
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    /* Write mask list. */
    BLO_write_struct_list(writer, bGPDlayer_Mask, &gpl->mask_layers);
    /* write this layer's frames to file */
    BLO_write_struct_list(writer, bGPDframe, &gpl->frames);
    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
      /* write strokes */
      BLO_write_struct_list(writer, bGPDstroke, &gpf->strokes);
      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        BLO_write_struct_array(writer, bGPDspoint, gps->totpoints, gps->points);
        BLO_write_struct_array(writer, bGPDtriangle, gps->tot_triangles, gps->triangles);
        BKE_defvert_blend_write(writer, gps->totpoints, gps->dvert);
        if (gps->editcurve != NULL) {
          bGPDcurve *gpc = gps->editcurve;
          BLO_write_struct(writer, bGPDcurve, gpc);
          BLO_write_struct_array(
              writer, bGPDcurve_point, gpc->tot_curve_points, gpc->curve_points);
        }
      }
    }
  }
}

void BKE_gpencil_blend_read_data(BlendDataReader *reader, bGPdata *gpd)
{
  /* We must firstly have some grease-pencil data to link! */
  if (gpd == NULL) {
    return;
  }

  /* Relink anim-data. */
  BLO_read_data_address(reader, &gpd->adt);
  BKE_animdata_blend_read_data(reader, gpd->adt);

  /* Ensure full objectmode for linked grease pencil. */
  if (ID_IS_LINKED(gpd)) {
    gpd->flag &= ~GP_DATA_STROKE_PAINTMODE;
    gpd->flag &= ~GP_DATA_STROKE_EDITMODE;
    gpd->flag &= ~GP_DATA_STROKE_SCULPTMODE;
    gpd->flag &= ~GP_DATA_STROKE_WEIGHTMODE;
    gpd->flag &= ~GP_DATA_STROKE_VERTEXMODE;
  }

  /* init stroke buffer */
  gpd->runtime.sbuffer = NULL;
  gpd->runtime.sbuffer_used = 0;
  gpd->runtime.sbuffer_size = 0;
  gpd->runtime.tot_cp_points = 0;
  gpd->runtime.update_cache = NULL;

  /* Relink palettes (old palettes deprecated, only to convert old files). */
  BLO_read_list(reader, &gpd->palettes);
  if (gpd->palettes.first != NULL) {
    LISTBASE_FOREACH (bGPDpalette *, palette, &gpd->palettes) {
      BLO_read_list(reader, &palette->colors);
    }
  }

  BLO_read_list(reader, &gpd->vertex_group_names);

  /* Materials. */
  BLO_read_pointer_array(reader, (void **)&gpd->mat);

  /* Relink layers. */
  BLO_read_list(reader, &gpd->layers);

  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    /* Relink frames. */
    BLO_read_list(reader, &gpl->frames);

    BLO_read_data_address(reader, &gpl->actframe);

    gpl->runtime.icon_id = 0;

    /* Relink masks. */
    BLO_read_list(reader, &gpl->mask_layers);

    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
      /* Relink strokes (and their points). */
      BLO_read_list(reader, &gpf->strokes);

      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        /* Relink stroke points array. */
        BLO_read_data_address(reader, &gps->points);
        /* Relink geometry. */
        BLO_read_data_address(reader, &gps->triangles);

        /* Relink stroke edit curve. */
        BLO_read_data_address(reader, &gps->editcurve);
        if (gps->editcurve != NULL) {
          /* Relink curve point array. */
          BLO_read_data_address(reader, &gps->editcurve->curve_points);
        }

        /* Relink weight data. */
        if (gps->dvert) {
          BLO_read_data_address(reader, &gps->dvert);
          BKE_defvert_blend_read(reader, gps->totpoints, gps->dvert);
        }
      }
    }
  }
}

static void greasepencil_blend_read_data(BlendDataReader *reader, ID *id)
{
  bGPdata *gpd = (bGPdata *)id;
  BKE_gpencil_blend_read_data(reader, gpd);
}

static void greasepencil_blend_read_lib(BlendLibReader *reader, ID *id)
{
  bGPdata *gpd = (bGPdata *)id;

  /* Relink all data-block linked by GP data-block. */
  /* Layers */
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    /* Layer -> Parent References */
    BLO_read_id_address(reader, gpd->id.lib, &gpl->parent);
  }

  /* materials */
  for (int a = 0; a < gpd->totcol; a++) {
    BLO_read_id_address(reader, gpd->id.lib, &gpd->mat[a]);
  }
}

static void greasepencil_blend_read_expand(BlendExpander *expander, ID *id)
{
  bGPdata *gpd = (bGPdata *)id;
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    BLO_expand(expander, gpl->parent);
  }

  for (int a = 0; a < gpd->totcol; a++) {
    BLO_expand(expander, gpd->mat[a]);
  }
}

IDTypeInfo IDType_ID_GD = {
    .id_code = ID_GD,
    .id_filter = FILTER_ID_GD,
    .main_listbase_index = INDEX_ID_GD,
    .struct_size = sizeof(bGPdata),
    .name = "GPencil",
    .name_plural = "grease_pencils",
    .translation_context = BLT_I18NCONTEXT_ID_GPENCIL,
    .flags = IDTYPE_FLAGS_APPEND_IS_REUSABLE,
    .asset_type_info = NULL,

    .init_data = NULL,
    .copy_data = greasepencil_copy_data,
    .free_data = greasepencil_free_data,
    .make_local = NULL,
    .foreach_id = greasepencil_foreach_id,
    .foreach_cache = NULL,
    .foreach_path = NULL,
    .owner_get = NULL,

    .blend_write = greasepencil_blend_write,
    .blend_read_data = greasepencil_blend_read_data,
    .blend_read_lib = greasepencil_blend_read_lib,
    .blend_read_expand = greasepencil_blend_read_expand,

    .blend_read_undo_preserve = NULL,

    .lib_override_apply_post = NULL,
};

/* ************************************************** */
/* Draw Engine */

void (*BKE_gpencil_batch_cache_dirty_tag_cb)(bGPdata *gpd) = NULL;
void (*BKE_gpencil_batch_cache_free_cb)(bGPdata *gpd) = NULL;

void BKE_gpencil_batch_cache_dirty_tag(bGPdata *gpd)
{
  if (gpd) {
    DEG_id_tag_update(&gpd->id, ID_RECALC_GEOMETRY);
    BKE_gpencil_batch_cache_dirty_tag_cb(gpd);
  }
}

void BKE_gpencil_batch_cache_free(bGPdata *gpd)
{
  if (gpd) {
    BKE_gpencil_batch_cache_free_cb(gpd);
  }
}

/* ************************************************** */
/* Memory Management */

void BKE_gpencil_free_point_weights(MDeformVert *dvert)
{
  if (dvert == NULL) {
    return;
  }
  MEM_SAFE_FREE(dvert->dw);
}

void BKE_gpencil_free_stroke_weights(bGPDstroke *gps)
{
  if (gps == NULL) {
    return;
  }

  if (gps->dvert == NULL) {
    return;
  }

  for (int i = 0; i < gps->totpoints; i++) {
    MDeformVert *dvert = &gps->dvert[i];
    BKE_gpencil_free_point_weights(dvert);
  }
}

void BKE_gpencil_free_stroke_editcurve(bGPDstroke *gps)
{
  if (gps == NULL) {
    return;
  }
  bGPDcurve *editcurve = gps->editcurve;
  if (editcurve == NULL) {
    return;
  }
  MEM_freeN(editcurve->curve_points);
  MEM_freeN(editcurve);
  gps->editcurve = NULL;
}

void BKE_gpencil_free_stroke(bGPDstroke *gps)
{
  if (gps == NULL) {
    return;
  }
  /* free stroke memory arrays, then stroke itself */
  if (gps->points) {
    MEM_freeN(gps->points);
  }
  if (gps->dvert) {
    BKE_gpencil_free_stroke_weights(gps);
    MEM_freeN(gps->dvert);
  }
  if (gps->triangles) {
    MEM_freeN(gps->triangles);
  }
  if (gps->editcurve != NULL) {
    BKE_gpencil_free_stroke_editcurve(gps);
  }

  MEM_freeN(gps);
}

bool BKE_gpencil_free_strokes(bGPDframe *gpf)
{
  bool changed = (BLI_listbase_is_empty(&gpf->strokes) == false);

  /* free strokes */
  LISTBASE_FOREACH_MUTABLE (bGPDstroke *, gps, &gpf->strokes) {
    BKE_gpencil_free_stroke(gps);
  }
  BLI_listbase_clear(&gpf->strokes);

  return changed;
}

void BKE_gpencil_free_frames(bGPDlayer *gpl)
{
  bGPDframe *gpf_next;

  /* error checking */
  if (gpl == NULL) {
    return;
  }

  /* free frames */
  for (bGPDframe *gpf = gpl->frames.first; gpf; gpf = gpf_next) {
    gpf_next = gpf->next;

    /* free strokes and their associated memory */
    BKE_gpencil_free_strokes(gpf);
    BLI_freelinkN(&gpl->frames, gpf);
  }
  gpl->actframe = NULL;
}

void BKE_gpencil_free_layer_masks(bGPDlayer *gpl)
{
  /* Free masks. */
  bGPDlayer_Mask *mask_next = NULL;
  for (bGPDlayer_Mask *mask = gpl->mask_layers.first; mask; mask = mask_next) {
    mask_next = mask->next;
    BLI_freelinkN(&gpl->mask_layers, mask);
  }
}
void BKE_gpencil_free_layers(ListBase *list)
{
  bGPDlayer *gpl_next;

  /* error checking */
  if (list == NULL) {
    return;
  }

  /* delete layers */
  for (bGPDlayer *gpl = list->first; gpl; gpl = gpl_next) {
    gpl_next = gpl->next;

    /* free layers and their data */
    BKE_gpencil_free_frames(gpl);

    /* Free masks. */
    BKE_gpencil_free_layer_masks(gpl);

    BLI_freelinkN(list, gpl);
  }
}

void BKE_gpencil_free_data(bGPdata *gpd, bool free_all)
{
  /* free layers */
  BKE_gpencil_free_layers(&gpd->layers);

  /* materials */
  MEM_SAFE_FREE(gpd->mat);

  BLI_freelistN(&gpd->vertex_group_names);

  BKE_gpencil_free_update_cache(gpd);

  /* free all data */
  if (free_all) {
    /* clear cache */
    BKE_gpencil_batch_cache_free(gpd);
  }
}

void BKE_gpencil_eval_delete(bGPdata *gpd_eval)
{
  BKE_gpencil_free_data(gpd_eval, true);
  BKE_libblock_free_data(&gpd_eval->id, false);
  BLI_assert(!gpd_eval->id.py_instance); /* Or call #BKE_libblock_free_data_py. */
  MEM_freeN(gpd_eval);
}

void BKE_gpencil_tag(bGPdata *gpd)
{
  DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
}

/* ************************************************** */
/* Container Creation */

bGPDframe *BKE_gpencil_frame_addnew(bGPDlayer *gpl, int cframe)
{
  bGPDframe *gpf = NULL, *gf = NULL;
  short state = 0;

  /* error checking */
  if (gpl == NULL) {
    return NULL;
  }

  /* allocate memory for this frame */
  gpf = MEM_callocN(sizeof(bGPDframe), "bGPDframe");
  gpf->framenum = cframe;

  /* find appropriate place to add frame */
  if (gpl->frames.first) {
    for (gf = gpl->frames.first; gf; gf = gf->next) {
      /* check if frame matches one that is supposed to be added */
      if (gf->framenum == cframe) {
        state = -1;
        break;
      }

      /* if current frame has already exceeded the frame to add, add before */
      if (gf->framenum > cframe) {
        BLI_insertlinkbefore(&gpl->frames, gf, gpf);
        state = 1;
        break;
      }
    }
  }

  /* check whether frame was added successfully */
  if (state == -1) {
    CLOG_ERROR(
        &LOG, "Frame (%d) existed already for this layer_active. Using existing frame", cframe);

    /* free the newly created one, and use the old one instead */
    MEM_freeN(gpf);

    /* return existing frame instead... */
    BLI_assert(gf != NULL);
    gpf = gf;
  }
  else if (state == 0) {
    /* add to end then! */
    BLI_addtail(&gpl->frames, gpf);
  }

  /* return frame */
  return gpf;
}

bGPDframe *BKE_gpencil_frame_addcopy(bGPDlayer *gpl, int cframe)
{
  bGPDframe *new_frame;
  bool found = false;

  /* Error checking/handling */
  if (gpl == NULL) {
    /* no layer */
    return NULL;
  }
  if (gpl->actframe == NULL) {
    /* no active frame, so just create a new one from scratch */
    return BKE_gpencil_frame_addnew(gpl, cframe);
  }

  /* Create a copy of the frame */
  new_frame = BKE_gpencil_frame_duplicate(gpl->actframe, true);

  /* Find frame to insert it before */
  LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
    if (gpf->framenum > cframe) {
      /* Add it here */
      BLI_insertlinkbefore(&gpl->frames, gpf, new_frame);

      found = true;
      break;
    }
    if (gpf->framenum == cframe) {
      /* This only happens when we're editing with framelock on...
       * - Delete the new frame and don't do anything else here...
       */
      BKE_gpencil_free_strokes(new_frame);
      MEM_freeN(new_frame);
      new_frame = NULL;

      found = true;
      break;
    }
  }

  if (found == false) {
    /* Add new frame to the end */
    BLI_addtail(&gpl->frames, new_frame);
  }

  /* Ensure that frame is set up correctly, and return it */
  if (new_frame) {
    new_frame->framenum = cframe;
    gpl->actframe = new_frame;
  }

  return new_frame;
}

bGPDlayer *BKE_gpencil_layer_addnew(bGPdata *gpd,
                                    const char *name,
                                    const bool setactive,
                                    const bool add_to_header)
{
  bGPDlayer *gpl = NULL;
  bGPDlayer *gpl_active = NULL;

  /* check that list is ok */
  if (gpd == NULL) {
    return NULL;
  }

  /* allocate memory for frame and add to end of list */
  gpl = MEM_callocN(sizeof(bGPDlayer), "bGPDlayer");

  gpl_active = BKE_gpencil_layer_active_get(gpd);

  /* Add to data-block. */
  if (add_to_header) {
    BLI_addhead(&gpd->layers, gpl);
  }
  else {
    if (gpl_active == NULL) {
      BLI_addtail(&gpd->layers, gpl);
    }
    else {
      /* if active layer, add after that layer */
      BLI_insertlinkafter(&gpd->layers, gpl_active, gpl);
    }
  }
  /* annotation vs GP Object behavior is slightly different */
  if (gpd->flag & GP_DATA_ANNOTATIONS) {
    /* set default color of new strokes for this layer */
    copy_v4_v4(gpl->color, U.gpencil_new_layer_col);
    gpl->opacity = 1.0f;

    /* set default thickness of new strokes for this layer */
    gpl->thickness = 3;

    /* Onion colors */
    ARRAY_SET_ITEMS(gpl->gcolor_prev, 0.302f, 0.851f, 0.302f);
    ARRAY_SET_ITEMS(gpl->gcolor_next, 0.250f, 0.1f, 1.0f);
  }
  else {
    /* thickness parameter represents "thickness change", not absolute thickness */
    gpl->thickness = 0;
    gpl->opacity = 1.0f;
    /* default channel color */
    ARRAY_SET_ITEMS(gpl->color, 0.2f, 0.2f, 0.2f);
    /* Default vertex mix. */
    gpl->vertex_paint_opacity = 1.0f;
    /* Enable onion skin. */
    gpl->onion_flag |= GP_LAYER_ONIONSKIN;
  }

  /* auto-name */
  BLI_strncpy(gpl->info, DATA_(name), sizeof(gpl->info));
  BLI_uniquename(&gpd->layers,
                 gpl,
                 (gpd->flag & GP_DATA_ANNOTATIONS) ? DATA_("Note") : DATA_("GP_Layer"),
                 '.',
                 offsetof(bGPDlayer, info),
                 sizeof(gpl->info));

  /* Enable always affected by scene lights. */
  gpl->flag |= GP_LAYER_USE_LIGHTS;

  /* Init transform. */
  zero_v3(gpl->location);
  zero_v3(gpl->rotation);
  copy_v3_fl(gpl->scale, 1.0f);
  loc_eul_size_to_mat4(gpl->layer_mat, gpl->location, gpl->rotation, gpl->scale);
  invert_m4_m4(gpl->layer_invmat, gpl->layer_mat);

  /* make this one the active one */
  if (setactive) {
    BKE_gpencil_layer_active_set(gpd, gpl);
  }

  /* return layer */
  return gpl;
}

bGPdata *BKE_gpencil_data_addnew(Main *bmain, const char name[])
{
  bGPdata *gpd;

  /* allocate memory for a new block */
  gpd = BKE_libblock_alloc(bmain, ID_GD, name, 0);

  /* initial settings */
  gpd->flag = (GP_DATA_DISPINFO | GP_DATA_EXPAND);

  /* general flags */
  gpd->flag |= GP_DATA_VIEWALIGN;
  /* always enable object onion skin switch */
  gpd->flag |= GP_DATA_SHOW_ONIONSKINS;
  /* GP object specific settings */
  ARRAY_SET_ITEMS(gpd->line_color, 0.6f, 0.6f, 0.6f, 0.5f);

  gpd->pixfactor = GP_DEFAULT_PIX_FACTOR;

  gpd->curve_edit_resolution = GP_DEFAULT_CURVE_RESOLUTION;
  gpd->curve_edit_threshold = GP_DEFAULT_CURVE_ERROR;
  gpd->curve_edit_corner_angle = GP_DEFAULT_CURVE_EDIT_CORNER_ANGLE;

  /* use adaptive curve resolution by default */
  gpd->flag |= GP_DATA_CURVE_ADAPTIVE_RESOLUTION;

  gpd->zdepth_offset = 0.150f;

  /* grid settings */
  ARRAY_SET_ITEMS(gpd->grid.color, 0.5f, 0.5f, 0.5f); /* Color */
  ARRAY_SET_ITEMS(gpd->grid.scale, 1.0f, 1.0f);       /* Scale */
  gpd->grid.lines = GP_DEFAULT_GRID_LINES;            /* Number of lines */

  /* Onion-skinning settings (data-block level) */
  gpd->onion_keytype = -1; /* All by default. */
  gpd->onion_flag |= (GP_ONION_GHOST_PREVCOL | GP_ONION_GHOST_NEXTCOL);
  gpd->onion_flag |= GP_ONION_FADE;
  gpd->onion_mode = GP_ONION_MODE_RELATIVE;
  gpd->onion_factor = 0.5f;
  ARRAY_SET_ITEMS(gpd->gcolor_prev, 0.145098f, 0.419608f, 0.137255f); /* green */
  ARRAY_SET_ITEMS(gpd->gcolor_next, 0.125490f, 0.082353f, 0.529412f); /* blue */
  gpd->gstep = 1;
  gpd->gstep_next = 1;

  return gpd;
}

/* ************************************************** */
/* Primitive Creation */
/* Utilities for easier bulk-creation of geometry */

bGPDstroke *BKE_gpencil_stroke_new(int mat_idx, int totpoints, short thickness)
{
  /* allocate memory for a new stroke */
  bGPDstroke *gps = MEM_callocN(sizeof(bGPDstroke), "gp_stroke");

  gps->thickness = thickness;
  gps->fill_opacity_fac = 1.0f;
  gps->hardeness = 1.0f;
  copy_v2_fl(gps->aspect_ratio, 1.0f);

  gps->uv_scale = 1.0f;

  gps->inittime = 0;

  gps->flag = GP_STROKE_3DSPACE;

  gps->totpoints = totpoints;
  if (gps->totpoints > 0) {
    gps->points = MEM_callocN(sizeof(bGPDspoint) * gps->totpoints, "gp_stroke_points");
  }
  else {
    gps->points = NULL;
  }

  /* initialize triangle memory to dummy data */
  gps->triangles = NULL;
  gps->tot_triangles = 0;

  gps->mat_nr = mat_idx;

  gps->dvert = NULL;
  gps->editcurve = NULL;

  return gps;
}

bGPDstroke *BKE_gpencil_stroke_add(
    bGPDframe *gpf, int mat_idx, int totpoints, short thickness, const bool insert_at_head)
{
  bGPDstroke *gps = BKE_gpencil_stroke_new(mat_idx, totpoints, thickness);

  /* Add to frame. */
  if ((gps != NULL) && (gpf != NULL)) {
    if (!insert_at_head) {
      BLI_addtail(&gpf->strokes, gps);
    }
    else {
      BLI_addhead(&gpf->strokes, gps);
    }
  }

  return gps;
}

bGPDstroke *BKE_gpencil_stroke_add_existing_style(
    bGPDframe *gpf, bGPDstroke *existing, int mat_idx, int totpoints, short thickness)
{
  bGPDstroke *gps = BKE_gpencil_stroke_add(gpf, mat_idx, totpoints, thickness, false);
  /* Copy run-time color data so that strokes added in the modifier has the style.
   * There are depsgraph reference pointers inside,
   * change the copy function if interfere with future drawing implementation. */
  memcpy(&gps->runtime, &existing->runtime, sizeof(bGPDstroke_Runtime));
  return gps;
}

bGPDcurve *BKE_gpencil_stroke_editcurve_new(const int tot_curve_points)
{
  bGPDcurve *new_gp_curve = (bGPDcurve *)MEM_callocN(sizeof(bGPDcurve), __func__);
  new_gp_curve->tot_curve_points = tot_curve_points;
  new_gp_curve->curve_points = (bGPDcurve_point *)MEM_callocN(
      sizeof(bGPDcurve_point) * tot_curve_points, __func__);

  return new_gp_curve;
}

/* ************************************************** */
/* Data Duplication */

void BKE_gpencil_stroke_weights_duplicate(bGPDstroke *gps_src, bGPDstroke *gps_dst)
{
  if (gps_src == NULL) {
    return;
  }
  BLI_assert(gps_src->totpoints == gps_dst->totpoints);

  BKE_defvert_array_copy(gps_dst->dvert, gps_src->dvert, gps_src->totpoints);
}

bGPDcurve *BKE_gpencil_stroke_curve_duplicate(bGPDcurve *gpc_src)
{
  bGPDcurve *gpc_dst = MEM_dupallocN(gpc_src);

  if (gpc_src->curve_points != NULL) {
    gpc_dst->curve_points = MEM_dupallocN(gpc_src->curve_points);
  }

  return gpc_dst;
}

bGPDstroke *BKE_gpencil_stroke_duplicate(bGPDstroke *gps_src,
                                         const bool dup_points,
                                         const bool dup_curve)
{
  bGPDstroke *gps_dst = NULL;

  gps_dst = MEM_dupallocN(gps_src);
  gps_dst->prev = gps_dst->next = NULL;
  gps_dst->triangles = MEM_dupallocN(gps_src->triangles);

  if (dup_points) {
    gps_dst->points = MEM_dupallocN(gps_src->points);

    if (gps_src->dvert != NULL) {
      gps_dst->dvert = MEM_dupallocN(gps_src->dvert);
      BKE_gpencil_stroke_weights_duplicate(gps_src, gps_dst);
    }
    else {
      gps_dst->dvert = NULL;
    }
  }
  else {
    gps_dst->points = NULL;
    gps_dst->dvert = NULL;
  }

  if (dup_curve && gps_src->editcurve != NULL) {
    gps_dst->editcurve = BKE_gpencil_stroke_curve_duplicate(gps_src->editcurve);
  }
  else {
    gps_dst->editcurve = NULL;
  }

  /* return new stroke */
  return gps_dst;
}

bGPDframe *BKE_gpencil_frame_duplicate(const bGPDframe *gpf_src, const bool dup_strokes)
{
  bGPDstroke *gps_dst = NULL;
  bGPDframe *gpf_dst;

  /* error checking */
  if (gpf_src == NULL) {
    return NULL;
  }

  /* make a copy of the source frame */
  gpf_dst = MEM_dupallocN(gpf_src);
  gpf_dst->prev = gpf_dst->next = NULL;

  /* Copy strokes. */
  BLI_listbase_clear(&gpf_dst->strokes);
  if (dup_strokes) {
    LISTBASE_FOREACH (bGPDstroke *, gps_src, &gpf_src->strokes) {
      /* make copy of source stroke */
      gps_dst = BKE_gpencil_stroke_duplicate(gps_src, true, true);
      BLI_addtail(&gpf_dst->strokes, gps_dst);
    }
  }

  /* return new frame */
  return gpf_dst;
}

void BKE_gpencil_frame_copy_strokes(bGPDframe *gpf_src, struct bGPDframe *gpf_dst)
{
  bGPDstroke *gps_dst = NULL;
  /* error checking */
  if ((gpf_src == NULL) || (gpf_dst == NULL)) {
    return;
  }

  /* copy strokes */
  BLI_listbase_clear(&gpf_dst->strokes);
  LISTBASE_FOREACH (bGPDstroke *, gps_src, &gpf_src->strokes) {
    /* make copy of source stroke */
    gps_dst = BKE_gpencil_stroke_duplicate(gps_src, true, true);
    BLI_addtail(&gpf_dst->strokes, gps_dst);
  }
}

bGPDlayer *BKE_gpencil_layer_duplicate(const bGPDlayer *gpl_src,
                                       const bool dup_frames,
                                       const bool dup_strokes)
{
  bGPDframe *gpf_dst;
  bGPDlayer *gpl_dst;

  /* error checking */
  if (gpl_src == NULL) {
    return NULL;
  }

  /* make a copy of source layer */
  gpl_dst = MEM_dupallocN(gpl_src);
  gpl_dst->prev = gpl_dst->next = NULL;

  /* Copy masks. */
  BKE_gpencil_layer_mask_copy(gpl_src, gpl_dst);

  /* copy frames */
  BLI_listbase_clear(&gpl_dst->frames);
  if (dup_frames) {
    LISTBASE_FOREACH (bGPDframe *, gpf_src, &gpl_src->frames) {
      /* make a copy of source frame */
      gpf_dst = BKE_gpencil_frame_duplicate(gpf_src, dup_strokes);
      BLI_addtail(&gpl_dst->frames, gpf_dst);

      /* if source frame was the current layer's 'active' frame, reassign that too */
      if (gpf_src == gpl_dst->actframe) {
        gpl_dst->actframe = gpf_dst;
      }
    }
  }

  /* return new layer */
  return gpl_dst;
}

void BKE_gpencil_data_copy_settings(const bGPdata *gpd_src, bGPdata *gpd_dst)
{
  gpd_dst->flag = gpd_src->flag;
  gpd_dst->curve_edit_resolution = gpd_src->curve_edit_resolution;
  gpd_dst->curve_edit_threshold = gpd_src->curve_edit_threshold;
  gpd_dst->curve_edit_corner_angle = gpd_src->curve_edit_corner_angle;
  gpd_dst->pixfactor = gpd_src->pixfactor;
  copy_v4_v4(gpd_dst->line_color, gpd_src->line_color);

  gpd_dst->onion_factor = gpd_src->onion_factor;
  gpd_dst->onion_mode = gpd_src->onion_mode;
  gpd_dst->onion_flag = gpd_src->onion_flag;
  gpd_dst->gstep = gpd_src->gstep;
  gpd_dst->gstep_next = gpd_src->gstep_next;

  copy_v3_v3(gpd_dst->gcolor_prev, gpd_src->gcolor_prev);
  copy_v3_v3(gpd_dst->gcolor_next, gpd_src->gcolor_next);

  gpd_dst->zdepth_offset = gpd_src->zdepth_offset;

  gpd_dst->totlayer = gpd_src->totlayer;
  gpd_dst->totframe = gpd_src->totframe;
  gpd_dst->totstroke = gpd_src->totstroke;
  gpd_dst->totpoint = gpd_src->totpoint;

  gpd_dst->draw_mode = gpd_src->draw_mode;
  gpd_dst->onion_keytype = gpd_src->onion_keytype;

  gpd_dst->select_last_index = gpd_src->select_last_index;
  gpd_dst->vertex_group_active_index = gpd_src->vertex_group_active_index;

  copy_v3_v3(gpd_dst->grid.color, gpd_src->grid.color);
  copy_v2_v2(gpd_dst->grid.scale, gpd_src->grid.scale);
  copy_v2_v2(gpd_dst->grid.offset, gpd_src->grid.offset);
  gpd_dst->grid.lines = gpd_src->grid.lines;
}

void BKE_gpencil_layer_copy_settings(const bGPDlayer *gpl_src, bGPDlayer *gpl_dst)
{
  gpl_dst->line_change = gpl_src->line_change;
  copy_v4_v4(gpl_dst->tintcolor, gpl_src->tintcolor);
  gpl_dst->opacity = gpl_src->opacity;
  gpl_dst->vertex_paint_opacity = gpl_src->vertex_paint_opacity;
  gpl_dst->pass_index = gpl_src->pass_index;
  gpl_dst->parent = gpl_src->parent;
  copy_m4_m4(gpl_dst->inverse, gpl_src->inverse);
  BLI_strncpy(gpl_dst->parsubstr, gpl_src->parsubstr, 64);
  gpl_dst->partype = gpl_src->partype;
  BLI_strncpy(gpl_dst->viewlayername, gpl_src->viewlayername, 64);
  copy_v3_v3(gpl_dst->location, gpl_src->location);
  copy_v3_v3(gpl_dst->rotation, gpl_src->rotation);
  copy_v3_v3(gpl_dst->scale, gpl_src->scale);
  copy_m4_m4(gpl_dst->layer_mat, gpl_src->layer_mat);
  copy_m4_m4(gpl_dst->layer_invmat, gpl_src->layer_invmat);
  gpl_dst->blend_mode = gpl_src->blend_mode;
  gpl_dst->flag = gpl_src->flag;
  gpl_dst->onion_flag = gpl_src->onion_flag;
}

void BKE_gpencil_frame_copy_settings(const bGPDframe *gpf_src, bGPDframe *gpf_dst)
{
  gpf_dst->flag = gpf_src->flag;
  gpf_dst->key_type = gpf_src->key_type;
  gpf_dst->framenum = gpf_src->framenum;
}

void BKE_gpencil_stroke_copy_settings(const bGPDstroke *gps_src, bGPDstroke *gps_dst)
{
  gps_dst->thickness = gps_src->thickness;
  gps_dst->flag = gps_src->flag;
  gps_dst->inittime = gps_src->inittime;
  gps_dst->mat_nr = gps_src->mat_nr;
  copy_v2_v2_short(gps_dst->caps, gps_src->caps);
  gps_dst->hardeness = gps_src->hardeness;
  copy_v2_v2(gps_dst->aspect_ratio, gps_src->aspect_ratio);
  gps_dst->fill_opacity_fac = gps_dst->fill_opacity_fac;
  copy_v3_v3(gps_dst->boundbox_min, gps_src->boundbox_min);
  copy_v3_v3(gps_dst->boundbox_max, gps_src->boundbox_max);
  gps_dst->uv_rotation = gps_src->uv_rotation;
  copy_v2_v2(gps_dst->uv_translation, gps_src->uv_translation);
  gps_dst->uv_scale = gps_src->uv_scale;
  gps_dst->select_index = gps_src->select_index;
  copy_v4_v4(gps_dst->vert_color_fill, gps_src->vert_color_fill);
}

bGPdata *BKE_gpencil_data_duplicate(Main *bmain, const bGPdata *gpd_src, bool internal_copy)
{
  bGPdata *gpd_dst;

  /* Yuck and super-uber-hyper yuck!!!
   * Should be replaceable with a no-main copy (LIB_ID_COPY_NO_MAIN etc.), but not sure about it,
   * so for now keep old code for that one. */

  /* error checking */
  if (gpd_src == NULL) {
    return NULL;
  }

  if (internal_copy) {
    /* make a straight copy for undo buffers used during stroke drawing */
    gpd_dst = MEM_dupallocN(gpd_src);
  }
  else {
    BLI_assert(bmain != NULL);
    gpd_dst = (bGPdata *)BKE_id_copy(bmain, &gpd_src->id);
  }

  /* Copy internal data (layers, etc.) */
  greasepencil_copy_data(bmain, &gpd_dst->id, &gpd_src->id, 0);

  /* return new */
  return gpd_dst;
}

/* ************************************************** */
/* GP Stroke API */

void BKE_gpencil_stroke_sync_selection(bGPdata *gpd, bGPDstroke *gps)
{
  bGPDspoint *pt;
  int i;

  /* error checking */
  if (gps == NULL) {
    return;
  }

  /* we'll stop when we find the first selected point,
   * so initially, we must deselect
   */
  gps->flag &= ~GP_STROKE_SELECT;
  BKE_gpencil_stroke_select_index_reset(gps);

  for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
    if (pt->flag & GP_SPOINT_SELECT) {
      gps->flag |= GP_STROKE_SELECT;
      break;
    }
  }

  if (gps->flag & GP_STROKE_SELECT) {
    BKE_gpencil_stroke_select_index_set(gpd, gps);
  }
}

void BKE_gpencil_curve_sync_selection(bGPdata *gpd, bGPDstroke *gps)
{
  bGPDcurve *gpc = gps->editcurve;
  if (gpc == NULL) {
    return;
  }

  gps->flag &= ~GP_STROKE_SELECT;
  BKE_gpencil_stroke_select_index_reset(gps);
  gpc->flag &= ~GP_CURVE_SELECT;

  bool is_selected = false;
  for (int i = 0; i < gpc->tot_curve_points; i++) {
    bGPDcurve_point *gpc_pt = &gpc->curve_points[i];
    BezTriple *bezt = &gpc_pt->bezt;

    if (BEZT_ISSEL_ANY(bezt)) {
      gpc_pt->flag |= GP_SPOINT_SELECT;
    }
    else {
      gpc_pt->flag &= ~GP_SPOINT_SELECT;
    }

    if (gpc_pt->flag & GP_SPOINT_SELECT) {
      is_selected = true;
    }
  }

  if (is_selected) {
    gpc->flag |= GP_CURVE_SELECT;
    gps->flag |= GP_STROKE_SELECT;
    BKE_gpencil_stroke_select_index_set(gpd, gps);
  }
}

void BKE_gpencil_stroke_select_index_set(bGPdata *gpd, bGPDstroke *gps)
{
  gpd->select_last_index++;
  gps->select_index = gpd->select_last_index;
}

void BKE_gpencil_stroke_select_index_reset(bGPDstroke *gps)
{
  gps->select_index = 0;
}

/* ************************************************** */
/* GP Frame API */

void BKE_gpencil_frame_delete_laststroke(bGPDlayer *gpl, bGPDframe *gpf)
{
  bGPDstroke *gps = (gpf) ? gpf->strokes.last : NULL;
  int cfra = (gpf) ? gpf->framenum : 0; /* assume that the current frame was not locked */

  /* error checking */
  if (ELEM(NULL, gpf, gps)) {
    return;
  }

  /* free the stroke and its data */
  if (gps->points) {
    MEM_freeN(gps->points);
  }
  if (gps->dvert) {
    BKE_gpencil_free_stroke_weights(gps);
    MEM_freeN(gps->dvert);
  }
  MEM_freeN(gps->triangles);
  BLI_freelinkN(&gpf->strokes, gps);

  /* if frame has no strokes after this, delete it */
  if (BLI_listbase_is_empty(&gpf->strokes)) {
    BKE_gpencil_layer_frame_delete(gpl, gpf);
    BKE_gpencil_layer_frame_get(gpl, cfra, GP_GETFRAME_USE_PREV);
  }
}

/* ************************************************** */
/* GP Layer API */

bool BKE_gpencil_layer_is_editable(const bGPDlayer *gpl)
{
  /* Sanity check */
  if (gpl == NULL) {
    return false;
  }

  /* Layer must be: Visible + Editable */
  if ((gpl->flag & (GP_LAYER_HIDE | GP_LAYER_LOCKED)) == 0) {
    return true;
  }

  /* Something failed */
  return false;
}

bGPDframe *BKE_gpencil_layer_frame_find(bGPDlayer *gpl, int cframe)
{
  bGPDframe *gpf;

  /* Search in reverse order, since this is often used for playback/adding,
   * where it's less likely that we're interested in the earlier frames
   */
  for (gpf = gpl->frames.last; gpf; gpf = gpf->prev) {
    if (gpf->framenum == cframe) {
      return gpf;
    }
  }

  return NULL;
}

bGPDframe *BKE_gpencil_layer_frame_get(bGPDlayer *gpl, int cframe, eGP_GetFrame_Mode addnew)
{
  bGPDframe *gpf = NULL;
  bool found = false;

  /* error checking */
  if (gpl == NULL) {
    return NULL;
  }

  /* check if there is already an active frame */
  if (gpl->actframe) {
    gpf = gpl->actframe;

    /* do not allow any changes to layer's active frame if layer is locked from changes
     * or if the layer has been set to stay on the current frame
     */
    if (gpl->flag & GP_LAYER_FRAMELOCK) {
      return gpf;
    }
    /* do not allow any changes to actframe if frame has painting tag attached to it */
    if (gpf->flag & GP_FRAME_PAINT) {
      return gpf;
    }

    /* try to find matching frame */
    if (gpf->framenum < cframe) {
      for (; gpf; gpf = gpf->next) {
        if (gpf->framenum == cframe) {
          found = true;
          break;
        }
        if ((gpf->next) && (gpf->next->framenum > cframe)) {
          found = true;
          break;
        }
      }

      /* set the appropriate frame */
      if (addnew) {
        if ((found) && (gpf->framenum == cframe)) {
          gpl->actframe = gpf;
        }
        else if (addnew == GP_GETFRAME_ADD_COPY) {
          gpl->actframe = BKE_gpencil_frame_addcopy(gpl, cframe);
        }
        else {
          gpl->actframe = BKE_gpencil_frame_addnew(gpl, cframe);
        }
      }
      else if (found) {
        gpl->actframe = gpf;
      }
      else {
        gpl->actframe = gpl->frames.last;
      }
    }
    else {
      for (; gpf; gpf = gpf->prev) {
        if (gpf->framenum <= cframe) {
          found = true;
          break;
        }
      }

      /* set the appropriate frame */
      if (addnew) {
        if ((found) && (gpf->framenum == cframe)) {
          gpl->actframe = gpf;
        }
        else if (addnew == GP_GETFRAME_ADD_COPY) {
          gpl->actframe = BKE_gpencil_frame_addcopy(gpl, cframe);
        }
        else {
          gpl->actframe = BKE_gpencil_frame_addnew(gpl, cframe);
        }
      }
      else if (found) {
        gpl->actframe = gpf;
      }
      else {
        gpl->actframe = gpl->frames.first;
      }
    }
  }
  else if (gpl->frames.first) {
    /* check which of the ends to start checking from */
    const int first = ((bGPDframe *)(gpl->frames.first))->framenum;
    const int last = ((bGPDframe *)(gpl->frames.last))->framenum;

    if (abs(cframe - first) > abs(cframe - last)) {
      /* find gp-frame which is less than or equal to cframe */
      for (gpf = gpl->frames.last; gpf; gpf = gpf->prev) {
        if (gpf->framenum <= cframe) {
          found = true;
          break;
        }
      }
    }
    else {
      /* find gp-frame which is less than or equal to cframe */
      for (gpf = gpl->frames.first; gpf; gpf = gpf->next) {
        if (gpf->framenum <= cframe) {
          found = true;
          break;
        }
      }
    }

    /* set the appropriate frame */
    if (addnew) {
      if ((found) && (gpf->framenum == cframe)) {
        gpl->actframe = gpf;
      }
      else {
        gpl->actframe = BKE_gpencil_frame_addnew(gpl, cframe);
      }
    }
    else if (found) {
      gpl->actframe = gpf;
    }
    else {
      /* If delete first frame, need to find one. */
      if (gpl->frames.first != NULL) {
        gpl->actframe = gpl->frames.first;
      }
      else {
        /* unresolved errogenous situation! */
        CLOG_STR_ERROR(&LOG, "cannot find appropriate gp-frame");
        /* gpl->actframe should still be NULL */
      }
    }
  }
  else {
    /* currently no frames (add if allowed to) */
    if (addnew) {
      gpl->actframe = BKE_gpencil_frame_addnew(gpl, cframe);
    }
    else {
      /* don't do anything... this may be when no frames yet! */
      /* gpl->actframe should still be NULL */
    }
  }

  /* Don't select first frame if greater than current frame. */
  if ((gpl->actframe != NULL) && (gpl->actframe == gpl->frames.first) &&
      (gpl->actframe->framenum > cframe)) {
    gpl->actframe = NULL;
  }

  /* return */
  return gpl->actframe;
}

bool BKE_gpencil_layer_frame_delete(bGPDlayer *gpl, bGPDframe *gpf)
{
  bool changed = false;

  /* error checking */
  if (ELEM(NULL, gpl, gpf)) {
    return false;
  }

  /* if this frame was active, make the previous frame active instead
   * since it's tricky to set active frame otherwise
   */
  if (gpl->actframe == gpf) {
    gpl->actframe = gpf->prev;
  }

  /* free the frame and its data */
  changed = BKE_gpencil_free_strokes(gpf);
  BLI_freelinkN(&gpl->frames, gpf);

  return changed;
}

bGPDlayer *BKE_gpencil_layer_named_get(bGPdata *gpd, const char *name)
{
  if (name[0] == '\0') {
    return NULL;
  }
  return BLI_findstring(&gpd->layers, name, offsetof(bGPDlayer, info));
}

bGPDlayer_Mask *BKE_gpencil_layer_mask_named_get(bGPDlayer *gpl, const char *name)
{
  if (name[0] == '\0') {
    return NULL;
  }
  return BLI_findstring(&gpl->mask_layers, name, offsetof(bGPDlayer_Mask, name));
}

bGPDlayer_Mask *BKE_gpencil_layer_mask_add(bGPDlayer *gpl, const char *name)
{

  bGPDlayer_Mask *mask = MEM_callocN(sizeof(bGPDlayer_Mask), "bGPDlayer_Mask");
  BLI_addtail(&gpl->mask_layers, mask);
  BLI_strncpy(mask->name, name, sizeof(mask->name));
  gpl->act_mask++;

  return mask;
}

void BKE_gpencil_layer_mask_remove(bGPDlayer *gpl, bGPDlayer_Mask *mask)
{
  BLI_freelinkN(&gpl->mask_layers, mask);
  gpl->act_mask--;
  CLAMP_MIN(gpl->act_mask, 0);
}

void BKE_gpencil_layer_mask_remove_ref(bGPdata *gpd, const char *name)
{
  bGPDlayer_Mask *mask_next;

  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    for (bGPDlayer_Mask *mask = gpl->mask_layers.first; mask; mask = mask_next) {
      mask_next = mask->next;
      if (STREQ(mask->name, name)) {
        BKE_gpencil_layer_mask_remove(gpl, mask);
      }
    }
  }
}

static int gpencil_cb_sort_masks(const void *arg1, const void *arg2)
{
  /* sort is inverted as layer list. */
  const struct bGPDlayer_Mask *mask1 = arg1;
  const struct bGPDlayer_Mask *mask2 = arg2;
  int val = 0;

  if (mask1->sort_index < mask2->sort_index) {
    val = 1;
  }
  else if (mask1->sort_index > mask2->sort_index) {
    val = -1;
  }

  return val;
}

void BKE_gpencil_layer_mask_sort(bGPdata *gpd, bGPDlayer *gpl)
{
  /* Update sort index. */
  LISTBASE_FOREACH (bGPDlayer_Mask *, mask, &gpl->mask_layers) {
    bGPDlayer *gpl_mask = BKE_gpencil_layer_named_get(gpd, mask->name);
    if (gpl_mask != NULL) {
      mask->sort_index = BLI_findindex(&gpd->layers, gpl_mask);
    }
    else {
      mask->sort_index = 0;
    }
  }
  BLI_listbase_sort(&gpl->mask_layers, gpencil_cb_sort_masks);
}

void BKE_gpencil_layer_mask_sort_all(bGPdata *gpd)
{
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    BKE_gpencil_layer_mask_sort(gpd, gpl);
  }
}

void BKE_gpencil_layer_mask_copy(const bGPDlayer *gpl_src, bGPDlayer *gpl_dst)
{
  BLI_listbase_clear(&gpl_dst->mask_layers);
  LISTBASE_FOREACH (bGPDlayer_Mask *, mask_src, &gpl_src->mask_layers) {
    bGPDlayer_Mask *mask_dst = MEM_dupallocN(mask_src);
    mask_dst->prev = mask_dst->next = NULL;
    BLI_addtail(&gpl_dst->mask_layers, mask_dst);
  }
}

void BKE_gpencil_layer_mask_cleanup(bGPdata *gpd, bGPDlayer *gpl)
{
  LISTBASE_FOREACH_MUTABLE (bGPDlayer_Mask *, mask, &gpl->mask_layers) {
    if (BKE_gpencil_layer_named_get(gpd, mask->name) == NULL) {
      BKE_gpencil_layer_mask_remove(gpl, mask);
    }
  }
}

void BKE_gpencil_layer_mask_cleanup_all_layers(bGPdata *gpd)
{
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    BKE_gpencil_layer_mask_cleanup(gpd, gpl);
  }
}

static int gpencil_cb_cmp_frame(void *thunk, const void *a, const void *b)
{
  const bGPDframe *frame_a = a;
  const bGPDframe *frame_b = b;

  if (frame_a->framenum < frame_b->framenum) {
    return -1;
  }
  if (frame_a->framenum > frame_b->framenum) {
    return 1;
  }
  if (thunk != NULL) {
    *((bool *)thunk) = true;
  }
  /* Sort selected last. */
  if ((frame_a->flag & GP_FRAME_SELECT) && ((frame_b->flag & GP_FRAME_SELECT) == 0)) {
    return 1;
  }
  return 0;
}

void BKE_gpencil_layer_frames_sort(struct bGPDlayer *gpl, bool *r_has_duplicate_frames)
{
  BLI_listbase_sort_r(&gpl->frames, gpencil_cb_cmp_frame, r_has_duplicate_frames);
}

bGPDlayer *BKE_gpencil_layer_active_get(bGPdata *gpd)
{
  /* error checking */
  if (ELEM(NULL, gpd, gpd->layers.first)) {
    return NULL;
  }

  /* loop over layers until found (assume only one active) */
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    if (gpl->flag & GP_LAYER_ACTIVE) {
      return gpl;
    }
  }

  /* no active layer found */
  return NULL;
}

bGPDlayer *BKE_gpencil_layer_get_by_name(bGPdata *gpd, char *name, int first_if_not_found)
{
  bGPDlayer *gpl;
  int i = 0;

  /* error checking */
  if (ELEM(NULL, gpd, gpd->layers.first)) {
    return NULL;
  }

  /* loop over layers until found (assume only one active) */
  for (gpl = gpd->layers.first; gpl; gpl = gpl->next) {
    if (STREQ(name, gpl->info)) {
      return gpl;
    }
    i++;
  }

  /* no such layer */
  if (first_if_not_found) {
    return gpd->layers.first;
  }
  return NULL;
}

void BKE_gpencil_layer_active_set(bGPdata *gpd, bGPDlayer *active)
{
  /* error checking */
  if (ELEM(NULL, gpd, gpd->layers.first, active)) {
    return;
  }

  /* loop over layers deactivating all */
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    gpl->flag &= ~GP_LAYER_ACTIVE;
    if (gpd->flag & GP_DATA_AUTOLOCK_LAYERS) {
      gpl->flag |= GP_LAYER_LOCKED;
    }
  }

  /* set as active one */
  active->flag |= GP_LAYER_ACTIVE;
  if (gpd->flag & GP_DATA_AUTOLOCK_LAYERS) {
    active->flag &= ~GP_LAYER_LOCKED;
  }
}

void BKE_gpencil_layer_autolock_set(bGPdata *gpd, const bool unlock)
{
  BLI_assert(gpd != NULL);

  if (gpd->flag & GP_DATA_AUTOLOCK_LAYERS) {
    bGPDlayer *layer_active = BKE_gpencil_layer_active_get(gpd);

    /* Lock all other layers */
    LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
      /* unlock active layer */
      if (gpl == layer_active) {
        gpl->flag &= ~GP_LAYER_LOCKED;
      }
      else {
        gpl->flag |= GP_LAYER_LOCKED;
      }
    }
  }
  else {
    /* If disable is better unlock all layers by default or it looks there is
     * a problem in the UI because the user expects all layers will be unlocked
     */
    if (unlock) {
      LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
        gpl->flag &= ~GP_LAYER_LOCKED;
      }
    }
  }
}

void BKE_gpencil_layer_delete(bGPdata *gpd, bGPDlayer *gpl)
{
  /* error checking */
  if (ELEM(NULL, gpd, gpl)) {
    return;
  }

  /* free layer */
  BKE_gpencil_free_frames(gpl);

  /* Free Masks. */
  BKE_gpencil_free_layer_masks(gpl);

  /* Remove any reference to that layer in masking lists. */
  BKE_gpencil_layer_mask_remove_ref(gpd, gpl->info);

  /* free icon providing preview of icon color */
  BKE_icon_delete(gpl->runtime.icon_id);

  BLI_freelinkN(&gpd->layers, gpl);
}

Material *BKE_gpencil_brush_material_get(Brush *brush)
{
  Material *ma = NULL;

  if ((brush != NULL) && (brush->gpencil_settings != NULL) &&
      (brush->gpencil_settings->material != NULL)) {
    ma = brush->gpencil_settings->material;
  }

  return ma;
}

void BKE_gpencil_brush_material_set(Brush *brush, Material *ma)
{
  BLI_assert(brush);
  BLI_assert(brush->gpencil_settings);
  if (brush->gpencil_settings->material != ma) {
    if (brush->gpencil_settings->material) {
      id_us_min(&brush->gpencil_settings->material->id);
    }
    if (ma) {
      id_us_plus(&ma->id);
    }
    brush->gpencil_settings->material = ma;
  }
}

Material *BKE_gpencil_object_material_ensure_from_brush(Main *bmain, Object *ob, Brush *brush)
{
  if (brush->gpencil_settings->flag & GP_BRUSH_MATERIAL_PINNED) {
    Material *ma = BKE_gpencil_brush_material_get(brush);

    /* check if the material is already on object material slots and add it if missing */
    if (ma && BKE_gpencil_object_material_index_get(ob, ma) < 0) {
      BKE_object_material_slot_add(bmain, ob);
      BKE_object_material_assign(bmain, ob, ma, ob->totcol, BKE_MAT_ASSIGN_USERPREF);
    }

    return ma;
  }

  /* using active material instead */
  return BKE_object_material_get(ob, ob->actcol);
}

int BKE_gpencil_object_material_ensure(Main *bmain, Object *ob, Material *material)
{
  if (!material) {
    return -1;
  }
  int index = BKE_gpencil_object_material_index_get(ob, material);
  if (index < 0) {
    BKE_object_material_slot_add(bmain, ob);
    BKE_object_material_assign(bmain, ob, material, ob->totcol, BKE_MAT_ASSIGN_USERPREF);
    return ob->totcol - 1;
  }
  return index;
}

Material *BKE_gpencil_object_material_new(Main *bmain, Object *ob, const char *name, int *r_index)
{
  Material *ma = BKE_gpencil_material_add(bmain, name);
  id_us_min(&ma->id); /* no users yet */

  BKE_object_material_slot_add(bmain, ob);
  BKE_object_material_assign(bmain, ob, ma, ob->totcol, BKE_MAT_ASSIGN_USERPREF);

  if (r_index) {
    *r_index = ob->actcol - 1;
  }
  return ma;
}

Material *BKE_gpencil_object_material_from_brush_get(Object *ob, Brush *brush)
{
  if ((brush) && (brush->gpencil_settings) &&
      (brush->gpencil_settings->flag & GP_BRUSH_MATERIAL_PINNED)) {
    Material *ma = BKE_gpencil_brush_material_get(brush);
    return ma;
  }

  return BKE_object_material_get(ob, ob->actcol);
}

int BKE_gpencil_object_material_get_index_from_brush(Object *ob, Brush *brush)
{
  if ((brush) && (brush->gpencil_settings->flag & GP_BRUSH_MATERIAL_PINNED)) {
    return BKE_gpencil_object_material_index_get(ob, brush->gpencil_settings->material);
  }

  return ob->actcol - 1;
}

Material *BKE_gpencil_object_material_ensure_from_active_input_toolsettings(Main *bmain,
                                                                            Object *ob,
                                                                            ToolSettings *ts)
{
  if (ts && ts->gp_paint && ts->gp_paint->paint.brush) {
    return BKE_gpencil_object_material_ensure_from_active_input_brush(
        bmain, ob, ts->gp_paint->paint.brush);
  }

  return BKE_gpencil_object_material_ensure_from_active_input_brush(bmain, ob, NULL);
}

Material *BKE_gpencil_object_material_ensure_from_active_input_brush(Main *bmain,
                                                                     Object *ob,
                                                                     Brush *brush)
{
  if (brush) {
    Material *ma = BKE_gpencil_object_material_ensure_from_brush(bmain, ob, brush);
    if (ma) {
      return ma;
    }
    if (brush->gpencil_settings->flag & GP_BRUSH_MATERIAL_PINNED) {
      /* it is easier to just unpin a NULL material, instead of setting a new one */
      brush->gpencil_settings->flag &= ~GP_BRUSH_MATERIAL_PINNED;
    }
  }
  return BKE_gpencil_object_material_ensure_from_active_input_material(ob);
}

Material *BKE_gpencil_object_material_ensure_from_active_input_material(Object *ob)
{
  Material *ma = BKE_object_material_get(ob, ob->actcol);
  if (ma) {
    return ma;
  }

  return BKE_material_default_gpencil();
}

Material *BKE_gpencil_object_material_ensure_active(Object *ob)
{
  Material *ma = NULL;

  /* sanity checks */
  if (ob == NULL) {
    return NULL;
  }

  ma = BKE_gpencil_object_material_ensure_from_active_input_material(ob);
  if (ma->gp_style == NULL) {
    BKE_gpencil_material_attr_init(ma);
  }

  return ma;
}

/* ************************************************** */
bool BKE_gpencil_stroke_select_check(const bGPDstroke *gps)
{
  const bGPDspoint *pt;
  int i;
  for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
    if (pt->flag & GP_SPOINT_SELECT) {
      return true;
    }
  }
  return false;
}

/* ************************************************** */
/* GP Object - Vertex Groups */

void BKE_gpencil_vgroup_remove(Object *ob, bDeformGroup *defgroup)
{
  bGPdata *gpd = ob->data;
  MDeformVert *dvert = NULL;

  const int def_nr = BLI_findindex(&gpd->vertex_group_names, defgroup);
  const int totgrp = BLI_listbase_count(&gpd->vertex_group_names);

  /* Remove points data */
  if (gpd) {
    LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
      LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
        LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
          if (gps->dvert != NULL) {
            for (int i = 0; i < gps->totpoints; i++) {
              dvert = &gps->dvert[i];
              MDeformWeight *dw = BKE_defvert_find_index(dvert, def_nr);
              if (dw != NULL) {
                BKE_defvert_remove_group(dvert, dw);
              }
              /* Reorganize weights for other groups after deleted one. */
              for (int g = 0; g < totgrp; g++) {
                dw = BKE_defvert_find_index(dvert, g);
                if ((dw != NULL) && (dw->def_nr > def_nr)) {
                  dw->def_nr--;
                }
              }
            }
          }
        }
      }
    }
  }

  /* Remove the group */
  BLI_freelinkN(&gpd->vertex_group_names, defgroup);
  DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
}

void BKE_gpencil_dvert_ensure(bGPDstroke *gps)
{
  if (gps->dvert == NULL) {
    gps->dvert = MEM_callocN(sizeof(MDeformVert) * gps->totpoints, "gp_stroke_weights");
  }
}

/* ************************************************** */

void BKE_gpencil_frame_range_selected(bGPDlayer *gpl, int *r_initframe, int *r_endframe)
{
  *r_initframe = gpl->actframe->framenum;
  *r_endframe = gpl->actframe->framenum;

  LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
    if (gpf->flag & GP_FRAME_SELECT) {
      if (gpf->framenum < *r_initframe) {
        *r_initframe = gpf->framenum;
      }
      if (gpf->framenum > *r_endframe) {
        *r_endframe = gpf->framenum;
      }
    }
  }
}

float BKE_gpencil_multiframe_falloff_calc(
    bGPDframe *gpf, int actnum, int f_init, int f_end, CurveMapping *cur_falloff)
{
  float fnum = 0.5f; /* default mid curve */
  float value;

  /* check curve is available */
  if (cur_falloff == NULL) {
    return 1.0f;
  }

  /* frames to the right of the active frame */
  if (gpf->framenum < actnum) {
    fnum = (float)(gpf->framenum - f_init) / (actnum - f_init);
    fnum *= 0.5f;
    value = BKE_curvemapping_evaluateF(cur_falloff, 0, fnum);
  }
  /* frames to the left of the active frame */
  else if (gpf->framenum > actnum) {
    fnum = (float)(gpf->framenum - actnum) / (f_end - actnum);
    fnum *= 0.5f;
    value = BKE_curvemapping_evaluateF(cur_falloff, 0, fnum + 0.5f);
  }
  else {
    /* Center of the curve. */
    value = BKE_curvemapping_evaluateF(cur_falloff, 0, 0.5f);
  }

  return value;
}

void BKE_gpencil_material_index_reassign(bGPdata *gpd, int totcol, int index)
{
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        /* reassign strokes */
        if ((gps->mat_nr > index) || (gps->mat_nr > totcol - 1)) {
          gps->mat_nr--;
          CLAMP_MIN(gps->mat_nr, 0);
        }
      }
    }
  }
}

bool BKE_gpencil_material_index_used(bGPdata *gpd, int index)
{
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        if (gps->mat_nr == index) {
          return true;
        }
      }
    }
  }

  return false;
}

void BKE_gpencil_material_remap(struct bGPdata *gpd,
                                const unsigned int *remap,
                                unsigned int remap_len)
{
  const short remap_len_short = (short)remap_len;

#define MAT_NR_REMAP(n) \
  if (n < remap_len_short) { \
    BLI_assert(n >= 0 && remap[n] < remap_len_short); \
    n = remap[n]; \
  } \
  ((void)0)

  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        /* reassign strokes */
        MAT_NR_REMAP(gps->mat_nr);
      }
    }
  }

#undef MAT_NR_REMAP
}

bool BKE_gpencil_merge_materials_table_get(Object *ob,
                                           const float hue_threshold,
                                           const float sat_threshold,
                                           const float val_threshold,
                                           GHash *r_mat_table)
{
  bool changed = false;

  Material *ma_primary = NULL;
  Material *ma_secondary = NULL;
  MaterialGPencilStyle *gp_style_primary = NULL;
  MaterialGPencilStyle *gp_style_secondary = NULL;
  GHash *mat_used = BLI_ghash_int_new(__func__);

  short *totcol = BKE_object_material_len_p(ob);
  if (totcol == 0) {
    return changed;
  }

  for (int idx_primary = 0; idx_primary < *totcol; idx_primary++) {
    /* Read primary material to compare. */
    ma_primary = BKE_gpencil_material(ob, idx_primary + 1);
    if (ma_primary == NULL) {
      continue;
    }
    for (int idx_secondary = 0; idx_secondary < *totcol; idx_secondary++) {
      if ((idx_secondary == idx_primary) ||
          BLI_ghash_haskey(r_mat_table, POINTER_FROM_INT(idx_secondary))) {
        continue;
      }
      if (BLI_ghash_haskey(mat_used, POINTER_FROM_INT(idx_secondary))) {
        continue;
      }

      /* Read secondary material to compare with primary material. */
      ma_secondary = BKE_gpencil_material(ob, idx_secondary + 1);
      if ((ma_secondary == NULL) ||
          (BLI_ghash_haskey(r_mat_table, POINTER_FROM_INT(idx_secondary)))) {
        continue;
      }
      gp_style_primary = ma_primary->gp_style;
      gp_style_secondary = ma_secondary->gp_style;

      if ((gp_style_primary == NULL) || (gp_style_secondary == NULL) ||
          (gp_style_secondary->flag & GP_MATERIAL_LOCKED)) {
        continue;
      }

      /* Check materials have the same mode. */
      if (gp_style_primary->mode != gp_style_secondary->mode) {
        continue;
      }

      /* Check materials have same stroke and fill attributes. */
      if ((gp_style_primary->flag & GP_MATERIAL_STROKE_SHOW) !=
          (gp_style_secondary->flag & GP_MATERIAL_STROKE_SHOW)) {
        continue;
      }

      if ((gp_style_primary->flag & GP_MATERIAL_FILL_SHOW) !=
          (gp_style_secondary->flag & GP_MATERIAL_FILL_SHOW)) {
        continue;
      }

      /* Check materials have the same type. */
      if ((gp_style_primary->stroke_style != gp_style_secondary->stroke_style) ||
          (gp_style_primary->fill_style != gp_style_secondary->fill_style)) {
        continue;
      }

      float s_hsv_a[3], s_hsv_b[3], f_hsv_a[3], f_hsv_b[3], col[3];
      zero_v3(s_hsv_a);
      zero_v3(s_hsv_b);
      zero_v3(f_hsv_a);
      zero_v3(f_hsv_b);

      copy_v3_v3(col, gp_style_primary->stroke_rgba);
      rgb_to_hsv_compat_v(col, s_hsv_a);
      copy_v3_v3(col, gp_style_secondary->stroke_rgba);
      rgb_to_hsv_compat_v(col, s_hsv_b);

      copy_v3_v3(col, gp_style_primary->fill_rgba);
      rgb_to_hsv_compat_v(col, f_hsv_a);
      copy_v3_v3(col, gp_style_secondary->fill_rgba);
      rgb_to_hsv_compat_v(col, f_hsv_b);

      /* Check stroke and fill color. */
      if ((!compare_ff(s_hsv_a[0], s_hsv_b[0], hue_threshold)) ||
          (!compare_ff(s_hsv_a[1], s_hsv_b[1], sat_threshold)) ||
          (!compare_ff(s_hsv_a[2], s_hsv_b[2], val_threshold)) ||
          (!compare_ff(f_hsv_a[0], f_hsv_b[0], hue_threshold)) ||
          (!compare_ff(f_hsv_a[1], f_hsv_b[1], sat_threshold)) ||
          (!compare_ff(f_hsv_a[2], f_hsv_b[2], val_threshold)) ||
          (!compare_ff(gp_style_primary->stroke_rgba[3],
                       gp_style_secondary->stroke_rgba[3],
                       val_threshold)) ||
          (!compare_ff(
              gp_style_primary->fill_rgba[3], gp_style_secondary->fill_rgba[3], val_threshold))) {
        continue;
      }

      /* Save conversion indexes. */
      if (!BLI_ghash_haskey(r_mat_table, POINTER_FROM_INT(idx_secondary))) {
        BLI_ghash_insert(
            r_mat_table, POINTER_FROM_INT(idx_secondary), POINTER_FROM_INT(idx_primary));
        changed = true;

        if (!BLI_ghash_haskey(mat_used, POINTER_FROM_INT(idx_primary))) {
          BLI_ghash_insert(mat_used, POINTER_FROM_INT(idx_primary), POINTER_FROM_INT(idx_primary));
        }
      }
    }
  }
  /* Free hash memory. */
  BLI_ghash_free(mat_used, NULL, NULL);

  return changed;
}

bool BKE_gpencil_merge_materials(Object *ob,
                                 const float hue_threshold,
                                 const float sat_threshold,
                                 const float val_threshold,
                                 int *r_removed)
{
  bGPdata *gpd = ob->data;

  short *totcol = BKE_object_material_len_p(ob);
  if (totcol == 0) {
    *r_removed = 0;
    return 0;
  }

  /* Review materials. */
  GHash *mat_table = BLI_ghash_int_new(__func__);

  bool changed = BKE_gpencil_merge_materials_table_get(
      ob, hue_threshold, sat_threshold, val_threshold, mat_table);

  *r_removed = BLI_ghash_len(mat_table);

  /* Update stroke material index. */
  if (changed) {
    LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
      if (gpl->flag & GP_LAYER_HIDE) {
        continue;
      }

      LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
        LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
          /* Check if the color is editable. */
          MaterialGPencilStyle *gp_style = BKE_gpencil_material_settings(ob, gps->mat_nr + 1);
          if (gp_style != NULL) {
            if (gp_style->flag & GP_MATERIAL_HIDE) {
              continue;
            }
            if (((gpl->flag & GP_LAYER_UNLOCK_COLOR) == 0) &&
                (gp_style->flag & GP_MATERIAL_LOCKED)) {
              continue;
            }
          }

          if (BLI_ghash_haskey(mat_table, POINTER_FROM_INT(gps->mat_nr))) {
            int *idx = BLI_ghash_lookup(mat_table, POINTER_FROM_INT(gps->mat_nr));
            gps->mat_nr = POINTER_AS_INT(idx);
          }
        }
      }
    }
  }

  /* Free hash memory. */
  BLI_ghash_free(mat_table, NULL, NULL);

  return changed;
}

void BKE_gpencil_stats_update(bGPdata *gpd)
{
  gpd->totlayer = 0;
  gpd->totframe = 0;
  gpd->totstroke = 0;
  gpd->totpoint = 0;

  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    gpd->totlayer++;
    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
      gpd->totframe++;
      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        gpd->totstroke++;
        gpd->totpoint += gps->totpoints;
      }
    }
  }
}

int BKE_gpencil_object_material_index_get(Object *ob, Material *ma)
{
  short *totcol = BKE_object_material_len_p(ob);
  Material *read_ma = NULL;
  for (short i = 0; i < *totcol; i++) {
    read_ma = BKE_object_material_get(ob, i + 1);
    if (ma == read_ma) {
      return i;
    }
  }

  return -1;
}

int BKE_gpencil_object_material_index_get_by_name(Object *ob, const char *name)
{
  short *totcol = BKE_object_material_len_p(ob);
  Material *read_ma = NULL;
  for (short i = 0; i < *totcol; i++) {
    read_ma = BKE_object_material_get(ob, i + 1);
    /* Material names are like "MAMaterial.001" */
    if (STREQ(name, &read_ma->id.name[2])) {
      return i;
    }
  }

  return -1;
}

Material *BKE_gpencil_object_material_ensure_by_name(Main *bmain,
                                                     Object *ob,
                                                     const char *name,
                                                     int *r_index)
{
  int index = BKE_gpencil_object_material_index_get_by_name(ob, name);
  if (index != -1) {
    *r_index = index;
    return BKE_object_material_get(ob, index + 1);
  }
  return BKE_gpencil_object_material_new(bmain, ob, name, r_index);
}

void BKE_gpencil_palette_ensure(Main *bmain, Scene *scene)
{
  const char *hexcol[] = {
      "FFFFFF", "F2F2F2", "E6E6E6", "D9D9D9", "CCCCCC", "BFBFBF", "B2B2B2", "A6A6A6", "999999",
      "8C8C8C", "808080", "737373", "666666", "595959", "4C4C4C", "404040", "333333", "262626",
      "1A1A1A", "000000", "F2FC24", "FFEA00", "FEA711", "FE8B68", "FB3B02", "FE3521", "D00000",
      "A81F3D", "780422", "2B0000", "F1E2C5", "FEE4B3", "FEDABB", "FEC28E", "D88F57", "BD6340",
      "A2402B", "63352D", "6B2833", "34120C", "E7CB8F", "D1B38B", "C1B17F", "D7980B", "FFB100",
      "FE8B00", "FF6A00", "B74100", "5F3E1D", "3B2300", "FECADA", "FE65CB", "FE1392", "DD3062",
      "C04A6D", "891688", "4D2689", "441521", "2C1139", "241422", "FFFF7D", "FFFF00", "FF7F00",
      "FF7D7D", "FF7DFF", "FF00FE", "FF007F", "FF0000", "7F0000", "0A0A00", "F6FDFF", "E9F7FF",
      "CFE6FE", "AAC7FE", "77B3FE", "1E74FD", "0046AA", "2F4476", "003052", "0E0E25", "EEF5F0",
      "D6E5DE", "ACD8B9", "6CADC6", "42A9AF", "007F7F", "49675C", "2E4E4E", "1D3239", "0F1C21",
      "D8FFF4", "B8F4F5", "AECCB5", "76C578", "358757", "409B68", "468768", "1F512B", "2A3C37",
      "122E1D", "EFFFC9", "E6F385", "BCF51C", "D4DC18", "82D322", "5C7F00", "59932B", "297F00",
      "004320", "1C3322", "00FF7F", "00FF00", "7DFF7D", "7DFFFF", "00FFFF", "7D7DFF", "7F00FF",
      "0000FF", "3F007F", "00007F"};

  ToolSettings *ts = scene->toolsettings;
  if (ts->gp_paint->paint.palette != NULL) {
    return;
  }

  /* Try to find the default palette. */
  const char *palette_id = "Palette";
  struct Palette *palette = BLI_findstring(&bmain->palettes, palette_id, offsetof(ID, name) + 2);

  if (palette == NULL) {
    /* Fall back to the first palette. */
    palette = bmain->palettes.first;
  }

  if (palette == NULL) {
    /* Fall back to creating a palette. */
    palette = BKE_palette_add(bmain, palette_id);
    id_us_min(&palette->id);

    /* Create Colors. */
    for (int i = 0; i < ARRAY_SIZE(hexcol); i++) {
      PaletteColor *palcol = BKE_palette_color_add(palette);
      hex_to_rgb(hexcol[i], palcol->rgb, palcol->rgb + 1, palcol->rgb + 2);
    }
  }

  BLI_assert(palette != NULL);
  BKE_paint_palette_set(&ts->gp_paint->paint, palette);
  BKE_paint_palette_set(&ts->gp_vertexpaint->paint, palette);
}

bool BKE_gpencil_from_image(
    SpaceImage *sima, bGPdata *gpd, bGPDframe *gpf, const float size, const bool mask)
{
  Image *image = sima->image;
  bool done = false;

  if (image == NULL) {
    return false;
  }

  ImageUser iuser = sima->iuser;
  void *lock;
  ImBuf *ibuf;

  ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);

  if (ibuf && ibuf->rect) {
    int img_x = ibuf->x;
    int img_y = ibuf->y;

    float color[4];
    bGPDspoint *pt;
    for (int row = 0; row < img_y; row++) {
      /* Create new stroke */
      bGPDstroke *gps = BKE_gpencil_stroke_add(gpf, 0, img_x, size * 1000, false);
      done = true;
      for (int col = 0; col < img_x; col++) {
        IMB_sampleImageAtLocation(ibuf, col, row, true, color);
        pt = &gps->points[col];
        pt->pressure = 1.0f;
        pt->x = col * size;
        pt->z = row * size;
        if (!mask) {
          copy_v3_v3(pt->vert_color, color);
          pt->vert_color[3] = 1.0f;
          pt->strength = color[3];
        }
        else {
          zero_v3(pt->vert_color);
          pt->vert_color[3] = 1.0f;
          pt->strength = 1.0f - color[3];
        }

        /* Selet Alpha points. */
        if (pt->strength < 0.03f) {
          gps->flag |= GP_STROKE_SELECT;
          pt->flag |= GP_SPOINT_SELECT;
        }
      }

      if (gps->flag & GP_STROKE_SELECT) {
        BKE_gpencil_stroke_select_index_set(gpd, gps);
      }

      BKE_gpencil_stroke_geometry_update(gpd, gps);
    }
  }

  /* Free memory. */
  BKE_image_release_ibuf(image, ibuf, lock);

  return done;
}

/**
 * Helper to check if a layers is used as mask
 * \param view_layer: Actual view layer.
 * \param gpd: Grease pencil data-block.
 * \param gpl_mask: Actual Layer.
 * \return True if the layer is used as mask.
 */
static bool gpencil_is_layer_mask(ViewLayer *view_layer, bGPdata *gpd, bGPDlayer *gpl_mask)
{
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    if ((gpl->viewlayername[0] != '\0') && (!STREQ(view_layer->name, gpl->viewlayername))) {
      continue;
    }

    /* Skip if masks are disabled for this view layer. */
    if (gpl->flag & GP_LAYER_DISABLE_MASKS_IN_VIEWLAYER) {
      continue;
    }

    LISTBASE_FOREACH (bGPDlayer_Mask *, mask, &gpl->mask_layers) {
      if (STREQ(gpl_mask->info, mask->name)) {
        return true;
      }
    }
  }

  return false;
}

/* -------------------------------------------------------------------- */
/** \name Iterator
 *
 * Iterate over all visible stroke of all visible layers inside a grease pencil datablock.
 * \{ */

void BKE_gpencil_visible_stroke_iter(bGPdata *gpd,
                                     gpIterCb layer_cb,
                                     gpIterCb stroke_cb,
                                     void *thunk)
{
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {

    if (gpl->flag & GP_LAYER_HIDE) {
      continue;
    }

    /* If scale to 0 the layer must be invisible. */
    if (is_zero_v3(gpl->scale)) {
      continue;
    }

    bGPDframe *act_gpf = gpl->actframe;
    if (layer_cb) {
      layer_cb(gpl, act_gpf, NULL, thunk);
    }

    if (act_gpf) {
      LISTBASE_FOREACH (bGPDstroke *, gps, &act_gpf->strokes) {
        if (gps->totpoints == 0) {
          continue;
        }
        stroke_cb(gpl, act_gpf, gps, thunk);
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Advanced Iterator
 *
 * Iterate over all visible stroke of all visible layers inside a gpObject.
 * Also take into account onion-skinning.
 * \{ */

void BKE_gpencil_visible_stroke_advanced_iter(ViewLayer *view_layer,
                                              Object *ob,
                                              gpIterCb layer_cb,
                                              gpIterCb stroke_cb,
                                              void *thunk,
                                              bool do_onion,
                                              int cfra)
{
  bGPdata *gpd = (bGPdata *)ob->data;
  const bool is_multiedit = (GPENCIL_MULTIEDIT_SESSIONS_ON(gpd) && (!GPENCIL_PLAY_ON(gpd)));
  const bool is_onion = do_onion && ((gpd->flag & GP_DATA_STROKE_WEIGHTMODE) == 0);
  const bool is_drawing = (gpd->runtime.sbuffer_used > 0);

  /* Onion skinning. */
  const bool onion_mode_abs = (gpd->onion_mode == GP_ONION_MODE_ABSOLUTE);
  const bool onion_mode_sel = (gpd->onion_mode == GP_ONION_MODE_SELECTED);
  const bool onion_loop = (gpd->onion_flag & GP_ONION_LOOP) != 0;
  const short onion_keytype = gpd->onion_keytype;

  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    /* Reset by layer. */
    bool is_before_first = false;

    bGPDframe *act_gpf = gpl->actframe;
    bGPDframe *sta_gpf = act_gpf;
    bGPDframe *end_gpf = act_gpf ? act_gpf->next : NULL;
    float prev_opacity = gpl->opacity;

    if (gpl->flag & GP_LAYER_HIDE) {
      continue;
    }

    /* If scale to 0 the layer must be invisible. */
    if (is_zero_v3(gpl->scale)) {
      continue;
    }

    /* Hide the layer if it's defined a view layer filter. This is used to
     * generate renders, putting only selected GP layers for each View Layer.
     * This is used only in final render and never in Viewport. */
    if ((view_layer != NULL) && (gpl->viewlayername[0] != '\0') &&
        (!STREQ(view_layer->name, gpl->viewlayername))) {
      /* Do not skip masks when rendering the view-layer so that it can still be used to clip
       * other layers. Instead set their opacity to zero. */
      if (gpencil_is_layer_mask(view_layer, gpd, gpl)) {
        gpl->opacity = 0.0f;
      }
      else {
        continue;
      }
    }

    if (is_multiedit) {
      sta_gpf = end_gpf = NULL;
      /* Check the whole range and tag the editable frames. */
      LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
        if (act_gpf != NULL && (gpf == act_gpf || (gpf->flag & GP_FRAME_SELECT))) {
          gpf->runtime.onion_id = 0;
          if (do_onion) {
            if (gpf->framenum < act_gpf->framenum) {
              gpf->runtime.onion_id = -1;
            }
            else {
              gpf->runtime.onion_id = 1;
            }
          }

          if (sta_gpf == NULL) {
            sta_gpf = gpf;
          }
          end_gpf = gpf->next;
        }
        else {
          gpf->runtime.onion_id = INT_MAX;
        }
      }
    }
    else if (is_onion && (gpl->onion_flag & GP_LAYER_ONIONSKIN)) {
      /* Special cases when cframe is before first frame. */
      bGPDframe *gpf_first = gpl->frames.first;
      if ((gpf_first != NULL) && (act_gpf != NULL) && (gpf_first->framenum > act_gpf->framenum)) {
        is_before_first = true;
      }
      if ((gpf_first != NULL) && (act_gpf == NULL)) {
        act_gpf = gpf_first;
        is_before_first = true;
      }

      if (act_gpf) {
        bGPDframe *last_gpf = gpl->frames.last;

        int frame_len = 0;
        LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
          gpf->runtime.frameid = frame_len++;
        }

        LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
          bool is_wrong_keytype = (onion_keytype > -1) && (gpf->key_type != onion_keytype);
          bool is_in_range;
          int delta = (onion_mode_abs) ? (gpf->framenum - cfra) :
                                         (gpf->runtime.frameid - act_gpf->runtime.frameid);

          if (is_before_first) {
            delta++;
          }

          if (onion_mode_sel) {
            is_in_range = (gpf->flag & GP_FRAME_SELECT) != 0;
          }
          else {
            is_in_range = (-delta <= gpd->gstep) && (delta <= gpd->gstep_next);

            if (onion_loop && !is_in_range) {
              /* We wrap the value using the last frame and 0 as reference. */
              /* FIXME: This might not be good for animations not starting at 0. */
              int shift = (onion_mode_abs) ? last_gpf->framenum : last_gpf->runtime.frameid;
              delta += (delta < 0) ? (shift + 1) : -(shift + 1);
              /* Test again with wrapped value. */
              is_in_range = (-delta <= gpd->gstep) && (delta <= gpd->gstep_next);
            }
          }
          /* Mask frames that have wrong keytype of are not in range. */
          gpf->runtime.onion_id = (is_wrong_keytype || !is_in_range) ? INT_MAX : delta;
        }
        /* Active frame is always shown. */
        if (!is_before_first || is_drawing) {
          act_gpf->runtime.onion_id = 0;
        }
      }

      sta_gpf = gpl->frames.first;
      end_gpf = NULL;
    }
    else {
      /* Bypass multiedit/onion skinning. */
      end_gpf = sta_gpf = NULL;
    }

    if (sta_gpf == NULL && act_gpf == NULL) {
      if (layer_cb) {
        layer_cb(gpl, act_gpf, NULL, thunk);
      }
      gpl->opacity = prev_opacity;
      continue;
    }

    /* Draw multiedit/onion skinning first */
    for (bGPDframe *gpf = sta_gpf; gpf && gpf != end_gpf; gpf = gpf->next) {
      if ((gpf->runtime.onion_id == INT_MAX || gpf == act_gpf) && (!is_before_first)) {
        continue;
      }

      /* Only do once for frame before first. */
      if (is_before_first && gpf == act_gpf) {
        is_before_first = false;
      }

      if (layer_cb) {
        layer_cb(gpl, gpf, NULL, thunk);
      }

      if (stroke_cb) {
        LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
          if (gps->totpoints == 0) {
            continue;
          }
          stroke_cb(gpl, gpf, gps, thunk);
        }
      }
    }
    /* Draw Active frame on top. */
    /* Use evaluated frame (with modifiers for active stroke)/ */
    act_gpf = gpl->actframe;
    if (act_gpf) {
      act_gpf->runtime.onion_id = 0;
      if (layer_cb) {
        layer_cb(gpl, act_gpf, NULL, thunk);
      }

      /* If layer solo mode and Paint mode, only keyframes with data are displayed. */
      if (GPENCIL_PAINT_MODE(gpd) && (gpl->flag & GP_LAYER_SOLO_MODE) &&
          (act_gpf->framenum != cfra)) {
        gpl->opacity = prev_opacity;
        continue;
      }
      if (stroke_cb) {
        LISTBASE_FOREACH (bGPDstroke *, gps, &act_gpf->strokes) {
          if (gps->totpoints == 0) {
            continue;
          }
          stroke_cb(gpl, act_gpf, gps, thunk);
        }
      }
    }

    /* Restore the opacity in case it was overwritten (used to hide masks in render). */
    gpl->opacity = prev_opacity;
  }
}

void BKE_gpencil_frame_original_pointers_update(const struct bGPDframe *gpf_orig,
                                                const struct bGPDframe *gpf_eval)
{
  bGPDstroke *gps_eval = gpf_eval->strokes.first;
  LISTBASE_FOREACH (bGPDstroke *, gps_orig, &gpf_orig->strokes) {

    /* Assign original stroke pointer. */
    if (gps_eval != NULL) {
      gps_eval->runtime.gps_orig = gps_orig;

      /* Assign original point pointer. */
      for (int i = 0; i < gps_orig->totpoints; i++) {
        if (i > gps_eval->totpoints - 1) {
          break;
        }
        bGPDspoint *pt_orig = &gps_orig->points[i];
        bGPDspoint *pt_eval = &gps_eval->points[i];
        pt_orig->runtime.pt_orig = NULL;
        pt_orig->runtime.idx_orig = i;
        pt_eval->runtime.pt_orig = pt_orig;
        pt_eval->runtime.idx_orig = i;
      }
      /* Increase pointer. */
      gps_eval = gps_eval->next;
    }
  }
}

/**
 * Update original pointers in evaluated layer.
 * \param gpl_orig: Original grease-pencil layer.
 * \param gpl_eval: Evaluated grease pencil layer.
 */
void BKE_gpencil_layer_original_pointers_update(const struct bGPDlayer *gpl_orig,
                                                const struct bGPDlayer *gpl_eval)
{
  bGPDframe *gpf_eval = gpl_eval->frames.first;
  LISTBASE_FOREACH (bGPDframe *, gpf_orig, &gpl_orig->frames) {
    if (gpf_eval != NULL) {
      /* Update frame reference pointers. */
      gpf_eval->runtime.gpf_orig = (bGPDframe *)gpf_orig;
      BKE_gpencil_frame_original_pointers_update(gpf_orig, gpf_eval);
      gpf_eval = gpf_eval->next;
    }
  }
}

void BKE_gpencil_data_update_orig_pointers(const bGPdata *gpd_orig, const bGPdata *gpd_eval)
{
  /* Assign pointers to the original stroke and points to the evaluated data. This must
   * be done before applying any modifier because at this moment the structure is equals,
   * so we can assume the layer index is the same in both data-blocks.
   * This data will be used by operators. */

  bGPDlayer *gpl_eval = gpd_eval->layers.first;
  LISTBASE_FOREACH (bGPDlayer *, gpl_orig, &gpd_orig->layers) {
    if (gpl_eval != NULL) {
      /* Update layer reference pointers. */
      gpl_eval->runtime.gpl_orig = gpl_orig;
      BKE_gpencil_layer_original_pointers_update(gpl_orig, gpl_eval);
      gpl_eval = gpl_eval->next;
    }
  }
}

/**
 * Update pointers of eval data to original data to keep references.
 * \param ob_orig: Original grease pencil object
 * \param ob_eval: Evaluated grease pencil object
 */
void BKE_gpencil_update_orig_pointers(const Object *ob_orig, const Object *ob_eval)
{
  BKE_gpencil_data_update_orig_pointers((bGPdata *)ob_orig->data, (bGPdata *)ob_eval->data);
}

void BKE_gpencil_layer_transform_matrix_get(const Depsgraph *depsgraph,
                                            Object *obact,
                                            bGPDlayer *gpl,
                                            float diff_mat[4][4])
{
  Object *ob_eval = depsgraph != NULL ? DEG_get_evaluated_object(depsgraph, obact) : obact;
  Object *obparent = gpl->parent;
  Object *obparent_eval = depsgraph != NULL ? DEG_get_evaluated_object(depsgraph, obparent) :
                                              obparent;

  /* if not layer parented, try with object parented */
  if (obparent_eval == NULL) {
    if ((ob_eval != NULL) && (ob_eval->type == OB_GPENCIL)) {
      copy_m4_m4(diff_mat, ob_eval->obmat);
      mul_m4_m4m4(diff_mat, diff_mat, gpl->layer_mat);
      return;
    }
    /* not gpencil object */
    unit_m4(diff_mat);
    return;
  }

  if (ELEM(gpl->partype, PAROBJECT, PARSKEL)) {
    mul_m4_m4m4(diff_mat, obparent_eval->obmat, gpl->inverse);
    add_v3_v3(diff_mat[3], ob_eval->obmat[3]);
    mul_m4_m4m4(diff_mat, diff_mat, gpl->layer_mat);
    return;
  }
  if (gpl->partype == PARBONE) {
    bPoseChannel *pchan = BKE_pose_channel_find_name(obparent_eval->pose, gpl->parsubstr);
    if (pchan) {
      float tmp_mat[4][4];
      mul_m4_m4m4(tmp_mat, obparent_eval->obmat, pchan->pose_mat);
      mul_m4_m4m4(diff_mat, tmp_mat, gpl->inverse);
      add_v3_v3(diff_mat[3], ob_eval->obmat[3]);
    }
    else {
      /* if bone not found use object (armature) */
      mul_m4_m4m4(diff_mat, obparent_eval->obmat, gpl->inverse);
      add_v3_v3(diff_mat[3], ob_eval->obmat[3]);
    }
    mul_m4_m4m4(diff_mat, diff_mat, gpl->layer_mat);
    return;
  }

  unit_m4(diff_mat); /* not defined type */
}

void BKE_gpencil_update_layer_transforms(const Depsgraph *depsgraph, Object *ob)
{
  if (ob->type != OB_GPENCIL) {
    return;
  }

  bGPdata *gpd = (bGPdata *)ob->data;
  float cur_mat[4][4];

  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    bool changed = false;
    unit_m4(cur_mat);

    /* Skip non-visible layers. */
    if (gpl->flag & GP_LAYER_HIDE || is_zero_v3(gpl->scale)) {
      continue;
    }

    /* Skip empty layers. */
    if (BLI_listbase_is_empty(&gpl->frames)) {
      continue;
    }

    /* Determine frame range to transform. */
    bGPDframe *gpf_start = NULL;
    bGPDframe *gpf_end = NULL;

    /* If onion skinning is activated, consider all frames. */
    if (gpl->onion_flag & GP_LAYER_ONIONSKIN) {
      gpf_start = gpl->frames.first;
    }
    /* Otherwise, consider only active frame. */
    else {
      /* Skip layer if it has no active frame to transform. */
      if (gpl->actframe == NULL) {
        continue;
      }
      gpf_start = gpl->actframe;
      gpf_end = gpl->actframe->next;
    }

    if (gpl->parent != NULL) {
      Object *ob_parent = DEG_get_evaluated_object(depsgraph, gpl->parent);
      /* calculate new matrix */
      if (ELEM(gpl->partype, PAROBJECT, PARSKEL)) {
        mul_m4_m4m4(cur_mat, ob->imat, ob_parent->obmat);
      }
      else if (gpl->partype == PARBONE) {
        bPoseChannel *pchan = BKE_pose_channel_find_name(ob_parent->pose, gpl->parsubstr);
        if (pchan != NULL) {
          mul_m4_series(cur_mat, ob->imat, ob_parent->obmat, pchan->pose_mat);
        }
        else {
          unit_m4(cur_mat);
        }
      }
      changed = !equals_m4m4(gpl->inverse, cur_mat);
    }

    /* Calc local layer transform. */
    bool transformed = ((!is_zero_v3(gpl->location)) || (!is_zero_v3(gpl->rotation)) ||
                        (!is_one_v3(gpl->scale)));
    if (transformed) {
      loc_eul_size_to_mat4(gpl->layer_mat, gpl->location, gpl->rotation, gpl->scale);
    }

    /* Continue if no transformations are applied to this layer. */
    if (!changed && !transformed) {
      continue;
    }

    /* Iterate over frame range. */
    for (bGPDframe *gpf = gpf_start; gpf != NULL && gpf != gpf_end; gpf = gpf->next) {
      /* Skip frames without a valid onion skinning id (NOTE: active frame has one). */
      if (gpf->runtime.onion_id == INT_MAX) {
        continue;
      }

      /* Apply transformations only if needed. */
      if (changed || transformed) {
        LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
          bGPDspoint *pt;
          int i;
          for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
            if (changed) {
              mul_m4_v3(gpl->inverse, &pt->x);
              mul_m4_v3(cur_mat, &pt->x);
            }

            if (transformed) {
              mul_m4_v3(gpl->layer_mat, &pt->x);
            }
          }
        }
      }
    }
  }
}

int BKE_gpencil_material_find_index_by_name_prefix(Object *ob, const char *name_prefix)
{
  const int name_prefix_len = strlen(name_prefix);
  for (int i = 0; i < ob->totcol; i++) {
    Material *ma = BKE_object_material_get(ob, i + 1);
    if ((ma != NULL) && (ma->gp_style != NULL) &&
        (STREQLEN(ma->id.name + 2, name_prefix, name_prefix_len))) {
      return i;
    }
  }

  return -1;
}

void BKE_gpencil_frame_selected_hash(bGPdata *gpd, struct GHash *r_list)
{
  const bool is_multiedit = (bool)GPENCIL_MULTIEDIT_SESSIONS_ON(gpd);
  bGPDlayer *gpl = BKE_gpencil_layer_active_get(gpd);

  LISTBASE_FOREACH (bGPDlayer *, gpl_iter, &gpd->layers) {
    if ((gpl != NULL) && (!is_multiedit) && (gpl != gpl_iter)) {
      continue;
    }

    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl_iter->frames) {
      if (((gpf == gpl->actframe) && (!is_multiedit)) ||
          ((gpf->flag & GP_FRAME_SELECT) && (is_multiedit))) {
        if (!BLI_ghash_lookup(r_list, POINTER_FROM_INT(gpf->framenum))) {
          BLI_ghash_insert(r_list, POINTER_FROM_INT(gpf->framenum), gpf);
        }
      }
    }
  }
}

bool BKE_gpencil_can_avoid_full_copy_on_write(const Depsgraph *depsgraph, bGPdata *gpd)
{
  /* For now, we only use the update cache in the active depsgraph. Otherwise we might access the
   * cache while another depsgraph frees it. */
  if (!DEG_is_active(depsgraph)) {
    return false;
  }

  GPencilUpdateCache *update_cache = gpd->runtime.update_cache;
  return update_cache != NULL && update_cache->flag != GP_UPDATE_NODE_FULL_COPY;
}

typedef struct tGPencilUpdateOnWriteTraverseData {
  bGPdata *gpd_eval;
  bGPDlayer *gpl_eval;
  bGPDframe *gpf_eval;
  bGPDstroke *gps_eval;
  int gpl_index;
  int gpf_index;
  int gps_index;
} tGPencilUpdateOnWriteTraverseData;

static bool gpencil_update_on_write_layer_cb(GPencilUpdateCache *gpl_cache, void *user_data)
{
  tGPencilUpdateOnWriteTraverseData *td = (tGPencilUpdateOnWriteTraverseData *)user_data;
  td->gpl_eval = BLI_findlinkfrom((Link *)td->gpl_eval, gpl_cache->index - td->gpl_index);
  td->gpl_index = gpl_cache->index;
  bGPDlayer *gpl = (bGPDlayer *)gpl_cache->data;

  if (gpl_cache->flag == GP_UPDATE_NODE_FULL_COPY) {
    bGPDlayer *gpl_eval_next = td->gpl_eval->next;
    BLI_assert(gpl != NULL);

    BKE_gpencil_layer_delete(td->gpd_eval, td->gpl_eval);

    td->gpl_eval = BKE_gpencil_layer_duplicate(gpl, true, true);
    BLI_insertlinkbefore(&td->gpd_eval->layers, gpl_eval_next, td->gpl_eval);

    BKE_gpencil_layer_original_pointers_update(gpl, td->gpl_eval);
    td->gpl_eval->runtime.gpl_orig = gpl;
    return true;
  }
  if (gpl_cache->flag == GP_UPDATE_NODE_LIGHT_COPY) {
    BLI_assert(gpl != NULL);
    BKE_gpencil_layer_copy_settings(gpl, td->gpl_eval);
    td->gpl_eval->runtime.gpl_orig = gpl;
  }

  td->gpf_eval = td->gpl_eval->frames.first;
  td->gpf_index = 0;
  return false;
}

static bool gpencil_update_on_write_frame_cb(GPencilUpdateCache *gpf_cache, void *user_data)
{
  tGPencilUpdateOnWriteTraverseData *td = (tGPencilUpdateOnWriteTraverseData *)user_data;
  td->gpf_eval = BLI_findlinkfrom((Link *)td->gpf_eval, gpf_cache->index - td->gpf_index);
  td->gpf_index = gpf_cache->index;

  bGPDframe *gpf = (bGPDframe *)gpf_cache->data;

  if (gpf_cache->flag == GP_UPDATE_NODE_FULL_COPY) {
    /* Do a full copy of the frame. */
    bGPDframe *gpf_eval_next = td->gpf_eval->next;
    BLI_assert(gpf != NULL);

    bool update_actframe = (td->gpl_eval->actframe == td->gpf_eval) ? true : false;
    BKE_gpencil_free_strokes(td->gpf_eval);
    BLI_freelinkN(&td->gpl_eval->frames, td->gpf_eval);

    td->gpf_eval = BKE_gpencil_frame_duplicate(gpf, true);
    BLI_insertlinkbefore(&td->gpl_eval->frames, gpf_eval_next, td->gpf_eval);

    BKE_gpencil_frame_original_pointers_update(gpf, td->gpf_eval);
    td->gpf_eval->runtime.gpf_orig = gpf;

    if (update_actframe) {
      td->gpl_eval->actframe = td->gpf_eval;
    }

    return true;
  }
  if (gpf_cache->flag == GP_UPDATE_NODE_LIGHT_COPY) {
    BLI_assert(gpf != NULL);
    BKE_gpencil_frame_copy_settings(gpf, td->gpf_eval);
    td->gpf_eval->runtime.gpf_orig = gpf;
  }

  td->gps_eval = td->gpf_eval->strokes.first;
  td->gps_index = 0;
  return false;
}

static bool gpencil_update_on_write_stroke_cb(GPencilUpdateCache *gps_cache, void *user_data)
{
  tGPencilUpdateOnWriteTraverseData *td = (tGPencilUpdateOnWriteTraverseData *)user_data;
  td->gps_eval = BLI_findlinkfrom((Link *)td->gps_eval, gps_cache->index - td->gps_index);
  td->gps_index = gps_cache->index;

  bGPDstroke *gps = (bGPDstroke *)gps_cache->data;

  if (gps_cache->flag == GP_UPDATE_NODE_FULL_COPY) {
    /* Do a full copy of the stroke. */
    bGPDstroke *gps_eval_next = td->gps_eval->next;
    BLI_assert(gps != NULL);

    BLI_remlink(&td->gpf_eval->strokes, td->gps_eval);
    BKE_gpencil_free_stroke(td->gps_eval);

    td->gps_eval = BKE_gpencil_stroke_duplicate(gps, true, true);
    BLI_insertlinkbefore(&td->gpf_eval->strokes, gps_eval_next, td->gps_eval);

    td->gps_eval->runtime.gps_orig = gps;

    /* Assign original pt pointers. */
    for (int i = 0; i < gps->totpoints; i++) {
      bGPDspoint *pt_orig = &gps->points[i];
      bGPDspoint *pt_eval = &td->gps_eval->points[i];
      pt_orig->runtime.pt_orig = NULL;
      pt_orig->runtime.idx_orig = i;
      pt_eval->runtime.pt_orig = pt_orig;
      pt_eval->runtime.idx_orig = i;
    }
  }
  else if (gps_cache->flag == GP_UPDATE_NODE_LIGHT_COPY) {
    BLI_assert(gps != NULL);
    BKE_gpencil_stroke_copy_settings(gps, td->gps_eval);
    td->gps_eval->runtime.gps_orig = gps;
  }

  return false;
}

/**
 * Update the geometry of the evaluated bGPdata.
 * This function will:
 *    1) Copy the original data over to the evaluated object.
 *    2) Update the original pointers in the runtime structs.
 */
void BKE_gpencil_update_on_write(bGPdata *gpd_orig, bGPdata *gpd_eval)
{
  GPencilUpdateCache *update_cache = gpd_orig->runtime.update_cache;

  /* We assume that a full copy is not needed and the update cache is populated. */
  if (update_cache == NULL || update_cache->flag == GP_UPDATE_NODE_FULL_COPY) {
    return;
  }

  if (update_cache->flag == GP_UPDATE_NODE_LIGHT_COPY) {
    BKE_gpencil_data_copy_settings(gpd_orig, gpd_eval);
  }

  GPencilUpdateCacheTraverseSettings ts = {{
      gpencil_update_on_write_layer_cb,
      gpencil_update_on_write_frame_cb,
      gpencil_update_on_write_stroke_cb,
  }};

  tGPencilUpdateOnWriteTraverseData data = {
      .gpd_eval = gpd_eval,
      .gpl_eval = gpd_eval->layers.first,
      .gpf_eval = NULL,
      .gps_eval = NULL,
      .gpl_index = 0,
      .gpf_index = 0,
      .gps_index = 0,
  };

  BKE_gpencil_traverse_update_cache(update_cache, &ts, &data);

  gpd_eval->flag |= GP_DATA_CACHE_IS_DIRTY;

  /* TODO: This might cause issues when we have multiple depsgraphs? */
  BKE_gpencil_free_update_cache(gpd_orig);
}

/** \} */
