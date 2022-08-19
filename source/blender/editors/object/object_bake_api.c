/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2004 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup edobj
 */

#include "MEM_guardedalloc.h"

#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"

#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_path_util.h"
#include "BLI_string.h"

#include "BKE_attribute.h"
#include "BKE_callbacks.h"
#include "BKE_context.h"
#include "BKE_editmesh.h"
#include "BKE_global.h"
#include "BKE_image.h"
#include "BKE_image_format.h"
#include "BKE_layer.h"
#include "BKE_lib_id.h"
#include "BKE_main.h"
#include "BKE_material.h"
#include "BKE_mesh.h"
#include "BKE_mesh_mapping.h"
#include "BKE_modifier.h"
#include "BKE_node.h"
#include "BKE_object.h"
#include "BKE_report.h"
#include "BKE_scene.h"
#include "BKE_screen.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"
#include "DEG_depsgraph_query.h"

#include "RE_engine.h"
#include "RE_pipeline.h"

#include "IMB_colormanagement.h"
#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_object.h"
#include "ED_screen.h"
#include "ED_uvedit.h"

#include "object_intern.h"

/* prototypes */
static void bake_set_props(wmOperator *op, Scene *scene);

typedef struct BakeAPIRender {
  /* Data to work on. */
  Main *main;
  Scene *scene;
  ViewLayer *view_layer;
  Object *ob;
  ListBase selected_objects;

  /* Baking settings. */
  eBakeTarget target;

  eScenePassType pass_type;
  int pass_filter;
  int margin;
  eBakeMarginType margin_type;

  bool is_clear;
  bool is_selected_to_active;
  bool is_cage;

  float cage_extrusion;
  float max_ray_distance;
  int normal_space;
  eBakeNormalSwizzle normal_swizzle[3];

  char uv_layer[MAX_CUSTOMDATA_LAYER_NAME];
  char custom_cage[MAX_NAME];

  /* Settings for external image saving. */
  eBakeSaveMode save_mode;
  char filepath[FILE_MAX];
  bool is_automatic_name;
  bool is_split_materials;
  int width;
  int height;
  const char *identifier;

  /* Baking render session. */
  Render *render;

  /* Progress Callbacks. */
  float *progress;
  short *do_update;

  /* Operator state. */
  ReportList *reports;
  int result;
  ScrArea *area;
} BakeAPIRender;

/* callbacks */

static void bake_progress_update(void *bjv, float progress)
{
  BakeAPIRender *bj = bjv;

  if (bj->progress && *bj->progress != progress) {
    *bj->progress = progress;

    /* make jobs timer to send notifier */
    *(bj->do_update) = true;
  }
}

/** Catch escape key to cancel. */
static int bake_modal(bContext *C, wmOperator *UNUSED(op), const wmEvent *event)
{
  /* no running blender, remove handler and pass through */
  if (0 == WM_jobs_test(CTX_wm_manager(C), CTX_data_scene(C), WM_JOB_TYPE_OBJECT_BAKE)) {
    return OPERATOR_FINISHED | OPERATOR_PASS_THROUGH;
  }

  /* running render */
  switch (event->type) {
    case EVT_ESCKEY: {
      G.is_break = true;
      return OPERATOR_RUNNING_MODAL;
    }
  }
  return OPERATOR_PASS_THROUGH;
}

/**
 * for exec() when there is no render job
 * NOTE: this won't check for the escape key being pressed, but doing so isn't thread-safe.
 */
static int bake_break(void *UNUSED(rjv))
{
  if (G.is_break) {
    return 1;
  }
  return 0;
}

static void bake_update_image(ScrArea *area, Image *image)
{
  if (area && area->spacetype == SPACE_IMAGE) { /* in case the user changed while baking */
    SpaceImage *sima = area->spacedata.first;
    if (sima) {
      sima->image = image;
    }
  }
}

static bool write_internal_bake_pixels(Image *image,
                                       const int image_tile_number,
                                       BakePixel pixel_array[],
                                       float *buffer,
                                       const int width,
                                       const int height,
                                       const int margin,
                                       const char margin_type,
                                       const bool is_clear,
                                       const bool is_noncolor,
                                       Mesh const *mesh_eval,
                                       char const *uv_layer,
                                       const float uv_offset[2])
{
  ImBuf *ibuf;
  void *lock;
  bool is_float;
  char *mask_buffer = NULL;
  const size_t pixels_num = (size_t)width * (size_t)height;

  ImageUser iuser;
  BKE_imageuser_default(&iuser);
  iuser.tile = image_tile_number;
  ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);

  if (!ibuf) {
    return false;
  }

  if (margin > 0 || !is_clear) {
    mask_buffer = MEM_callocN(sizeof(char) * pixels_num, "Bake Mask");
    RE_bake_mask_fill(pixel_array, pixels_num, mask_buffer);
  }

  is_float = (ibuf->rect_float != NULL);

  /* colormanagement conversions */
  if (!is_noncolor) {
    const char *from_colorspace;
    const char *to_colorspace;

    from_colorspace = IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_SCENE_LINEAR);

    if (is_float) {
      to_colorspace = IMB_colormanagement_get_float_colorspace(ibuf);
    }
    else {
      to_colorspace = IMB_colormanagement_get_rect_colorspace(ibuf);
    }

    if (from_colorspace != to_colorspace) {
      IMB_colormanagement_transform(
          buffer, ibuf->x, ibuf->y, ibuf->channels, from_colorspace, to_colorspace, false);
    }
  }

  /* populates the ImBuf */
  if (is_clear) {
    if (is_float) {
      IMB_buffer_float_from_float(ibuf->rect_float,
                                  buffer,
                                  ibuf->channels,
                                  IB_PROFILE_LINEAR_RGB,
                                  IB_PROFILE_LINEAR_RGB,
                                  false,
                                  ibuf->x,
                                  ibuf->y,
                                  ibuf->x,
                                  ibuf->x);
    }
    else {
      IMB_buffer_byte_from_float((uchar *)ibuf->rect,
                                 buffer,
                                 ibuf->channels,
                                 ibuf->dither,
                                 IB_PROFILE_SRGB,
                                 IB_PROFILE_SRGB,
                                 false,
                                 ibuf->x,
                                 ibuf->y,
                                 ibuf->x,
                                 ibuf->x);
    }
  }
  else {
    if (is_float) {
      IMB_buffer_float_from_float_mask(ibuf->rect_float,
                                       buffer,
                                       ibuf->channels,
                                       ibuf->x,
                                       ibuf->y,
                                       ibuf->x,
                                       ibuf->x,
                                       mask_buffer);
    }
    else {
      IMB_buffer_byte_from_float_mask((uchar *)ibuf->rect,
                                      buffer,
                                      ibuf->channels,
                                      ibuf->dither,
                                      false,
                                      ibuf->x,
                                      ibuf->y,
                                      ibuf->x,
                                      ibuf->x,
                                      mask_buffer);
    }
  }

  /* margins */
  if (margin > 0) {
    RE_bake_margin(ibuf, mask_buffer, margin, margin_type, mesh_eval, uv_layer, uv_offset);
  }

  ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
  BKE_image_mark_dirty(image, ibuf);

  if (ibuf->rect_float) {
    ibuf->userflags |= IB_RECT_INVALID;
  }

  /* force mipmap recalc */
  if (ibuf->mipmap[0]) {
    ibuf->userflags |= IB_MIPMAP_INVALID;
    imb_freemipmapImBuf(ibuf);
  }

  BKE_image_release_ibuf(image, ibuf, NULL);

  if (mask_buffer) {
    MEM_freeN(mask_buffer);
  }

  return true;
}

/* force OpenGL reload */
static void bake_targets_refresh(BakeTargets *targets)
{
  for (int i = 0; i < targets->images_num; i++) {
    Image *ima = targets->images[i].image;

    if (ima) {
      BKE_image_partial_update_mark_full_update(ima);
      BKE_image_free_gputextures(ima);
      DEG_id_tag_update(&ima->id, 0);
    }
  }
}

static bool write_external_bake_pixels(const char *filepath,
                                       BakePixel pixel_array[],
                                       float *buffer,
                                       const int width,
                                       const int height,
                                       const int margin,
                                       const int margin_type,
                                       ImageFormatData const *im_format,
                                       const bool is_noncolor,
                                       Mesh const *mesh_eval,
                                       char const *uv_layer,
                                       const float uv_offset[2])
{
  ImBuf *ibuf = NULL;
  bool ok = false;
  bool is_float;

  is_float = im_format->depth > 8;

  /* create a new ImBuf */
  ibuf = IMB_allocImBuf(width, height, im_format->planes, (is_float ? IB_rectfloat : IB_rect));

  if (!ibuf) {
    return false;
  }

  /* populates the ImBuf */
  if (is_float) {
    IMB_buffer_float_from_float(ibuf->rect_float,
                                buffer,
                                ibuf->channels,
                                IB_PROFILE_LINEAR_RGB,
                                IB_PROFILE_LINEAR_RGB,
                                false,
                                ibuf->x,
                                ibuf->y,
                                ibuf->x,
                                ibuf->x);
  }
  else {
    if (!is_noncolor) {
      const char *from_colorspace = IMB_colormanagement_role_colorspace_name_get(
          COLOR_ROLE_SCENE_LINEAR);
      const char *to_colorspace = IMB_colormanagement_get_rect_colorspace(ibuf);
      IMB_colormanagement_transform(
          buffer, ibuf->x, ibuf->y, ibuf->channels, from_colorspace, to_colorspace, false);
    }

    IMB_buffer_byte_from_float((uchar *)ibuf->rect,
                               buffer,
                               ibuf->channels,
                               ibuf->dither,
                               IB_PROFILE_SRGB,
                               IB_PROFILE_SRGB,
                               false,
                               ibuf->x,
                               ibuf->y,
                               ibuf->x,
                               ibuf->x);
  }

  /* margins */
  if (margin > 0) {
    char *mask_buffer = NULL;
    const size_t pixels_num = (size_t)width * (size_t)height;

    mask_buffer = MEM_callocN(sizeof(char) * pixels_num, "Bake Mask");
    RE_bake_mask_fill(pixel_array, pixels_num, mask_buffer);
    RE_bake_margin(ibuf, mask_buffer, margin, margin_type, mesh_eval, uv_layer, uv_offset);

    if (mask_buffer) {
      MEM_freeN(mask_buffer);
    }
  }

  if ((ok = BKE_imbuf_write(ibuf, filepath, im_format))) {
#ifndef WIN32
    chmod(filepath, S_IRUSR | S_IWUSR);
#endif
    // printf("%s saving bake map: '%s'\n", __func__, filepath);
  }

  /* garbage collection */
  IMB_freeImBuf(ibuf);

  return ok;
}

static bool is_noncolor_pass(eScenePassType pass_type)
{
  return ELEM(pass_type,
              SCE_PASS_Z,
              SCE_PASS_POSITION,
              SCE_PASS_NORMAL,
              SCE_PASS_VECTOR,
              SCE_PASS_INDEXOB,
              SCE_PASS_UV,
              SCE_PASS_INDEXMA);
}

/* if all is good tag image and return true */
static bool bake_object_check(ViewLayer *view_layer,
                              Object *ob,
                              const eBakeTarget target,
                              ReportList *reports)
{
  Base *base = BKE_view_layer_base_find(view_layer, ob);

  if (base == NULL) {
    BKE_reportf(reports, RPT_ERROR, "Object \"%s\" is not in view layer", ob->id.name + 2);
    return false;
  }

  if (!(base->flag & BASE_ENABLED_RENDER)) {
    BKE_reportf(reports, RPT_ERROR, "Object \"%s\" is not enabled for rendering", ob->id.name + 2);
    return false;
  }

  if (ob->type != OB_MESH) {
    BKE_reportf(reports, RPT_ERROR, "Object \"%s\" is not a mesh", ob->id.name + 2);
    return false;
  }

  Mesh *me = (Mesh *)ob->data;

  if (me->totpoly == 0) {
    BKE_reportf(reports, RPT_ERROR, "No faces found in the object \"%s\"", ob->id.name + 2);
    return false;
  }

  if (target == R_BAKE_TARGET_VERTEX_COLORS) {
    if (BKE_id_attributes_active_color_get(&me->id) == NULL) {
      BKE_reportf(reports,
                  RPT_ERROR,
                  "Mesh does not have an active color attribute \"%s\"",
                  me->id.name + 2);
      return false;
    }
  }
  else if (target == R_BAKE_TARGET_IMAGE_TEXTURES) {
    if (CustomData_get_active_layer_index(&me->ldata, CD_MLOOPUV) == -1) {
      BKE_reportf(
          reports, RPT_ERROR, "No active UV layer found in the object \"%s\"", ob->id.name + 2);
      return false;
    }

    for (int i = 0; i < ob->totcol; i++) {
      bNodeTree *ntree = NULL;
      bNode *node = NULL;
      const int mat_nr = i + 1;
      Image *image;
      ED_object_get_active_image(ob, mat_nr, &image, NULL, &node, &ntree);

      if (image) {

        if (node) {
          if (BKE_node_is_connected_to_output(ntree, node)) {
            /* we don't return false since this may be a false positive
             * this can't be RPT_ERROR though, otherwise it prevents
             * multiple highpoly objects to be baked at once */
            BKE_reportf(reports,
                        RPT_INFO,
                        "Circular dependency for image \"%s\" from object \"%s\"",
                        image->id.name + 2,
                        ob->id.name + 2);
          }
        }

        LISTBASE_FOREACH (ImageTile *, tile, &image->tiles) {
          ImageUser iuser;
          BKE_imageuser_default(&iuser);
          iuser.tile = tile->tile_number;

          void *lock;
          ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);

          if (ibuf) {
            BKE_image_release_ibuf(image, ibuf, lock);
          }
          else {
            BKE_reportf(reports,
                        RPT_ERROR,
                        "Uninitialized image \"%s\" from object \"%s\"",
                        image->id.name + 2,
                        ob->id.name + 2);

            BKE_image_release_ibuf(image, ibuf, lock);
            return false;
          }
        }
      }
      else {
        Material *mat = BKE_object_material_get(ob, mat_nr);
        if (mat != NULL) {
          BKE_reportf(reports,
                      RPT_INFO,
                      "No active image found in material \"%s\" (%d) for object \"%s\"",
                      mat->id.name + 2,
                      i,
                      ob->id.name + 2);
        }
        else {
          BKE_reportf(reports,
                      RPT_INFO,
                      "No active image found in material slot (%d) for object \"%s\"",
                      i,
                      ob->id.name + 2);
        }
        continue;
      }

      image->id.tag |= LIB_TAG_DOIT;
    }
  }

  return true;
}

static bool bake_pass_filter_check(eScenePassType pass_type,
                                   const int pass_filter,
                                   ReportList *reports)
{
  switch (pass_type) {
    case SCE_PASS_COMBINED:
      if ((pass_filter & R_BAKE_PASS_FILTER_EMIT) != 0) {
        return true;
      }

      if (((pass_filter & R_BAKE_PASS_FILTER_DIRECT) != 0) ||
          ((pass_filter & R_BAKE_PASS_FILTER_INDIRECT) != 0)) {
        if (((pass_filter & R_BAKE_PASS_FILTER_DIFFUSE) != 0) ||
            ((pass_filter & R_BAKE_PASS_FILTER_GLOSSY) != 0) ||
            ((pass_filter & R_BAKE_PASS_FILTER_TRANSM) != 0) ||
            ((pass_filter & R_BAKE_PASS_FILTER_SUBSURFACE) != 0)) {
          return true;
        }

        BKE_report(reports,
                   RPT_ERROR,
                   "Combined bake pass requires Emit, or a light pass with "
                   "Direct or Indirect contributions enabled");

        return false;
      }
      BKE_report(reports,
                 RPT_ERROR,
                 "Combined bake pass requires Emit, or a light pass with "
                 "Direct or Indirect contributions enabled");
      return false;
    case SCE_PASS_DIFFUSE_COLOR:
    case SCE_PASS_GLOSSY_COLOR:
    case SCE_PASS_TRANSM_COLOR:
    case SCE_PASS_SUBSURFACE_COLOR:
      if (((pass_filter & R_BAKE_PASS_FILTER_COLOR) != 0) ||
          ((pass_filter & R_BAKE_PASS_FILTER_DIRECT) != 0) ||
          ((pass_filter & R_BAKE_PASS_FILTER_INDIRECT) != 0)) {
        return true;
      }
      else {
        BKE_report(reports,
                   RPT_ERROR,
                   "Bake pass requires Direct, Indirect, or Color contributions to be enabled");
        return false;
      }
      break;
    default:
      return true;
      break;
  }
}

/* before even getting in the bake function we check for some basic errors */
static bool bake_objects_check(Main *bmain,
                               ViewLayer *view_layer,
                               Object *ob,
                               ListBase *selected_objects,
                               ReportList *reports,
                               const bool is_selected_to_active,
                               const eBakeTarget target)
{
  CollectionPointerLink *link;

  /* error handling and tag (in case multiple materials share the same image) */
  BKE_main_id_tag_idcode(bmain, ID_IM, LIB_TAG_DOIT, false);

  if (is_selected_to_active) {
    int tot_objects = 0;

    if (!bake_object_check(view_layer, ob, target, reports)) {
      return false;
    }

    for (link = selected_objects->first; link; link = link->next) {
      Object *ob_iter = (Object *)link->ptr.data;

      if (ob_iter == ob) {
        continue;
      }

      if (ELEM(ob_iter->type, OB_MESH, OB_FONT, OB_CURVES_LEGACY, OB_SURF, OB_MBALL) == false) {
        BKE_reportf(reports,
                    RPT_ERROR,
                    "Object \"%s\" is not a mesh or can't be converted to a mesh (Curve, Text, "
                    "Surface or Metaball)",
                    ob_iter->id.name + 2);
        return false;
      }
      tot_objects += 1;
    }

    if (tot_objects == 0) {
      BKE_report(reports, RPT_ERROR, "No valid selected objects");
      return false;
    }
  }
  else {
    if (BLI_listbase_is_empty(selected_objects)) {
      BKE_report(reports, RPT_ERROR, "No valid selected objects");
      return false;
    }

    for (link = selected_objects->first; link; link = link->next) {
      if (!bake_object_check(view_layer, link->ptr.data, target, reports)) {
        return false;
      }
    }
  }
  return true;
}

/* it needs to be called after bake_objects_check since the image tagging happens there */
static void bake_targets_clear(Main *bmain, const bool is_tangent)
{
  Image *image;
  for (image = bmain->images.first; image; image = image->id.next) {
    if ((image->id.tag & LIB_TAG_DOIT) != 0) {
      RE_bake_ibuf_clear(image, is_tangent);
    }
  }
}

/* create new mesh with edit mode changes and modifiers applied */
static Mesh *bake_mesh_new_from_object(Depsgraph *depsgraph,
                                       Object *object,
                                       const bool preserve_origindex)
{
  Mesh *me = BKE_mesh_new_from_object(depsgraph, object, false, preserve_origindex);

  if (me->flag & ME_AUTOSMOOTH) {
    BKE_mesh_split_faces(me, true);
  }

  return me;
}

/* Image Bake Targets */

static bool bake_targets_init_image_textures(const BakeAPIRender *bkr,
                                             BakeTargets *targets,
                                             Object *ob,
                                             ReportList *reports)
{
  int materials_num = ob->totcol;

  if (materials_num == 0) {
    if (bkr->save_mode == R_BAKE_SAVE_INTERNAL) {
      BKE_report(
          reports, RPT_ERROR, "No active image found, add a material or bake to an external file");
      return false;
    }
    if (bkr->is_split_materials) {
      BKE_report(
          reports,
          RPT_ERROR,
          "No active image found, add a material or bake without the Split Materials option");
      return false;
    }
  }

  /* Allocate material mapping. */
  targets->materials_num = materials_num;
  targets->material_to_image = MEM_callocN(sizeof(Image *) * targets->materials_num,
                                           "BakeTargets.material_to_image");

  /* Error handling and tag (in case multiple materials share the same image). */
  BKE_main_id_tag_idcode(bkr->main, ID_IM, LIB_TAG_DOIT, false);

  targets->images = NULL;

  for (int i = 0; i < materials_num; i++) {
    Image *image;
    ED_object_get_active_image(ob, i + 1, &image, NULL, NULL, NULL);

    targets->material_to_image[i] = image;

    /* Some materials have no image, we just ignore those cases.
     * Also setup each image only once. */
    if (image && !(image->id.tag & LIB_TAG_DOIT)) {
      LISTBASE_FOREACH (ImageTile *, tile, &image->tiles) {
        /* Add bake image. */
        targets->images = MEM_recallocN(targets->images,
                                        sizeof(BakeImage) * (targets->images_num + 1));
        targets->images[targets->images_num].image = image;
        targets->images[targets->images_num].tile_number = tile->tile_number;
        targets->images_num++;
      }

      image->id.tag |= LIB_TAG_DOIT;
    }
  }

  return true;
}

static bool bake_targets_init_internal(const BakeAPIRender *bkr,
                                       BakeTargets *targets,
                                       Object *ob,
                                       ReportList *reports)
{
  if (!bake_targets_init_image_textures(bkr, targets, ob, reports)) {
    return false;
  }

  /* Saving to image datablocks. */
  for (int i = 0; i < targets->images_num; i++) {
    BakeImage *bk_image = &targets->images[i];

    ImageUser iuser;
    BKE_imageuser_default(&iuser);
    iuser.tile = bk_image->tile_number;

    void *lock;
    ImBuf *ibuf = BKE_image_acquire_ibuf(bk_image->image, &iuser, &lock);

    if (ibuf) {
      bk_image->width = ibuf->x;
      bk_image->height = ibuf->y;
      bk_image->offset = targets->pixels_num;
      BKE_image_get_tile_uv(bk_image->image, bk_image->tile_number, bk_image->uv_offset);

      targets->pixels_num += (size_t)ibuf->x * (size_t)ibuf->y;
    }
    else {
      BKE_image_release_ibuf(bk_image->image, ibuf, lock);
      BKE_reportf(reports, RPT_ERROR, "Uninitialized image %s", bk_image->image->id.name + 2);
      return false;
    }
    BKE_image_release_ibuf(bk_image->image, ibuf, lock);
  }

  return true;
}

static bool bake_targets_output_internal(const BakeAPIRender *bkr,
                                         BakeTargets *targets,
                                         Object *ob,
                                         BakePixel *pixel_array,
                                         ReportList *reports,
                                         Mesh *mesh_eval)
{
  bool all_ok = true;

  for (int i = 0; i < targets->images_num; i++) {
    BakeImage *bk_image = &targets->images[i];
    const bool ok = write_internal_bake_pixels(bk_image->image,
                                               bk_image->tile_number,
                                               pixel_array + bk_image->offset,
                                               targets->result +
                                                   bk_image->offset * targets->channels_num,
                                               bk_image->width,
                                               bk_image->height,
                                               bkr->margin,
                                               bkr->margin_type,
                                               bkr->is_clear,
                                               targets->is_noncolor,
                                               mesh_eval,
                                               bkr->uv_layer,
                                               bk_image->uv_offset);

    /* might be read by UI to set active image for display */
    bake_update_image(bkr->area, bk_image->image);

    if (!ok) {
      BKE_reportf(reports,
                  RPT_ERROR,
                  "Problem saving the bake map internally for object \"%s\"",
                  ob->id.name + 2);
      all_ok = false;
    }
    else {
      BKE_report(
          reports, RPT_INFO, "Baking map saved to internal image, save it externally or pack it");
    }
  }

  return all_ok;
}

static bool bake_targets_init_external(const BakeAPIRender *bkr,
                                       BakeTargets *targets,
                                       Object *ob,
                                       ReportList *reports)
{
  if (!bake_targets_init_image_textures(bkr, targets, ob, reports)) {
    return false;
  }

  /* Saving to disk. */
  for (int i = 0; i < targets->images_num; i++) {
    BakeImage *bk_image = &targets->images[i];

    bk_image->width = bkr->width;
    bk_image->height = bkr->height;
    bk_image->offset = targets->pixels_num;

    targets->pixels_num += (size_t)bkr->width * (size_t)bkr->height;

    if (!bkr->is_split_materials) {
      break;
    }
  }

  if (!bkr->is_split_materials) {
    /* saving a single image */
    for (int i = 0; i < targets->materials_num; i++) {
      targets->material_to_image[i] = targets->images[0].image;
    }
  }

  return true;
}

static bool bake_targets_output_external(const BakeAPIRender *bkr,
                                         BakeTargets *targets,
                                         Object *ob,
                                         Object *ob_eval,
                                         Mesh *mesh_eval,
                                         BakePixel *pixel_array,
                                         ReportList *reports)
{
  bool all_ok = true;

  for (int i = 0; i < targets->images_num; i++) {
    BakeImage *bk_image = &targets->images[i];

    BakeData *bake = &bkr->scene->r.bake;
    char name[FILE_MAX];

    BKE_image_path_from_imtype(name,
                               bkr->filepath,
                               BKE_main_blendfile_path(bkr->main),
                               0,
                               bake->im_format.imtype,
                               true,
                               false,
                               NULL);

    if (bkr->is_automatic_name) {
      BLI_path_suffix(name, FILE_MAX, ob->id.name + 2, "_");
      BLI_path_suffix(name, FILE_MAX, bkr->identifier, "_");
    }

    if (bkr->is_split_materials) {
      if (ob_eval->mat[i]) {
        BLI_path_suffix(name, FILE_MAX, ob_eval->mat[i]->id.name + 2, "_");
      }
      else if (mesh_eval->mat[i]) {
        BLI_path_suffix(name, FILE_MAX, mesh_eval->mat[i]->id.name + 2, "_");
      }
      else {
        /* if everything else fails, use the material index */
        char tmp[5];
        sprintf(tmp, "%d", i % 1000);
        BLI_path_suffix(name, FILE_MAX, tmp, "_");
      }
    }

    if (bk_image->tile_number) {
      char tmp[FILE_MAX];
      SNPRINTF(tmp, "%d", bk_image->tile_number);
      BLI_path_suffix(name, FILE_MAX, tmp, "_");
    }

    /* save it externally */
    const bool ok = write_external_bake_pixels(name,
                                               pixel_array + bk_image->offset,
                                               targets->result +
                                                   bk_image->offset * targets->channels_num,
                                               bk_image->width,
                                               bk_image->height,
                                               bkr->margin,
                                               bkr->margin_type,
                                               &bake->im_format,
                                               targets->is_noncolor,
                                               mesh_eval,
                                               bkr->uv_layer,
                                               bk_image->uv_offset);

    if (!ok) {
      BKE_reportf(reports, RPT_ERROR, "Problem saving baked map in \"%s\"", name);
      all_ok = false;
    }
    else {
      BKE_reportf(reports, RPT_INFO, "Baking map written to \"%s\"", name);
    }

    if (!bkr->is_split_materials) {
      break;
    }
  }

  return all_ok;
}

/* Vertex Color Bake Targets */

static bool bake_targets_init_vertex_colors(Main *bmain,
                                            BakeTargets *targets,
                                            Object *ob,
                                            ReportList *reports)
{
  if (ob->type != OB_MESH) {
    BKE_report(reports, RPT_ERROR, "Color attribute baking is only supported for mesh objects");
    return false;
  }

  Mesh *me = ob->data;
  if (BKE_id_attributes_active_color_get(&me->id) == NULL) {
    BKE_report(reports, RPT_ERROR, "No active color attribute to bake to");
    return false;
  }

  /* Ensure mesh and editmesh topology are in sync. */
  ED_object_editmode_load(bmain, ob);

  targets->images = MEM_callocN(sizeof(BakeImage), "BakeTargets.images");
  targets->images_num = 1;

  targets->material_to_image = MEM_callocN(sizeof(int) * ob->totcol,
                                           "BakeTargets.material_to_image");
  targets->materials_num = ob->totcol;

  BakeImage *bk_image = &targets->images[0];
  bk_image->width = me->totloop;
  bk_image->height = 1;
  bk_image->offset = 0;
  bk_image->image = NULL;

  targets->pixels_num = bk_image->width * bk_image->height;

  return true;
}

static int find_original_loop(const Mesh *me_orig,
                              const int *vert_origindex,
                              const int *poly_origindex,
                              const int poly_eval,
                              const int vert_eval)
{
  /* Get original vertex and polygon index. There is currently no loop mapping
   * in modifier stack evaluation. */
  const int vert_orig = vert_origindex[vert_eval];
  const int poly_orig = poly_origindex[poly_eval];

  if (vert_orig == ORIGINDEX_NONE || poly_orig == ORIGINDEX_NONE) {
    return ORIGINDEX_NONE;
  }

  /* Find matching loop with original vertex in original polygon. */
  MPoly *mpoly_orig = me_orig->mpoly + poly_orig;
  MLoop *mloop_orig = me_orig->mloop + mpoly_orig->loopstart;
  for (int j = 0; j < mpoly_orig->totloop; ++j, ++mloop_orig) {
    if (mloop_orig->v == vert_orig) {
      return mpoly_orig->loopstart + j;
    }
  }

  return ORIGINDEX_NONE;
}

static void bake_targets_populate_pixels_color_attributes(BakeTargets *targets,
                                                          Object *ob,
                                                          Mesh *me_eval,
                                                          BakePixel *pixel_array)
{
  Mesh *me = ob->data;
  const int pixels_num = targets->pixels_num;

  /* Initialize blank pixels. */
  for (int i = 0; i < pixels_num; i++) {
    BakePixel *pixel = &pixel_array[i];

    pixel->primitive_id = -1;
    pixel->object_id = 0;
    pixel->seed = 0;
    pixel->du_dx = 0.0f;
    pixel->du_dy = 0.0f;
    pixel->dv_dx = 0.0f;
    pixel->dv_dy = 0.0f;
    pixel->uv[0] = 0.0f;
    pixel->uv[1] = 0.0f;
  }

  /* Populate through adjacent triangles, first triangle wins. */
  const int tottri = poly_to_tri_count(me_eval->totpoly, me_eval->totloop);
  MLoopTri *looptri = MEM_mallocN(sizeof(*looptri) * tottri, __func__);

  BKE_mesh_recalc_looptri(
      me_eval->mloop, me_eval->mpoly, me_eval->mvert, me_eval->totloop, me_eval->totpoly, looptri);

  /* For mapping back to original mesh in case there are modifiers. */
  const int *vert_origindex = CustomData_get_layer(&me_eval->vdata, CD_ORIGINDEX);
  const int *poly_origindex = CustomData_get_layer(&me_eval->pdata, CD_ORIGINDEX);

  for (int i = 0; i < tottri; i++) {
    const MLoopTri *lt = &looptri[i];

    for (int j = 0; j < 3; j++) {
      unsigned int l = lt->tri[j];
      unsigned int v = me_eval->mloop[l].v;

      /* Map back to original loop if there are modifiers. */
      if (vert_origindex != NULL && poly_origindex != NULL) {
        l = find_original_loop(me, vert_origindex, poly_origindex, lt->poly, v);
        if (l == ORIGINDEX_NONE || l >= me->totloop) {
          continue;
        }
      }

      BakePixel *pixel = &pixel_array[l];

      if (pixel->primitive_id != -1) {
        continue;
      }

      pixel->primitive_id = i;

      /* Seed is the vertex, so that sampling noise is coherent for the same
       * vertex, but different corners can still have different normals,
       * materials and UVs. */
      pixel->seed = v;

      /* Barycentric coordinates. */
      if (j == 0) {
        pixel->uv[0] = 1.0f;
        pixel->uv[1] = 0.0f;
      }
      else if (j == 1) {
        pixel->uv[0] = 0.0f;
        pixel->uv[1] = 1.0f;
      }
      else if (j == 2) {
        pixel->uv[0] = 0.0f;
        pixel->uv[1] = 0.0f;
      }
    }
  }

  MEM_freeN(looptri);
}

static void bake_result_add_to_rgba(float rgba[4], const float *result, const int channels_num)
{
  if (channels_num == 4) {
    add_v4_v4(rgba, result);
  }
  else if (channels_num == 3) {
    add_v3_v3(rgba, result);
    rgba[3] += 1.0f;
  }
  else {
    rgba[0] += result[0];
    rgba[1] += result[0];
    rgba[2] += result[0];
    rgba[3] += 1.0f;
  }
}

static void convert_float_color_to_byte_color(const MPropCol *float_colors,
                                              const int num,
                                              const bool is_noncolor,
                                              MLoopCol *byte_colors)
{
  if (is_noncolor) {
    for (int i = 0; i < num; i++) {
      unit_float_to_uchar_clamp_v4(&byte_colors->r, float_colors[i].color);
    }
  }
  else {
    for (int i = 0; i < num; i++) {
      linearrgb_to_srgb_uchar4(&byte_colors[i].r, float_colors[i].color);
    }
  }
}

static bool bake_targets_output_vertex_colors(BakeTargets *targets, Object *ob)
{
  Mesh *me = ob->data;
  BMEditMesh *em = me->edit_mesh;
  CustomDataLayer *active_color_layer = BKE_id_attributes_active_color_get(&me->id);
  BLI_assert(active_color_layer != NULL);
  const eAttrDomain domain = BKE_id_attribute_domain(&me->id, active_color_layer);

  const int channels_num = targets->channels_num;
  const bool is_noncolor = targets->is_noncolor;
  const float *result = targets->result;

  if (domain == ATTR_DOMAIN_POINT) {
    const int totvert = me->totvert;
    const int totloop = me->totloop;

    MPropCol *mcol = MEM_malloc_arrayN(totvert, sizeof(MPropCol), __func__);

    /* Accumulate float vertex colors in scene linear color space. */
    int *num_loops_for_vertex = MEM_callocN(sizeof(int) * me->totvert, "num_loops_for_vertex");
    memset(mcol, 0, sizeof(MPropCol) * me->totvert);

    MLoop *mloop = me->mloop;
    for (int i = 0; i < totloop; i++, mloop++) {
      const int v = mloop->v;
      bake_result_add_to_rgba(mcol[v].color, &result[i * channels_num], channels_num);
      num_loops_for_vertex[v]++;
    }

    /* Normalize for number of loops. */
    for (int i = 0; i < totvert; i++) {
      if (num_loops_for_vertex[i] > 0) {
        mul_v4_fl(mcol[i].color, 1.0f / num_loops_for_vertex[i]);
      }
    }

    if (em) {
      /* Copy to bmesh. */
      const int active_color_offset = CustomData_get_offset_named(
          &em->bm->vdata, active_color_layer->type, active_color_layer->name);
      BMVert *v;
      BMIter viter;
      int i = 0;
      BM_ITER_MESH (v, &viter, em->bm, BM_VERTS_OF_MESH) {
        void *data = BM_ELEM_CD_GET_VOID_P(v, active_color_offset);
        if (active_color_layer->type == CD_PROP_COLOR) {
          memcpy(data, &mcol[i], sizeof(MPropCol));
        }
        else {
          convert_float_color_to_byte_color(&mcol[i], 1, is_noncolor, data);
        }
        i++;
      }
    }
    else {
      /* Copy to mesh. */
      if (active_color_layer->type == CD_PROP_COLOR) {
        memcpy(active_color_layer->data, mcol, sizeof(MPropCol) * me->totvert);
      }
      else {
        convert_float_color_to_byte_color(mcol, totvert, is_noncolor, active_color_layer->data);
      }
    }

    MEM_freeN(mcol);

    MEM_SAFE_FREE(num_loops_for_vertex);
  }
  else if (domain == ATTR_DOMAIN_CORNER) {
    if (em) {
      /* Copy to bmesh. */
      const int active_color_offset = CustomData_get_offset_named(
          &em->bm->ldata, active_color_layer->type, active_color_layer->name);
      BMFace *f;
      BMIter fiter;
      int i = 0;
      BM_ITER_MESH (f, &fiter, em->bm, BM_FACES_OF_MESH) {
        BMLoop *l;
        BMIter liter;
        BM_ITER_ELEM (l, &liter, f, BM_LOOPS_OF_FACE) {
          MPropCol color;
          zero_v4(color.color);
          bake_result_add_to_rgba(color.color, &result[i * channels_num], channels_num);
          i++;

          void *data = BM_ELEM_CD_GET_VOID_P(l, active_color_offset);
          if (active_color_layer->type == CD_PROP_COLOR) {
            memcpy(data, &color, sizeof(MPropCol));
          }
          else {
            convert_float_color_to_byte_color(&color, 1, is_noncolor, data);
          }
        }
      }
    }
    else {
      /* Copy to mesh. */
      if (active_color_layer->type == CD_PROP_COLOR) {
        MPropCol *colors = active_color_layer->data;
        for (int i = 0; i < me->totloop; i++) {
          zero_v4(colors[i].color);
          bake_result_add_to_rgba(colors[i].color, &result[i * channels_num], channels_num);
        }
      }
      else {
        MLoopCol *colors = active_color_layer->data;
        for (int i = 0; i < me->totloop; i++) {
          MPropCol color;
          zero_v4(color.color);
          bake_result_add_to_rgba(color.color, &result[i * channels_num], channels_num);
          convert_float_color_to_byte_color(&color, 1, is_noncolor, &colors[i]);
        }
      }
    }
  }

  DEG_id_tag_update(&me->id, ID_RECALC_GEOMETRY);

  return true;
}

/* Bake Targets */

static bool bake_targets_init(const BakeAPIRender *bkr,
                              BakeTargets *targets,
                              Object *ob,
                              Object *ob_eval,
                              ReportList *reports)
{
  if (bkr->target == R_BAKE_TARGET_IMAGE_TEXTURES) {
    if (bkr->save_mode == R_BAKE_SAVE_INTERNAL) {
      if (!bake_targets_init_internal(bkr, targets, ob_eval, reports)) {
        return false;
      }
    }
    else if (bkr->save_mode == R_BAKE_SAVE_EXTERNAL) {
      if (!bake_targets_init_external(bkr, targets, ob_eval, reports)) {
        return false;
      }
    }
  }
  else if (bkr->target == R_BAKE_TARGET_VERTEX_COLORS) {
    if (!bake_targets_init_vertex_colors(bkr->main, targets, ob, reports)) {
      return false;
    }
  }

  if (targets->pixels_num == 0) {
    return false;
  }

  targets->is_noncolor = is_noncolor_pass(bkr->pass_type);
  targets->channels_num = RE_pass_depth(bkr->pass_type);
  targets->result = MEM_callocN(sizeof(float) * targets->channels_num * targets->pixels_num,
                                "bake return pixels");

  return true;
}

static void bake_targets_populate_pixels(const BakeAPIRender *bkr,
                                         BakeTargets *targets,
                                         Object *ob,
                                         Mesh *me_eval,
                                         BakePixel *pixel_array)
{
  if (bkr->target == R_BAKE_TARGET_VERTEX_COLORS) {
    bake_targets_populate_pixels_color_attributes(targets, ob, me_eval, pixel_array);
  }
  else {
    RE_bake_pixels_populate(me_eval, pixel_array, targets->pixels_num, targets, bkr->uv_layer);
  }
}

static bool bake_targets_output(const BakeAPIRender *bkr,
                                BakeTargets *targets,
                                Object *ob,
                                Object *ob_eval,
                                Mesh *me_eval,
                                BakePixel *pixel_array,
                                ReportList *reports)
{
  if (bkr->target == R_BAKE_TARGET_IMAGE_TEXTURES) {
    if (bkr->save_mode == R_BAKE_SAVE_INTERNAL) {
      return bake_targets_output_internal(bkr, targets, ob, pixel_array, reports, me_eval);
    }
    if (bkr->save_mode == R_BAKE_SAVE_EXTERNAL) {
      return bake_targets_output_external(
          bkr, targets, ob, ob_eval, me_eval, pixel_array, reports);
    }
  }
  else if (bkr->target == R_BAKE_TARGET_VERTEX_COLORS) {
    return bake_targets_output_vertex_colors(targets, ob);
  }

  return false;
}

static void bake_targets_free(BakeTargets *targets)
{
  MEM_SAFE_FREE(targets->images);
  MEM_SAFE_FREE(targets->material_to_image);
  MEM_SAFE_FREE(targets->result);
}

/* Main Bake Logic */

static int bake(const BakeAPIRender *bkr,
                Object *ob_low,
                const ListBase *selected_objects,
                ReportList *reports)
{
  Render *re = bkr->render;
  Main *bmain = bkr->main;
  Scene *scene = bkr->scene;
  ViewLayer *view_layer = bkr->view_layer;

  /* We build a depsgraph for the baking,
   * so we don't need to change the original data to adjust visibility and modifiers. */
  Depsgraph *depsgraph = DEG_graph_new(bmain, scene, view_layer, DAG_EVAL_RENDER);
  DEG_graph_build_from_view_layer(depsgraph);

  int op_result = OPERATOR_CANCELLED;
  bool ok = false;

  Object *ob_cage = NULL;
  Object *ob_cage_eval = NULL;
  Object *ob_low_eval = NULL;

  BakeHighPolyData *highpoly = NULL;
  int tot_highpoly = 0;

  Mesh *me_low_eval = NULL;
  Mesh *me_cage_eval = NULL;

  MultiresModifierData *mmd_low = NULL;
  int mmd_flags_low = 0;

  BakePixel *pixel_array_low = NULL;
  BakePixel *pixel_array_high = NULL;

  BakeTargets targets = {NULL};

  const bool preserve_origindex = (bkr->target == R_BAKE_TARGET_VERTEX_COLORS);

  RE_bake_engine_set_engine_parameters(re, bmain, scene);

  if (!RE_bake_has_engine(re)) {
    BKE_report(reports, RPT_ERROR, "Current render engine does not support baking");
    goto cleanup;
  }

  if (bkr->uv_layer[0] != '\0') {
    Mesh *me = (Mesh *)ob_low->data;
    if (CustomData_get_named_layer(&me->ldata, CD_MLOOPUV, bkr->uv_layer) == -1) {
      BKE_reportf(reports,
                  RPT_ERROR,
                  "No UV layer named \"%s\" found in the object \"%s\"",
                  bkr->uv_layer,
                  ob_low->id.name + 2);
      goto cleanup;
    }
  }

  if (bkr->is_selected_to_active) {
    CollectionPointerLink *link;
    tot_highpoly = 0;

    for (link = selected_objects->first; link; link = link->next) {
      Object *ob_iter = link->ptr.data;

      if (ob_iter == ob_low) {
        continue;
      }

      tot_highpoly++;
    }

    if (bkr->is_cage && bkr->custom_cage[0] != '\0') {
      ob_cage = BLI_findstring(&bmain->objects, bkr->custom_cage, offsetof(ID, name) + 2);

      if (ob_cage == NULL || ob_cage->type != OB_MESH) {
        BKE_report(reports, RPT_ERROR, "No valid cage object");
        goto cleanup;
      }
      else {
        ob_cage_eval = DEG_get_evaluated_object(depsgraph, ob_cage);
        ob_cage_eval->visibility_flag |= OB_HIDE_RENDER;
        ob_cage_eval->base_flag &= ~(BASE_VISIBLE_DEPSGRAPH | BASE_ENABLED_RENDER);
      }
    }
  }

  /* for multires bake, use linear UV subdivision to match low res UVs */
  if (bkr->pass_type == SCE_PASS_NORMAL && bkr->normal_space == R_BAKE_SPACE_TANGENT &&
      !bkr->is_selected_to_active) {
    mmd_low = (MultiresModifierData *)BKE_modifiers_findby_type(ob_low, eModifierType_Multires);
    if (mmd_low) {
      mmd_flags_low = mmd_low->flags;
      mmd_low->uv_smooth = SUBSURF_UV_SMOOTH_NONE;
    }
  }

  /* Make sure depsgraph is up to date. */
  BKE_scene_graph_update_tagged(depsgraph, bmain);
  ob_low_eval = DEG_get_evaluated_object(depsgraph, ob_low);

  /* get the mesh as it arrives in the renderer */
  me_low_eval = bake_mesh_new_from_object(depsgraph, ob_low_eval, preserve_origindex);

  /* Initialize bake targets. */
  if (!bake_targets_init(bkr, &targets, ob_low, ob_low_eval, reports)) {
    goto cleanup;
  }

  /* Populate the pixel array with the face data. Except if we use a cage, then
   * it is populated later with the cage mesh (smoothed version of the mesh). */
  pixel_array_low = MEM_mallocN(sizeof(BakePixel) * targets.pixels_num, "bake pixels low poly");
  if ((bkr->is_selected_to_active && (ob_cage == NULL) && bkr->is_cage) == false) {
    bake_targets_populate_pixels(bkr, &targets, ob_low, me_low_eval, pixel_array_low);
  }

  if (bkr->is_selected_to_active) {
    CollectionPointerLink *link;
    int i = 0;

    /* prepare cage mesh */
    if (ob_cage) {
      me_cage_eval = bake_mesh_new_from_object(depsgraph, ob_cage_eval, preserve_origindex);
      if ((me_low_eval->totpoly != me_cage_eval->totpoly) ||
          (me_low_eval->totloop != me_cage_eval->totloop)) {
        BKE_report(reports,
                   RPT_ERROR,
                   "Invalid cage object, the cage mesh must have the same number "
                   "of faces as the active object");
        goto cleanup;
      }
    }
    else if (bkr->is_cage) {
      bool is_changed = false;

      ModifierData *md = ob_low_eval->modifiers.first;
      while (md) {
        ModifierData *md_next = md->next;

        /* Edge Split cannot be applied in the cage,
         * the cage is supposed to have interpolated normals
         * between the faces unless the geometry is physically
         * split. So we create a copy of the low poly mesh without
         * the eventual edge split. */

        if (md->type == eModifierType_EdgeSplit) {
          BLI_remlink(&ob_low_eval->modifiers, md);
          BKE_modifier_free(md);
          is_changed = true;
        }
        md = md_next;
      }

      if (is_changed) {
        /* Make sure object is evaluated with the new modifier settings.
         *
         * NOTE: Since the dependency graph was fully evaluated prior to bake, and we only made
         * single modification to this object all the possible dependencies for evaluation are
         * already up to date. This means we can do a cheap single object update
         * (as an opposite of full depsgraph update). */
        BKE_object_eval_reset(ob_low_eval);
        BKE_object_handle_data_update(depsgraph, scene, ob_low_eval);
      }

      me_cage_eval = BKE_mesh_new_from_object(NULL, ob_low_eval, false, preserve_origindex);
      bake_targets_populate_pixels(bkr, &targets, ob_low, me_cage_eval, pixel_array_low);
    }

    highpoly = MEM_callocN(sizeof(BakeHighPolyData) * tot_highpoly, "bake high poly objects");

    /* populate highpoly array */
    for (link = selected_objects->first; link; link = link->next) {
      Object *ob_iter = link->ptr.data;

      if (ob_iter == ob_low) {
        continue;
      }

      /* initialize highpoly_data */
      highpoly[i].ob = ob_iter;
      highpoly[i].ob_eval = DEG_get_evaluated_object(depsgraph, ob_iter);
      highpoly[i].ob_eval->visibility_flag &= ~OB_HIDE_RENDER;
      highpoly[i].ob_eval->base_flag |= (BASE_VISIBLE_DEPSGRAPH | BASE_ENABLED_RENDER);
      highpoly[i].me = BKE_mesh_new_from_object(NULL, highpoly[i].ob_eval, false, false);

      /* Low-poly to high-poly transformation matrix. */
      copy_m4_m4(highpoly[i].obmat, highpoly[i].ob->obmat);
      invert_m4_m4(highpoly[i].imat, highpoly[i].obmat);

      highpoly[i].is_flip_object = is_negative_m4(highpoly[i].ob->obmat);

      i++;
    }

    BLI_assert(i == tot_highpoly);

    if (ob_cage != NULL) {
      ob_cage_eval->visibility_flag |= OB_HIDE_RENDER;
      ob_cage_eval->base_flag &= ~(BASE_VISIBLE_DEPSGRAPH | BASE_ENABLED_RENDER);
    }
    ob_low_eval->visibility_flag |= OB_HIDE_RENDER;
    ob_low_eval->base_flag &= ~(BASE_VISIBLE_DEPSGRAPH | BASE_ENABLED_RENDER);

    /* populate the pixel arrays with the corresponding face data for each high poly object */
    pixel_array_high = MEM_mallocN(sizeof(BakePixel) * targets.pixels_num,
                                   "bake pixels high poly");

    if (!RE_bake_pixels_populate_from_objects(me_low_eval,
                                              pixel_array_low,
                                              pixel_array_high,
                                              highpoly,
                                              tot_highpoly,
                                              targets.pixels_num,
                                              ob_cage != NULL,
                                              bkr->cage_extrusion,
                                              bkr->max_ray_distance,
                                              ob_low_eval->obmat,
                                              (ob_cage ? ob_cage->obmat : ob_low_eval->obmat),
                                              me_cage_eval)) {
      BKE_report(reports, RPT_ERROR, "Error handling selected objects");
      goto cleanup;
    }

    /* the baking itself */
    for (i = 0; i < tot_highpoly; i++) {
      ok = RE_bake_engine(re,
                          depsgraph,
                          highpoly[i].ob,
                          i,
                          pixel_array_high,
                          &targets,
                          bkr->pass_type,
                          bkr->pass_filter,
                          targets.result);
      if (!ok) {
        BKE_reportf(
            reports, RPT_ERROR, "Error baking from object \"%s\"", highpoly[i].ob->id.name + 2);
        goto cleanup;
      }
    }
  }
  else {
    /* If low poly is not renderable it should have failed long ago. */
    BLI_assert((ob_low_eval->visibility_flag & OB_HIDE_RENDER) == 0);

    if (RE_bake_has_engine(re)) {
      ok = RE_bake_engine(re,
                          depsgraph,
                          ob_low_eval,
                          0,
                          pixel_array_low,
                          &targets,
                          bkr->pass_type,
                          bkr->pass_filter,
                          targets.result);
    }
    else {
      BKE_report(reports, RPT_ERROR, "Current render engine does not support baking");
      goto cleanup;
    }
  }

  /* normal space conversion
   * the normals are expected to be in world space, +X +Y +Z */
  if (ok && bkr->pass_type == SCE_PASS_NORMAL) {
    switch (bkr->normal_space) {
      case R_BAKE_SPACE_WORLD: {
        /* Cycles internal format */
        if ((bkr->normal_swizzle[0] == R_BAKE_POSX) && (bkr->normal_swizzle[1] == R_BAKE_POSY) &&
            (bkr->normal_swizzle[2] == R_BAKE_POSZ)) {
          break;
        }
        RE_bake_normal_world_to_world(pixel_array_low,
                                      targets.pixels_num,
                                      targets.channels_num,
                                      targets.result,
                                      bkr->normal_swizzle);
        break;
      }
      case R_BAKE_SPACE_OBJECT: {
        RE_bake_normal_world_to_object(pixel_array_low,
                                       targets.pixels_num,
                                       targets.channels_num,
                                       targets.result,
                                       ob_low_eval,
                                       bkr->normal_swizzle);
        break;
      }
      case R_BAKE_SPACE_TANGENT: {
        if (bkr->is_selected_to_active) {
          RE_bake_normal_world_to_tangent(pixel_array_low,
                                          targets.pixels_num,
                                          targets.channels_num,
                                          targets.result,
                                          me_low_eval,
                                          bkr->normal_swizzle,
                                          ob_low_eval->obmat);
        }
        else {
          /* From multi-resolution. */
          Mesh *me_nores = NULL;
          ModifierData *md = NULL;
          int mode;

          BKE_object_eval_reset(ob_low_eval);
          md = BKE_modifiers_findby_type(ob_low_eval, eModifierType_Multires);

          if (md) {
            mode = md->mode;
            md->mode &= ~eModifierMode_Render;

            /* Evaluate modifiers again. */
            me_nores = BKE_mesh_new_from_object(NULL, ob_low_eval, false, false);
            bake_targets_populate_pixels(bkr, &targets, ob_low, me_nores, pixel_array_low);
          }

          RE_bake_normal_world_to_tangent(pixel_array_low,
                                          targets.pixels_num,
                                          targets.channels_num,
                                          targets.result,
                                          (me_nores) ? me_nores : me_low_eval,
                                          bkr->normal_swizzle,
                                          ob_low_eval->obmat);

          if (md) {
            BKE_id_free(NULL, &me_nores->id);
            md->mode = mode;
          }
        }
        break;
      }
      default:
        break;
    }
  }

  if (!ok) {
    BKE_reportf(reports, RPT_ERROR, "Problem baking object \"%s\"", ob_low->id.name + 2);
    op_result = OPERATOR_CANCELLED;
  }
  else {
    /* save the results */
    if (bake_targets_output(
            bkr, &targets, ob_low, ob_low_eval, me_low_eval, pixel_array_low, reports)) {
      op_result = OPERATOR_FINISHED;
    }
    else {
      op_result = OPERATOR_CANCELLED;
    }
  }

  bake_targets_refresh(&targets);

cleanup:

  if (highpoly) {
    for (int i = 0; i < tot_highpoly; i++) {
      if (highpoly[i].me != NULL) {
        BKE_id_free(NULL, &highpoly[i].me->id);
      }
    }
    MEM_freeN(highpoly);
  }

  if (mmd_low) {
    mmd_low->flags = mmd_flags_low;
  }

  if (pixel_array_low) {
    MEM_freeN(pixel_array_low);
  }

  if (pixel_array_high) {
    MEM_freeN(pixel_array_high);
  }

  bake_targets_free(&targets);

  if (me_low_eval != NULL) {
    BKE_id_free(NULL, &me_low_eval->id);
  }

  if (me_cage_eval != NULL) {
    BKE_id_free(NULL, &me_cage_eval->id);
  }

  DEG_graph_free(depsgraph);

  return op_result;
}

/* Bake Operator */

static void bake_init_api_data(wmOperator *op, bContext *C, BakeAPIRender *bkr)
{
  bScreen *screen = CTX_wm_screen(C);

  bkr->ob = CTX_data_active_object(C);
  bkr->main = CTX_data_main(C);
  bkr->view_layer = CTX_data_view_layer(C);
  bkr->scene = CTX_data_scene(C);
  bkr->area = screen ? BKE_screen_find_big_area(screen, SPACE_IMAGE, 10) : NULL;

  bkr->pass_type = RNA_enum_get(op->ptr, "type");
  bkr->pass_filter = RNA_enum_get(op->ptr, "pass_filter");
  bkr->margin = RNA_int_get(op->ptr, "margin");
  bkr->margin_type = RNA_enum_get(op->ptr, "margin_type");

  bkr->save_mode = (eBakeSaveMode)RNA_enum_get(op->ptr, "save_mode");
  bkr->target = (eBakeTarget)RNA_enum_get(op->ptr, "target");

  bkr->is_clear = RNA_boolean_get(op->ptr, "use_clear");
  bkr->is_split_materials = (bkr->target == R_BAKE_TARGET_IMAGE_TEXTURES &&
                             bkr->save_mode == R_BAKE_SAVE_EXTERNAL) &&
                            RNA_boolean_get(op->ptr, "use_split_materials");
  bkr->is_automatic_name = RNA_boolean_get(op->ptr, "use_automatic_name");
  bkr->is_selected_to_active = RNA_boolean_get(op->ptr, "use_selected_to_active");
  bkr->is_cage = RNA_boolean_get(op->ptr, "use_cage");
  bkr->cage_extrusion = RNA_float_get(op->ptr, "cage_extrusion");
  bkr->max_ray_distance = RNA_float_get(op->ptr, "max_ray_distance");

  bkr->normal_space = RNA_enum_get(op->ptr, "normal_space");
  bkr->normal_swizzle[0] = RNA_enum_get(op->ptr, "normal_r");
  bkr->normal_swizzle[1] = RNA_enum_get(op->ptr, "normal_g");
  bkr->normal_swizzle[2] = RNA_enum_get(op->ptr, "normal_b");

  bkr->width = RNA_int_get(op->ptr, "width");
  bkr->height = RNA_int_get(op->ptr, "height");
  bkr->identifier = "";

  RNA_string_get(op->ptr, "uv_layer", bkr->uv_layer);

  RNA_string_get(op->ptr, "cage_object", bkr->custom_cage);

  if (bkr->save_mode == R_BAKE_SAVE_EXTERNAL && bkr->is_automatic_name) {
    PropertyRNA *prop = RNA_struct_find_property(op->ptr, "type");
    RNA_property_enum_identifier(C, op->ptr, prop, bkr->pass_type, &bkr->identifier);
  }

  CTX_data_selected_objects(C, &bkr->selected_objects);

  bkr->reports = op->reports;

  bkr->result = OPERATOR_CANCELLED;

  bkr->render = RE_NewSceneRender(bkr->scene);

  /* XXX hack to force saving to always be internal. Whether (and how) to support
   * external saving will be addressed later */
  if (bkr->save_mode == R_BAKE_SAVE_EXTERNAL) {
    bkr->save_mode = R_BAKE_SAVE_INTERNAL;
  }

  if (((bkr->pass_type == SCE_PASS_NORMAL) && (bkr->normal_space == R_BAKE_SPACE_TANGENT)) ||
      bkr->pass_type == SCE_PASS_UV) {
    bkr->margin_type = R_BAKE_EXTEND;
  }
}

static int bake_exec(bContext *C, wmOperator *op)
{
  Render *re;
  int result = OPERATOR_CANCELLED;
  BakeAPIRender bkr = {NULL};
  Scene *scene = CTX_data_scene(C);

  G.is_break = false;
  G.is_rendering = true;

  bake_set_props(op, scene);

  bake_init_api_data(op, C, &bkr);
  re = bkr.render;

  /* setup new render */
  RE_test_break_cb(re, NULL, bake_break);

  if (!bake_pass_filter_check(bkr.pass_type, bkr.pass_filter, bkr.reports)) {
    goto finally;
  }

  if (!bake_objects_check(bkr.main,
                          bkr.view_layer,
                          bkr.ob,
                          &bkr.selected_objects,
                          bkr.reports,
                          bkr.is_selected_to_active,
                          bkr.target)) {
    goto finally;
  }

  if (bkr.is_clear) {
    const bool is_tangent = ((bkr.pass_type == SCE_PASS_NORMAL) &&
                             (bkr.normal_space == R_BAKE_SPACE_TANGENT));
    bake_targets_clear(bkr.main, is_tangent);
  }

  RE_SetReports(re, bkr.reports);

  if (bkr.is_selected_to_active) {
    result = bake(&bkr, bkr.ob, &bkr.selected_objects, bkr.reports);
  }
  else {
    CollectionPointerLink *link;
    bkr.is_clear = bkr.is_clear && BLI_listbase_is_single(&bkr.selected_objects);
    for (link = bkr.selected_objects.first; link; link = link->next) {
      Object *ob_iter = link->ptr.data;
      result = bake(&bkr, ob_iter, NULL, bkr.reports);
    }
  }

  RE_SetReports(re, NULL);

finally:
  G.is_rendering = false;
  BLI_freelistN(&bkr.selected_objects);
  return result;
}

static void bake_startjob(void *bkv, short *UNUSED(stop), short *do_update, float *progress)
{
  BakeAPIRender *bkr = (BakeAPIRender *)bkv;

  /* setup new render */
  bkr->do_update = do_update;
  bkr->progress = progress;

  RE_SetReports(bkr->render, bkr->reports);

  if (!bake_pass_filter_check(bkr->pass_type, bkr->pass_filter, bkr->reports)) {
    bkr->result = OPERATOR_CANCELLED;
    return;
  }

  if (!bake_objects_check(bkr->main,
                          bkr->view_layer,
                          bkr->ob,
                          &bkr->selected_objects,
                          bkr->reports,
                          bkr->is_selected_to_active,
                          bkr->target)) {
    bkr->result = OPERATOR_CANCELLED;
    return;
  }

  if (bkr->is_clear) {
    const bool is_tangent = ((bkr->pass_type == SCE_PASS_NORMAL) &&
                             (bkr->normal_space == R_BAKE_SPACE_TANGENT));
    bake_targets_clear(bkr->main, is_tangent);
  }

  if (bkr->is_selected_to_active) {
    bkr->result = bake(bkr, bkr->ob, &bkr->selected_objects, bkr->reports);
  }
  else {
    CollectionPointerLink *link;
    bkr->is_clear = bkr->is_clear && BLI_listbase_is_single(&bkr->selected_objects);
    for (link = bkr->selected_objects.first; link; link = link->next) {
      Object *ob_iter = link->ptr.data;
      bkr->result = bake(bkr, ob_iter, NULL, bkr->reports);

      if (bkr->result == OPERATOR_CANCELLED) {
        return;
      }
    }
  }

  RE_SetReports(bkr->render, NULL);
}

static void bake_job_complete(void *bkv)
{
  BakeAPIRender *bkr = (BakeAPIRender *)bkv;
  BKE_callback_exec_id(bkr->main, &bkr->ob->id, BKE_CB_EVT_OBJECT_BAKE_COMPLETE);
}
static void bake_job_canceled(void *bkv)
{
  BakeAPIRender *bkr = (BakeAPIRender *)bkv;
  BKE_callback_exec_id(bkr->main, &bkr->ob->id, BKE_CB_EVT_OBJECT_BAKE_CANCEL);
}

static void bake_freejob(void *bkv)
{
  BakeAPIRender *bkr = (BakeAPIRender *)bkv;

  BLI_freelistN(&bkr->selected_objects);
  MEM_freeN(bkr);

  G.is_rendering = false;
}

static void bake_set_props(wmOperator *op, Scene *scene)
{
  PropertyRNA *prop;
  BakeData *bake = &scene->r.bake;

  prop = RNA_struct_find_property(op->ptr, "filepath");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_string_set(op->ptr, prop, bake->filepath);
  }

  prop = RNA_struct_find_property(op->ptr, "width");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_int_set(op->ptr, prop, bake->width);
  }

  prop = RNA_struct_find_property(op->ptr, "height");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_int_set(op->ptr, prop, bake->width);
  }

  prop = RNA_struct_find_property(op->ptr, "margin");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_int_set(op->ptr, prop, bake->margin);
  }

  prop = RNA_struct_find_property(op->ptr, "margin_type");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_enum_set(op->ptr, prop, bake->margin_type);
  }

  prop = RNA_struct_find_property(op->ptr, "use_selected_to_active");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_boolean_set(op->ptr, prop, (bake->flag & R_BAKE_TO_ACTIVE) != 0);
  }

  prop = RNA_struct_find_property(op->ptr, "max_ray_distance");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_float_set(op->ptr, prop, bake->max_ray_distance);
  }

  prop = RNA_struct_find_property(op->ptr, "cage_extrusion");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_float_set(op->ptr, prop, bake->cage_extrusion);
  }

  prop = RNA_struct_find_property(op->ptr, "cage_object");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_string_set(
        op->ptr, prop, (bake->cage_object) ? bake->cage_object->id.name + 2 : "");
  }

  prop = RNA_struct_find_property(op->ptr, "normal_space");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_enum_set(op->ptr, prop, bake->normal_space);
  }

  prop = RNA_struct_find_property(op->ptr, "normal_r");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_enum_set(op->ptr, prop, bake->normal_swizzle[0]);
  }

  prop = RNA_struct_find_property(op->ptr, "normal_g");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_enum_set(op->ptr, prop, bake->normal_swizzle[1]);
  }

  prop = RNA_struct_find_property(op->ptr, "normal_b");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_enum_set(op->ptr, prop, bake->normal_swizzle[2]);
  }

  prop = RNA_struct_find_property(op->ptr, "target");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_enum_set(op->ptr, prop, bake->target);
  }

  prop = RNA_struct_find_property(op->ptr, "save_mode");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_enum_set(op->ptr, prop, bake->save_mode);
  }

  prop = RNA_struct_find_property(op->ptr, "use_clear");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_boolean_set(op->ptr, prop, (bake->flag & R_BAKE_CLEAR) != 0);
  }

  prop = RNA_struct_find_property(op->ptr, "use_cage");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_boolean_set(op->ptr, prop, (bake->flag & R_BAKE_CAGE) != 0);
  }

  prop = RNA_struct_find_property(op->ptr, "use_split_materials");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_boolean_set(op->ptr, prop, (bake->flag & R_BAKE_SPLIT_MAT) != 0);
  }

  prop = RNA_struct_find_property(op->ptr, "use_automatic_name");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_boolean_set(op->ptr, prop, (bake->flag & R_BAKE_AUTO_NAME) != 0);
  }

  prop = RNA_struct_find_property(op->ptr, "pass_filter");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_enum_set(op->ptr, prop, bake->pass_filter);
  }
}

static int bake_invoke(bContext *C, wmOperator *op, const wmEvent *UNUSED(event))
{
  wmJob *wm_job;
  BakeAPIRender *bkr;
  Render *re;
  Scene *scene = CTX_data_scene(C);

  bake_set_props(op, scene);

  /* only one render job at a time */
  if (WM_jobs_test(CTX_wm_manager(C), scene, WM_JOB_TYPE_OBJECT_BAKE)) {
    return OPERATOR_CANCELLED;
  }

  bkr = MEM_mallocN(sizeof(BakeAPIRender), "render bake");

  /* init bake render */
  bake_init_api_data(op, C, bkr);
  BKE_callback_exec_id(CTX_data_main(C), &bkr->ob->id, BKE_CB_EVT_OBJECT_BAKE_PRE);
  re = bkr->render;

  /* setup new render */
  RE_test_break_cb(re, NULL, bake_break);
  RE_progress_cb(re, bkr, bake_progress_update);

  /* setup job */
  wm_job = WM_jobs_get(CTX_wm_manager(C),
                       CTX_wm_window(C),
                       scene,
                       "Texture Bake",
                       WM_JOB_EXCL_RENDER | WM_JOB_PRIORITY | WM_JOB_PROGRESS,
                       WM_JOB_TYPE_OBJECT_BAKE);
  WM_jobs_customdata_set(wm_job, bkr, bake_freejob);
  /* TODO: only draw bake image, can we enforce this. */
  WM_jobs_timer(
      wm_job, 0.5, (bkr->target == R_BAKE_TARGET_VERTEX_COLORS) ? NC_GEOM | ND_DATA : NC_IMAGE, 0);
  WM_jobs_callbacks_ex(
      wm_job, bake_startjob, NULL, NULL, NULL, bake_job_complete, bake_job_canceled);

  G.is_break = false;
  G.is_rendering = true;

  WM_jobs_start(CTX_wm_manager(C), wm_job);

  WM_cursor_wait(false);

  /* add modal handler for ESC */
  WM_event_add_modal_handler(C, op);

  WM_event_add_notifier(C, NC_SCENE | ND_RENDER_RESULT, scene);
  return OPERATOR_RUNNING_MODAL;
}

void OBJECT_OT_bake(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Bake";
  ot->description = "Bake image textures of selected objects";
  ot->idname = "OBJECT_OT_bake";

  /* api callbacks */
  ot->exec = bake_exec;
  ot->modal = bake_modal;
  ot->invoke = bake_invoke;
  ot->poll = ED_operator_object_active_editable_mesh;

  RNA_def_enum(
      ot->srna,
      "type",
      rna_enum_bake_pass_type_items,
      SCE_PASS_COMBINED,
      "Type",
      "Type of pass to bake, some of them may not be supported by the current render engine");
  prop = RNA_def_enum(ot->srna,
                      "pass_filter",
                      rna_enum_bake_pass_filter_type_items,
                      R_BAKE_PASS_FILTER_NONE,
                      "Pass Filter",
                      "Filter to combined, diffuse, glossy, transmission and subsurface passes");
  RNA_def_property_flag(prop, PROP_ENUM_FLAG);
  RNA_def_string_file_path(ot->srna,
                           "filepath",
                           NULL,
                           FILE_MAX,
                           "File Path",
                           "Image filepath to use when saving externally");
  RNA_def_int(ot->srna,
              "width",
              512,
              1,
              INT_MAX,
              "Width",
              "Horizontal dimension of the baking map (external only)",
              64,
              4096);
  RNA_def_int(ot->srna,
              "height",
              512,
              1,
              INT_MAX,
              "Height",
              "Vertical dimension of the baking map (external only)",
              64,
              4096);
  RNA_def_int(ot->srna,
              "margin",
              16,
              0,
              INT_MAX,
              "Margin",
              "Extends the baked result as a post process filter",
              0,
              64);
  RNA_def_enum(ot->srna,
               "margin_type",
               rna_enum_bake_margin_type_items,
               R_BAKE_EXTEND,
               "Margin Type",
               "Which algorithm to use to generate the margin");
  RNA_def_boolean(ot->srna,
                  "use_selected_to_active",
                  false,
                  "Selected to Active",
                  "Bake shading on the surface of selected objects to the active object");
  RNA_def_float(ot->srna,
                "max_ray_distance",
                0.0f,
                0.0f,
                FLT_MAX,
                "Max Ray Distance",
                "The maximum ray distance for matching points between the active and selected "
                "objects. If zero, there is no limit",
                0.0f,
                1.0f);
  RNA_def_float(ot->srna,
                "cage_extrusion",
                0.0f,
                0.0f,
                FLT_MAX,
                "Cage Extrusion",
                "Inflate the active object by the specified distance for baking. This helps "
                "matching to points nearer to the outside of the selected object meshes",
                0.0f,
                1.0f);
  RNA_def_string(ot->srna,
                 "cage_object",
                 NULL,
                 MAX_NAME,
                 "Cage Object",
                 "Object to use as cage, instead of calculating the cage from the active object "
                 "with cage extrusion");
  RNA_def_enum(ot->srna,
               "normal_space",
               rna_enum_normal_space_items,
               R_BAKE_SPACE_TANGENT,
               "Normal Space",
               "Choose normal space for baking");
  RNA_def_enum(ot->srna,
               "normal_r",
               rna_enum_normal_swizzle_items,
               R_BAKE_POSX,
               "R",
               "Axis to bake in red channel");
  RNA_def_enum(ot->srna,
               "normal_g",
               rna_enum_normal_swizzle_items,
               R_BAKE_POSY,
               "G",
               "Axis to bake in green channel");
  RNA_def_enum(ot->srna,
               "normal_b",
               rna_enum_normal_swizzle_items,
               R_BAKE_POSZ,
               "B",
               "Axis to bake in blue channel");
  RNA_def_enum(ot->srna,
               "target",
               rna_enum_bake_target_items,
               R_BAKE_TARGET_IMAGE_TEXTURES,
               "Target",
               "Where to output the baked map");
  RNA_def_enum(ot->srna,
               "save_mode",
               rna_enum_bake_save_mode_items,
               R_BAKE_SAVE_INTERNAL,
               "Save Mode",
               "Where to save baked image textures");
  RNA_def_boolean(ot->srna,
                  "use_clear",
                  false,
                  "Clear",
                  "Clear images before baking (only for internal saving)");
  RNA_def_boolean(ot->srna, "use_cage", false, "Cage", "Cast rays to active object from a cage");
  RNA_def_boolean(
      ot->srna,
      "use_split_materials",
      false,
      "Split Materials",
      "Split baked maps per material, using material name in output file (external only)");
  RNA_def_boolean(ot->srna,
                  "use_automatic_name",
                  false,
                  "Automatic Name",
                  "Automatically name the output file with the pass type");
  RNA_def_string(ot->srna,
                 "uv_layer",
                 NULL,
                 MAX_CUSTOMDATA_LAYER_NAME,
                 "UV Layer",
                 "UV layer to override active");
}
