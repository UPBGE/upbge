/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "BLI_compiler_compat.h"

#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_scene_types.h"

#include "BKE_customdata.h"
#include "BKE_image.h"
#include "BKE_material.h"
#include "BKE_paint.h"

#include "IMB_imbuf_types.h"

namespace blender::bke::paint::canvas {
static TexPaintSlot *get_active_slot(Object *ob)
{
  Material *mat = BKE_object_material_get(ob, ob->actcol);
  if (mat == nullptr) {
    return nullptr;
  }
  if (mat->texpaintslot == nullptr) {
    return nullptr;
  }
  if (mat->paint_active_slot >= mat->tot_slots) {
    return nullptr;
  }

  TexPaintSlot *slot = &mat->texpaintslot[mat->paint_active_slot];
  return slot;
}

}  // namespace blender::bke::paint::canvas

extern "C" {

using namespace blender::bke::paint::canvas;

bool BKE_paint_canvas_image_get(PaintModeSettings *settings,
                                Object *ob,
                                Image **r_image,
                                ImageUser **r_image_user)
{
  *r_image = nullptr;
  *r_image_user = nullptr;

  switch (settings->canvas_source) {
    case PAINT_CANVAS_SOURCE_COLOR_ATTRIBUTE:
      break;

    case PAINT_CANVAS_SOURCE_IMAGE:
      *r_image = settings->canvas_image;
      *r_image_user = &settings->image_user;
      break;

    case PAINT_CANVAS_SOURCE_MATERIAL: {
      TexPaintSlot *slot = get_active_slot(ob);
      if (slot == nullptr) {
        break;
      }

      *r_image = slot->ima;
      *r_image_user = slot->image_user;
      break;
    }
  }
  return *r_image != nullptr;
}

int BKE_paint_canvas_uvmap_layer_index_get(const struct PaintModeSettings *settings,
                                           struct Object *ob)
{
  switch (settings->canvas_source) {
    case PAINT_CANVAS_SOURCE_COLOR_ATTRIBUTE:
      return -1;
    case PAINT_CANVAS_SOURCE_IMAGE: {
      /* Use active uv map of the object. */
      if (ob->type != OB_MESH) {
        return -1;
      }

      const Mesh *mesh = static_cast<Mesh *>(ob->data);
      return CustomData_get_active_layer_index(&mesh->ldata, CD_MLOOPUV);
    }
    case PAINT_CANVAS_SOURCE_MATERIAL: {
      /* Use uv map of the canvas. */
      TexPaintSlot *slot = get_active_slot(ob);
      if (slot == nullptr) {
        break;
      }

      if (ob->type != OB_MESH) {
        return -1;
      }

      if (slot->uvname == nullptr) {
        return -1;
      }

      const Mesh *mesh = static_cast<Mesh *>(ob->data);
      return CustomData_get_named_layer_index(&mesh->ldata, CD_MLOOPUV, slot->uvname);
    }
  }
  return -1;
}

char *BKE_paint_canvas_key_get(struct PaintModeSettings *settings, struct Object *ob)
{
  std::stringstream ss;
  int active_uv_map_layer_index = BKE_paint_canvas_uvmap_layer_index_get(settings, ob);
  ss << "UV_MAP:" << active_uv_map_layer_index;

  Image *image;
  ImageUser *image_user;
  if (BKE_paint_canvas_image_get(settings, ob, &image, &image_user)) {
    ImageUser tile_user = *image_user;
    LISTBASE_FOREACH (ImageTile *, image_tile, &image->tiles) {
      tile_user.tile = image_tile->tile_number;
      ImBuf *image_buffer = BKE_image_acquire_ibuf(image, &tile_user, nullptr);
      if (!image_buffer) {
        continue;
      }
      ss << ",TILE_" << image_tile->tile_number;
      ss << "(" << image_buffer->x << "," << image_buffer->y << ")";
      BKE_image_release_ibuf(image, image_buffer, nullptr);
    }
  }

  return BLI_strdup(ss.str().c_str());
}
}
