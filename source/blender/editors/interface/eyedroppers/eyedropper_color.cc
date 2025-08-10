/* SPDX-FileCopyrightText: 2009 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Eyedropper (RGB Color)
 *
 * Defines:
 * - #UI_OT_eyedropper_color
 */

#include "MEM_guardedalloc.h"

#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_string_ref.hh"

#include "BKE_context.hh"
#include "BKE_cryptomatte.h"
#include "BKE_image.hh"
#include "BKE_material.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "BLT_translation.hh"

#include "NOD_composite.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_path.hh"
#include "RNA_prototypes.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "interface_intern.hh"

#include "ED_clip.hh"
#include "ED_image.hh"
#include "ED_node.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "RE_pipeline.h"

#include "eyedropper_intern.hh"

struct Eyedropper {
  const ColorManagedDisplay *display = nullptr;

  PointerRNA ptr = {};
  PropertyRNA *prop = nullptr;
  int index = 0;
  bool is_undo = false;

  bool is_set = false;
  float init_col[3] = {}; /* for resetting on cancel */

  bool accum_start = false; /* has mouse been pressed */
  float accum_col[3] = {};
  int accum_tot = 0;

  wmWindow *cb_win = nullptr;
  int cb_win_event_xy[2] = {};
  void *draw_handle_sample_text = nullptr;
  char sample_text[MAX_NAME] = {};

  bNode *crypto_node = nullptr;
  CryptomatteSession *cryptomatte_session = nullptr;
  ViewportColorSampleSession *viewport_session = nullptr;
};

static void eyedropper_draw_cb(const wmWindow * /*window*/, void *arg)
{
  Eyedropper *eye = static_cast<Eyedropper *>(arg);
  eyedropper_draw_cursor_text_region(eye->cb_win_event_xy, eye->sample_text);
}

/* A heuristic to check whether the current eyedropper destination property is used for non-color
 * painting. If so, the eyedropper will ignore the PROP_COLOR_GAMMA nature of the property and
 * not convert linear colors to display space.
 *
 * The current logic is targeting texture painting, both 2D and 3D. It assumes that invoking the
 * operator from 3D viewport means 3D painting, and invoking from image editor means 2D painting.
 *
 * For the 3D painting the function checks whether active object is in texture paint mode, and if
 * so checks the active image (via material slot, or the explicitly specified image) to have
 * non-color (data) colorspace.
 *
 * For the 2D painting it checks the active image editor's image colorspace.
 *
 * Since brush color could be re-used from multiple spaces the check is not fully reliable: it is
 * possible to invoke sampling from one editor and do stroke in other editor. There is no easy way
 * of dealing with this, and it is unlikely to be a common configuration. */
static bool is_data_destination(const bContext *C, const Eyedropper *eye)
{
  if (eye->ptr.type != &RNA_Brush) {
    return false;
  }

  const View3D *v3d = CTX_wm_view3d(C);
  if (v3d) {
    /*const*/ Object *object = CTX_data_active_object(C);
    if (!object) {
      return false;
    }
    if ((object->mode & OB_MODE_TEXTURE_PAINT) == 0) {
      return false;
    }

    const Scene *scene = CTX_data_scene(C);
    const ImagePaintSettings &settings = scene->toolsettings->imapaint;
    Image *image = nullptr;
    if (settings.mode == IMAGEPAINT_MODE_MATERIAL) {
      Material *material = BKE_object_material_get(object, object->actcol);
      if (material && material->texpaintslot) {
        image = material->texpaintslot[material->paint_active_slot].ima;
      }
    }
    else if (settings.mode == IMAGEPAINT_MODE_IMAGE) {
      image = settings.canvas;
    }

    return image && IMB_colormanagement_space_name_is_data(image->colorspace_settings.name);
  }

  const SpaceImage *space_image = CTX_wm_space_image(C);
  if (space_image) {
    return space_image->image &&
           IMB_colormanagement_space_name_is_data(space_image->image->colorspace_settings.name);
  }

  return false;
}

static bool eyedropper_init(bContext *C, wmOperator *op)
{
  Eyedropper *eye = MEM_new<Eyedropper>(__func__);

  PropertyRNA *prop = RNA_struct_find_property(op->ptr, "prop_data_path");
  if (prop && RNA_property_is_set(op->ptr, prop)) {
    std::string prop_data_path = RNA_string_get(op->ptr, "prop_data_path");
    if (prop_data_path.empty()) {
      MEM_delete(eye);
      return false;
    }
    PointerRNA ctx_ptr = RNA_pointer_create_discrete(nullptr, &RNA_Context, C);
    if (!RNA_path_resolve(&ctx_ptr, prop_data_path.c_str(), &eye->ptr, &eye->prop)) {
      BKE_reportf(op->reports, RPT_ERROR, "Could not resolve path '%s'", prop_data_path.c_str());
      MEM_delete(eye);
      return false;
    }
    eye->is_undo = true;
  }
  else {
    uiBut *but = UI_context_active_but_prop_get(C, &eye->ptr, &eye->prop, &eye->index);
    if (but != nullptr) {
      eye->is_undo = UI_but_flag_is_set(but, UI_BUT_UNDO);
    }
  }

  const enum PropertySubType prop_subtype = eye->prop ? RNA_property_subtype(eye->prop) :
                                                        PropertySubType(0);

  if ((eye->ptr.data == nullptr) || (eye->prop == nullptr) ||
      (RNA_property_editable(&eye->ptr, eye->prop) == false) ||
      (RNA_property_array_length(&eye->ptr, eye->prop) < 3) ||
      (RNA_property_type(eye->prop) != PROP_FLOAT) ||
      (ELEM(prop_subtype, PROP_COLOR, PROP_COLOR_GAMMA) == 0))
  {
    MEM_delete(eye);
    return false;
  }
  op->customdata = eye;

  float col[4];
  RNA_property_float_get_array_at_most(&eye->ptr, eye->prop, col, ARRAY_SIZE(col));
  if (eye->ptr.type == &RNA_CompositorNodeCryptomatteV2) {
    eye->crypto_node = (bNode *)eye->ptr.data;
    eye->cryptomatte_session = ntreeCompositCryptomatteSession(eye->crypto_node);
    eye->cb_win = CTX_wm_window(C);
    eye->draw_handle_sample_text = WM_draw_cb_activate(eye->cb_win, eyedropper_draw_cb, eye);
  }

  if (prop_subtype != PROP_COLOR && !is_data_destination(C, eye)) {
    Scene *scene = CTX_data_scene(C);
    const char *display_device;

    display_device = scene->display_settings.display_device;
    eye->display = IMB_colormanagement_display_get_named(display_device);

    /* store initial color */
    if (eye->display) {
      IMB_colormanagement_display_to_scene_linear_v3(col, eye->display);
    }
  }
  copy_v3_v3(eye->init_col, col);

  return true;
}

static void eyedropper_exit(bContext *C, wmOperator *op)
{
  Eyedropper *eye = static_cast<Eyedropper *>(op->customdata);
  wmWindow *window = CTX_wm_window(C);
  WM_cursor_modal_restore(window);

  ED_workspace_status_text(C, nullptr);

  if (eye->draw_handle_sample_text) {
    WM_draw_cb_exit(eye->cb_win, eye->draw_handle_sample_text);
    eye->draw_handle_sample_text = nullptr;
  }

  if (eye->cryptomatte_session) {
    BKE_cryptomatte_free(eye->cryptomatte_session);
    eye->cryptomatte_session = nullptr;
  }

  if (eye->viewport_session) {
    MEM_delete(eye->viewport_session);
    eye->viewport_session = nullptr;
  }

  op->customdata = nullptr;
  MEM_delete(eye);
}

/* *** eyedropper_color_ helper functions *** */

static bool eyedropper_cryptomatte_sample_view3d_fl(bContext *C,
                                                    const char *type_name,
                                                    const int mval[2],
                                                    float r_col[3])
{
  int material_slot = 0;
  Object *object = ED_view3d_give_material_slot_under_cursor(C, mval, &material_slot);
  if (!object) {
    return false;
  }

  const ID *id = nullptr;
  if (blender::StringRef(type_name).endswith(RE_PASSNAME_CRYPTOMATTE_OBJECT)) {
    id = &object->id;
  }
  else if (blender::StringRef(type_name).endswith(RE_PASSNAME_CRYPTOMATTE_MATERIAL)) {
    Material *material = BKE_object_material_get(object, material_slot);
    if (!material) {
      return false;
    }
    id = &material->id;
  }

  if (!id) {
    return false;
  }

  const char *name = &id->name[2];
  const int name_length = BLI_strnlen(name, MAX_NAME - 2);
  uint32_t cryptomatte_hash = BKE_cryptomatte_hash(name, name_length);
  r_col[0] = BKE_cryptomatte_hash_to_float(cryptomatte_hash);
  return true;
}

static bool eyedropper_cryptomatte_sample_renderlayer_fl(RenderLayer *render_layer,
                                                         const char *prefix,
                                                         const float fpos[2],
                                                         float r_col[3])
{
  if (!render_layer) {
    return false;
  }

  const int render_layer_name_len = STRNLEN(render_layer->name);
  if (strncmp(prefix, render_layer->name, render_layer_name_len) != 0) {
    return false;
  }

  const int prefix_len = strlen(prefix);
  if (prefix_len <= render_layer_name_len + 1) {
    return false;
  }

  /* RenderResult from images can have no render layer name. */
  const char *render_pass_name_prefix = render_layer_name_len ?
                                            prefix + 1 + render_layer_name_len :
                                            prefix;

  LISTBASE_FOREACH (RenderPass *, render_pass, &render_layer->passes) {
    if (STRPREFIX(render_pass->name, render_pass_name_prefix) &&
        !STREQLEN(render_pass->name, render_pass_name_prefix, sizeof(render_pass->name)))
    {
      BLI_assert(render_pass->channels == 4);

      /* Pass was allocated but not rendered yet. */
      if (!render_pass->ibuf) {
        return false;
      }

      const int x = int(fpos[0] * render_pass->rectx);
      const int y = int(fpos[1] * render_pass->recty);
      const int offset = 4 * (y * render_pass->rectx + x);
      zero_v3(r_col);
      r_col[0] = render_pass->ibuf->float_buffer.data[offset];
      return true;
    }
  }

  return false;
}

static bool eyedropper_cryptomatte_sample_render_fl(const bNode *node,
                                                    const char *prefix,
                                                    const float fpos[2],
                                                    float r_col[3])
{
  bool success = false;
  Scene *scene = (Scene *)node->id;
  BLI_assert(GS(scene->id.name) == ID_SCE);
  Render *re = RE_GetSceneRender(scene);

  if (re) {
    RenderResult *rr = RE_AcquireResultRead(re);
    if (rr) {
      LISTBASE_FOREACH (ViewLayer *, view_layer, &scene->view_layers) {
        RenderLayer *render_layer = RE_GetRenderLayer(rr, view_layer->name);
        success = eyedropper_cryptomatte_sample_renderlayer_fl(render_layer, prefix, fpos, r_col);
        if (success) {
          break;
        }
      }
    }
    RE_ReleaseResult(re);
  }
  return success;
}

static bool eyedropper_cryptomatte_sample_image_fl(bContext *C,
                                                   const bNode *node,
                                                   NodeCryptomatte *crypto,
                                                   const char *prefix,
                                                   const float fpos[2],
                                                   float r_col[3])
{
  bool success = false;
  Image *image = (Image *)node->id;
  BLI_assert((image == nullptr) || (GS(image->id.name) == ID_IM));

  /* Compute the effective frame number of the image if it was animated. */
  Scene *scene = CTX_data_scene(C);
  ImageUser image_user_for_frame = crypto->iuser;
  BKE_image_user_frame_calc(image, &image_user_for_frame, scene->r.cfra);

  if (image && image->type == IMA_TYPE_MULTILAYER) {
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &image_user_for_frame, nullptr);
    if (image->rr) {
      LISTBASE_FOREACH (RenderLayer *, render_layer, &image->rr->layers) {
        success = eyedropper_cryptomatte_sample_renderlayer_fl(render_layer, prefix, fpos, r_col);
        if (success) {
          break;
        }
      }
    }
    BKE_image_release_ibuf(image, ibuf, nullptr);
  }
  return success;
}

static bool eyedropper_cryptomatte_sample_fl(bContext *C,
                                             Eyedropper *eye,
                                             const int event_xy[2],
                                             float r_col[3])
{
  bNode *node = eye->crypto_node;
  NodeCryptomatte *crypto = node ? ((NodeCryptomatte *)node->storage) : nullptr;

  if (!crypto) {
    return false;
  }

  ScrArea *area = nullptr;

  int event_xy_win[2];
  wmWindow *win = WM_window_find_under_cursor(CTX_wm_window(C), event_xy, event_xy_win);
  if (win) {
    bScreen *screen = WM_window_get_active_screen(win);
    area = BKE_screen_find_area_xy(screen, SPACE_TYPE_ANY, event_xy_win);
  }

  eye->cb_win_event_xy[0] = event_xy_win[0];
  eye->cb_win_event_xy[1] = event_xy_win[1];

  if (win && win != eye->cb_win && eye->draw_handle_sample_text) {
    WM_draw_cb_exit(eye->cb_win, eye->draw_handle_sample_text);
    eye->cb_win = win;
    eye->draw_handle_sample_text = WM_draw_cb_activate(eye->cb_win, eyedropper_draw_cb, eye);
    ED_region_tag_redraw(CTX_wm_region(C));
  }

  if (!area || !ELEM(area->spacetype, SPACE_IMAGE, SPACE_NODE, SPACE_CLIP, SPACE_VIEW3D)) {
    return false;
  }

  ARegion *region = BKE_area_find_region_xy(area, RGN_TYPE_WINDOW, event_xy_win);

  if (!region) {
    return false;
  }

  const int mval[2] = {
      event_xy_win[0] - region->winrct.xmin,
      event_xy_win[1] - region->winrct.ymin,
  };
  float fpos[2] = {-1.0f, -1.0};
  switch (area->spacetype) {
    case SPACE_IMAGE: {
      SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
      ED_space_image_get_position(sima, region, mval, fpos);
      break;
    }
    case SPACE_NODE: {
      Main *bmain = CTX_data_main(C);
      SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
      ED_space_node_get_position(bmain, snode, region, mval, fpos);
      break;
    }
    case SPACE_CLIP: {
      SpaceClip *sc = static_cast<SpaceClip *>(area->spacedata.first);
      ED_space_clip_get_position(sc, region, mval, fpos);
      break;
    }
    default: {
      break;
    }
  }

  if (area->spacetype != SPACE_VIEW3D &&
      (fpos[0] < 0.0f || fpos[1] < 0.0f || fpos[0] >= 1.0f || fpos[1] >= 1.0f))
  {
    return false;
  }

  /* CMP_NODE_CRYPTOMATTE_SOURCE_RENDER and CMP_NODE_CRYPTOMATTE_SOURCE_IMAGE require a referenced
   * image/scene to work properly. */
  if (!node->id) {
    return false;
  }

  ED_region_tag_redraw(region);

  /* TODO(jbakker): Migrate this file to cc and use std::string as return param. */
  char prefix[MAX_NAME + 1];
  ntreeCompositCryptomatteLayerPrefix(node, prefix, sizeof(prefix) - 1);
  prefix[MAX_NAME] = '\0';

  if (area->spacetype == SPACE_VIEW3D) {
    wmWindow *win_prev = CTX_wm_window(C);
    ScrArea *area_prev = CTX_wm_area(C);
    ARegion *region_prev = CTX_wm_region(C);

    CTX_wm_window_set(C, win);
    CTX_wm_area_set(C, area);
    CTX_wm_region_set(C, region);

    const bool success = eyedropper_cryptomatte_sample_view3d_fl(C, prefix, mval, r_col);

    CTX_wm_window_set(C, win_prev);
    CTX_wm_area_set(C, area_prev);
    CTX_wm_region_set(C, region_prev);

    return success;
  }
  if (node->custom1 == CMP_NODE_CRYPTOMATTE_SOURCE_RENDER) {
    return eyedropper_cryptomatte_sample_render_fl(node, prefix, fpos, r_col);
  }
  if (node->custom1 == CMP_NODE_CRYPTOMATTE_SOURCE_IMAGE) {
    return eyedropper_cryptomatte_sample_image_fl(C, node, crypto, prefix, fpos, r_col);
  }
  return false;
}

bool eyedropper_color_sample_fl(bContext *C,
                                Eyedropper *eye,
                                const int event_xy[2],
                                float r_col[3])
{
  ScrArea *area = nullptr;

  int event_xy_win[2];
  wmWindow *win = WM_window_find_under_cursor(CTX_wm_window(C), event_xy, event_xy_win);
  if (win) {
    bScreen *screen = WM_window_get_active_screen(win);
    area = BKE_screen_find_area_xy(screen, SPACE_TYPE_ANY, event_xy_win);
  }

  if (area) {
    ARegion *region = BKE_area_find_region_xy(area, RGN_TYPE_WINDOW, event_xy_win);
    if (region) {
      const int mval[2] = {
          event_xy_win[0] - region->winrct.xmin,
          event_xy_win[1] - region->winrct.ymin,
      };
      if (area->spacetype == SPACE_IMAGE) {
        SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
        if (ED_space_image_color_sample(sima, region, mval, r_col, nullptr)) {
          return true;
        }
      }
      else if (area->spacetype == SPACE_NODE) {
        SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
        Main *bmain = CTX_data_main(C);
        if (ED_space_node_color_sample(bmain, snode, region, mval, r_col)) {
          return true;
        }
      }
      else if (area->spacetype == SPACE_CLIP) {
        SpaceClip *sc = static_cast<SpaceClip *>(area->spacedata.first);
        if (ED_space_clip_color_sample(sc, region, mval, r_col)) {
          return true;
        }
      }
      else if (eye != nullptr && area->spacetype == SPACE_VIEW3D) {
        /* Viewport color picking involves a fairly expensive operation to copy the GPU viewport
         * back to the CPU, so to support smooth dragging with the eyedropper, we keep the copy
         * around for the entire operation. */
        if (eye->viewport_session == nullptr) {
          eye->viewport_session = MEM_new<ViewportColorSampleSession>("viewport_session");
          eye->viewport_session->init(region);
        }
        if (eye->viewport_session->sample(mval, r_col)) {
          return true;
        }
      }
    }
  }

  /* Other areas within a Blender window. */
  if (win) {
    if (!WM_window_pixels_read_sample(C, win, event_xy_win, r_col)) {
      WM_window_pixels_read_sample_from_offscreen(C, win, event_xy_win, r_col);
    }
    const char *display_device = CTX_data_scene(C)->display_settings.display_device;
    const ColorManagedDisplay *display = IMB_colormanagement_display_get_named(display_device);
    IMB_colormanagement_display_to_scene_linear_v3(r_col, display);
    return true;
  }

  /* Outside the Blender window if we support it. */
  if (WM_capabilities_flag() & WM_CAPABILITY_DESKTOP_SAMPLE) {
    if (WM_desktop_cursor_sample_read(r_col)) {
      IMB_colormanagement_srgb_to_scene_linear_v3(r_col, r_col);
      return true;
    }
  }

  zero_v3(r_col);
  return false;
}

/* sets the sample color RGB, maintaining A */
static void eyedropper_color_set(bContext *C, Eyedropper *eye, const float col[3])
{
  float col_conv[4];

  /* to maintain alpha */
  RNA_property_float_get_array_at_most(&eye->ptr, eye->prop, col_conv, ARRAY_SIZE(col_conv));

  /* convert from linear rgb space to display space */
  if (eye->display) {
    copy_v3_v3(col_conv, col);
    IMB_colormanagement_scene_linear_to_display_v3(col_conv, eye->display);
  }
  else {
    copy_v3_v3(col_conv, col);
  }

  RNA_property_float_set_array_at_most(&eye->ptr, eye->prop, col_conv, ARRAY_SIZE(col_conv));
  eye->is_set = true;

  RNA_property_update(C, &eye->ptr, eye->prop);
}

static void eyedropper_color_sample(bContext *C, Eyedropper *eye, const int event_xy[2])
{
  /* Accumulate color. */
  float col[3];
  if (eye->crypto_node) {
    if (!eyedropper_cryptomatte_sample_fl(C, eye, event_xy, col)) {
      return;
    }
  }
  else {
    if (!eyedropper_color_sample_fl(C, eye, event_xy, col)) {
      return;
    }
  }

  if (!eye->crypto_node) {
    add_v3_v3(eye->accum_col, col);
    eye->accum_tot++;
  }
  else {
    copy_v3_v3(eye->accum_col, col);
    eye->accum_tot = 1;
  }

  /* Apply to property. */
  float accum_col[3];
  if (eye->accum_tot > 1) {
    mul_v3_v3fl(accum_col, eye->accum_col, 1.0f / float(eye->accum_tot));
  }
  else {
    copy_v3_v3(accum_col, eye->accum_col);
  }
  eyedropper_color_set(C, eye, accum_col);
}

static void eyedropper_color_sample_text_update(bContext *C,
                                                Eyedropper *eye,
                                                const int event_xy[2])
{
  float col[3];
  eye->sample_text[0] = '\0';

  if (eye->cryptomatte_session) {
    if (eyedropper_cryptomatte_sample_fl(C, eye, event_xy, col)) {
      BKE_cryptomatte_find_name(
          eye->cryptomatte_session, col[0], eye->sample_text, sizeof(eye->sample_text));
      eye->sample_text[sizeof(eye->sample_text) - 1] = '\0';
    }
  }
}

static void eyedropper_cancel(bContext *C, wmOperator *op)
{
  Eyedropper *eye = static_cast<Eyedropper *>(op->customdata);
  if (eye->is_set) {
    eyedropper_color_set(C, eye, eye->init_col);
  }
  eyedropper_exit(C, op);
}

/* main modal status check */
static wmOperatorStatus eyedropper_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  Eyedropper *eye = (Eyedropper *)op->customdata;

  /* handle modal keymap */
  if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case EYE_MODAL_CANCEL:
        eyedropper_cancel(C, op);
        return OPERATOR_CANCELLED;
      case EYE_MODAL_SAMPLE_CONFIRM: {
        const bool is_undo = eye->is_undo;
        if (eye->accum_tot == 0) {
          eyedropper_color_sample(C, eye, event->xy);
        }
        eyedropper_exit(C, op);
        /* Could support finished & undo-skip. */
        return is_undo ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
      }
      case EYE_MODAL_SAMPLE_BEGIN:
        /* enable accum and make first sample */
        eye->accum_start = true;
        eyedropper_color_sample(C, eye, event->xy);
        break;
      case EYE_MODAL_SAMPLE_RESET:
        eye->accum_tot = 0;
        zero_v3(eye->accum_col);
        eyedropper_color_sample(C, eye, event->xy);
        break;
    }
  }
  else if (ISMOUSE_MOTION(event->type)) {
    if (eye->accum_start) {
      /* button is pressed so keep sampling */
      eyedropper_color_sample(C, eye, event->xy);
      WorkspaceStatus status(C);
      status.item(TIP_("Drag to continue sampling, release when done"), ICON_MOUSE_MOVE);
    }
    else {
      WorkspaceStatus status(C);
      status.opmodal(IFACE_("Confirm"), op->type, EYE_MODAL_SAMPLE_CONFIRM);
      status.opmodal(IFACE_("Cancel"), op->type, EYE_MODAL_CANCEL);
#ifdef __APPLE__
      status.item(TIP_("Press 'Enter' to sample outside of a Blender window"), ICON_INFO);
#endif
    }

    if (eye->draw_handle_sample_text) {
      eyedropper_color_sample_text_update(C, eye, event->xy);
    }
  }

  return OPERATOR_RUNNING_MODAL;
}

/* Modal Operator init */
static wmOperatorStatus eyedropper_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  /* init */
  if (eyedropper_init(C, op)) {
    wmWindow *win = CTX_wm_window(C);
    /* Workaround for de-activating the button clearing the cursor, see #76794 */
    UI_context_active_but_clear(C, win, CTX_wm_region(C));
    WM_cursor_modal_set(win, WM_CURSOR_EYEDROPPER);

    /* add temp handler */
    WM_event_add_modal_handler(C, op);

    return OPERATOR_RUNNING_MODAL;
  }
  return OPERATOR_PASS_THROUGH;
}

/* Repeat operator */
static wmOperatorStatus eyedropper_exec(bContext *C, wmOperator *op)
{
  /* init */
  if (eyedropper_init(C, op)) {

    /* do something */

    /* cleanup */
    eyedropper_exit(C, op);

    return OPERATOR_FINISHED;
  }
  return OPERATOR_PASS_THROUGH;
}

static bool eyedropper_poll(bContext *C)
{
  /* Actual test for active button happens later, since we don't
   * know which one is active until mouse over. */
  return (CTX_wm_window(C) != nullptr);
}

void UI_OT_eyedropper_color(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Eyedropper";
  ot->idname = "UI_OT_eyedropper_color";
  ot->description = "Sample a color from the Blender window to store in a property";

  /* API callbacks. */
  ot->invoke = eyedropper_invoke;
  ot->modal = eyedropper_modal;
  ot->cancel = eyedropper_cancel;
  ot->exec = eyedropper_exec;
  ot->poll = eyedropper_poll;

  /* flags */
  ot->flag = OPTYPE_UNDO | OPTYPE_BLOCKING | OPTYPE_INTERNAL;

  /* Paths relative to the context. */
  PropertyRNA *prop;
  prop = RNA_def_string(ot->srna,
                        "prop_data_path",
                        nullptr,
                        0,
                        "Data Path",
                        "Path of property to be set with the depth");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}
