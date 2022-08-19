/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2018 Blender Foundation. */

/** \file
 * \ingroup draw_engine
 */

#include "workbench_private.h"

#include "BLI_memblock.h"

#include "BKE_image.h"
#include "BKE_node.h"

#include "BLI_dynstr.h"
#include "BLI_hash.h"

#include "DNA_mesh_types.h"
#include "DNA_node_types.h"

#include "GPU_uniform_buffer.h"

#include "ED_uvedit.h"

#define HSV_SATURATION 0.5
#define HSV_VALUE 0.8

void workbench_material_ubo_data(WORKBENCH_PrivateData *wpd,
                                 Object *ob,
                                 Material *mat,
                                 WORKBENCH_UBO_Material *data,
                                 eV3DShadingColorType color_type)
{
  float metallic = 0.0f;
  float roughness = 0.632455532f; /* sqrtf(0.4f); */
  float alpha = wpd->shading.xray_alpha;

  switch (color_type) {
    case V3D_SHADING_SINGLE_COLOR:
      copy_v3_v3(data->base_color, wpd->shading.single_color);
      break;
    case V3D_SHADING_RANDOM_COLOR: {
      uint hash = BLI_ghashutil_strhash_p_murmur(ob->id.name);
      if (ob->id.lib) {
        hash = (hash * 13) ^ BLI_ghashutil_strhash_p_murmur(ob->id.lib->filepath);
      }
      float hue = BLI_hash_int_01(hash);
      const float hsv[3] = {hue, HSV_SATURATION, HSV_VALUE};
      hsv_to_rgb_v(hsv, data->base_color);
      break;
    }
    case V3D_SHADING_OBJECT_COLOR:
    case V3D_SHADING_VERTEX_COLOR:
      alpha *= ob->color[3];
      copy_v3_v3(data->base_color, ob->color);
      break;
    case V3D_SHADING_MATERIAL_COLOR:
    case V3D_SHADING_TEXTURE_COLOR:
    default:
      if (mat) {
        alpha *= mat->a;
        copy_v3_v3(data->base_color, &mat->r);
        metallic = mat->metallic;
        roughness = sqrtf(mat->roughness); /* Remap to Disney roughness. */
      }
      else {
        copy_v3_fl(data->base_color, 0.8f);
      }
      break;
  }

  uint32_t packed_metallic = unit_float_to_uchar_clamp(metallic);
  uint32_t packed_roughness = unit_float_to_uchar_clamp(roughness);
  uint32_t packed_alpha = unit_float_to_uchar_clamp(alpha);
  data->packed_data = (packed_alpha << 16u) | (packed_roughness << 8u) | packed_metallic;
}

/* Return correct material or empty default material if slot is empty. */
BLI_INLINE Material *workbench_object_material_get(Object *ob, int mat_nr)
{
  Material *ma = BKE_object_material_get_eval(ob, mat_nr);
  if (ma == NULL) {
    ma = BKE_material_default_empty();
  }
  return ma;
}

BLI_INLINE void workbench_material_get_image(
    Object *ob, int mat_nr, Image **r_image, ImageUser **r_iuser, eGPUSamplerState *r_sampler)
{
  bNode *node;
  *r_sampler = 0;

  ED_object_get_active_image(ob, mat_nr, r_image, r_iuser, &node, NULL);
  if (node && *r_image) {
    switch (node->type) {
      case SH_NODE_TEX_IMAGE: {
        NodeTexImage *storage = node->storage;
        const bool use_filter = (storage->interpolation != SHD_INTERP_CLOSEST);
        const bool use_repeat = (storage->extension == SHD_IMAGE_EXTENSION_REPEAT);
        const bool use_clip = (storage->extension == SHD_IMAGE_EXTENSION_CLIP);
        SET_FLAG_FROM_TEST(*r_sampler, use_filter, GPU_SAMPLER_FILTER);
        SET_FLAG_FROM_TEST(*r_sampler, use_repeat, GPU_SAMPLER_REPEAT);
        SET_FLAG_FROM_TEST(*r_sampler, use_clip, GPU_SAMPLER_CLAMP_BORDER);
        break;
      }
      case SH_NODE_TEX_ENVIRONMENT: {
        NodeTexEnvironment *storage = node->storage;
        const bool use_filter = (storage->interpolation != SHD_INTERP_CLOSEST);
        SET_FLAG_FROM_TEST(*r_sampler, use_filter, GPU_SAMPLER_FILTER);
        break;
      }
      default:
        BLI_assert_msg(0, "Node type not supported by workbench");
    }
  }
}

/* Return true if the current material ubo has changed and needs to be rebind. */
BLI_INLINE bool workbench_material_chunk_select(WORKBENCH_PrivateData *wpd,
                                                uint32_t id,
                                                uint32_t *r_mat_id)
{
  bool resource_changed = false;
  /* Divide in chunks of MAX_MATERIAL. */
  uint32_t chunk = id >> 12u;
  *r_mat_id = id & 0xFFFu;
  /* We need to add a new chunk. */
  while (chunk >= wpd->material_chunk_count) {
    wpd->material_chunk_count++;
    wpd->material_ubo_data_curr = BLI_memblock_alloc(wpd->material_ubo_data);
    wpd->material_ubo_curr = workbench_material_ubo_alloc(wpd);
    wpd->material_chunk_curr = chunk;
    resource_changed = true;
  }
  /* We need to go back to a previous chunk. */
  if (wpd->material_chunk_curr != chunk) {
    wpd->material_ubo_data_curr = BLI_memblock_elem_get(wpd->material_ubo_data, 0, chunk);
    wpd->material_ubo_curr = BLI_memblock_elem_get(wpd->material_ubo, 0, chunk);
    wpd->material_chunk_curr = chunk;
    resource_changed = true;
  }
  return resource_changed;
}

DRWShadingGroup *workbench_material_setup_ex(WORKBENCH_PrivateData *wpd,
                                             Object *ob,
                                             int mat_nr,
                                             eV3DShadingColorType color_type,
                                             eWORKBENCH_DataType datatype,
                                             bool *r_transp)
{
  Image *ima = NULL;
  ImageUser *iuser = NULL;
  eGPUSamplerState sampler;
  const bool infront = (ob->dtx & OB_DRAW_IN_FRONT) != 0;

  if (color_type == V3D_SHADING_TEXTURE_COLOR) {
    workbench_material_get_image(ob, mat_nr, &ima, &iuser, &sampler);
    if (ima == NULL) {
      /* Fallback to material color. */
      color_type = V3D_SHADING_MATERIAL_COLOR;
    }
  }

  switch (color_type) {
    case V3D_SHADING_TEXTURE_COLOR: {
      return workbench_image_setup_ex(wpd, ob, mat_nr, ima, iuser, sampler, datatype);
    }
    case V3D_SHADING_MATERIAL_COLOR: {
      /* For now, we use the same ubo for material and object coloring but with different indices.
       * This means they are mutually exclusive. */
      BLI_assert(
          ELEM(wpd->shading.color_type, V3D_SHADING_MATERIAL_COLOR, V3D_SHADING_TEXTURE_COLOR));

      Material *ma = workbench_object_material_get(ob, mat_nr);

      const bool transp = wpd->shading.xray_alpha < 1.0f || ma->a < 1.0f;
      WORKBENCH_Prepass *prepass = &wpd->prepass[transp][infront][datatype];

      if (r_transp && transp) {
        *r_transp = true;
      }

      DRWShadingGroup **grp_mat = NULL;
      /* A hash-map stores material shgroups to pack all similar drawcalls together. */
      if (BLI_ghash_ensure_p(prepass->material_hash, ma, (void ***)&grp_mat)) {
        return *grp_mat;
      }

      uint32_t mat_id, id = wpd->material_index++;

      workbench_material_chunk_select(wpd, id, &mat_id);
      workbench_material_ubo_data(wpd, ob, ma, &wpd->material_ubo_data_curr[mat_id], color_type);

      DRWShadingGroup *grp = prepass->common_shgrp;
      *grp_mat = grp = DRW_shgroup_create_sub(grp);
      DRW_shgroup_uniform_block(grp, "materials_data", wpd->material_ubo_curr);
      DRW_shgroup_uniform_int_copy(grp, "materialIndex", mat_id);
      return grp;
    }
    case V3D_SHADING_VERTEX_COLOR: {
      const bool transp = wpd->shading.xray_alpha < 1.0f;
      DRWShadingGroup *grp = wpd->prepass[transp][infront][datatype].vcol_shgrp;
      return grp;
    }
    default: {
      /* For now, we use the same ubo for material and object coloring but with different indices.
       * This means they are mutually exclusive. */
      BLI_assert(
          !ELEM(wpd->shading.color_type, V3D_SHADING_MATERIAL_COLOR, V3D_SHADING_TEXTURE_COLOR));

      uint32_t mat_id, id = DRW_object_resource_id_get(ob);

      bool resource_changed = workbench_material_chunk_select(wpd, id, &mat_id);
      workbench_material_ubo_data(wpd, ob, NULL, &wpd->material_ubo_data_curr[mat_id], color_type);

      const bool transp = wpd->shading.xray_alpha < 1.0f || ob->color[3] < 1.0f;
      DRWShadingGroup **grp = &wpd->prepass[transp][infront][datatype].common_shgrp;
      if (resource_changed) {
        *grp = DRW_shgroup_create_sub(*grp);
        DRW_shgroup_uniform_block(*grp, "materials_data", wpd->material_ubo_curr);
      }
      if (r_transp && transp) {
        *r_transp = true;
      }
      return *grp;
    }
  }
}

DRWShadingGroup *workbench_image_setup_ex(WORKBENCH_PrivateData *wpd,
                                          Object *ob,
                                          int mat_nr,
                                          Image *ima,
                                          ImageUser *iuser,
                                          eGPUSamplerState sampler,
                                          eWORKBENCH_DataType datatype)
{
  GPUTexture *tex = NULL, *tex_tile_data = NULL;

  if (ima == NULL) {
    workbench_material_get_image(ob, mat_nr, &ima, &iuser, &sampler);
  }

  if (ima) {
    if (ima->source == IMA_SRC_TILED) {
      tex = BKE_image_get_gpu_tiles(ima, iuser, NULL);
      tex_tile_data = BKE_image_get_gpu_tilemap(ima, iuser, NULL);
    }
    else {
      tex = BKE_image_get_gpu_texture(ima, iuser, NULL);
    }
  }

  if (tex == NULL) {
    tex = wpd->dummy_image_tx;
  }

  const bool infront = (ob->dtx & OB_DRAW_IN_FRONT) != 0;
  const bool transp = wpd->shading.xray_alpha < 1.0f;
  WORKBENCH_Prepass *prepass = &wpd->prepass[transp][infront][datatype];

  DRWShadingGroup **grp_tex = NULL;
  /* A hash-map stores image shgroups to pack all similar drawcalls together. */
  if (BLI_ghash_ensure_p(prepass->material_hash, tex, (void ***)&grp_tex)) {
    return *grp_tex;
  }

  DRWShadingGroup *grp = (tex_tile_data) ? prepass->image_tiled_shgrp : prepass->image_shgrp;

  *grp_tex = grp = DRW_shgroup_create_sub(grp);
  if (tex_tile_data) {
    DRW_shgroup_uniform_texture_ex(grp, "imageTileArray", tex, sampler);
    DRW_shgroup_uniform_texture(grp, "imageTileData", tex_tile_data);
  }
  else {
    DRW_shgroup_uniform_texture_ex(grp, "imageTexture", tex, sampler);
  }
  DRW_shgroup_uniform_bool_copy(grp, "imagePremult", (ima && ima->alpha_mode == IMA_ALPHA_PREMUL));
  DRW_shgroup_uniform_float_copy(grp, "imageTransparencyCutoff", 0.1f);
  return grp;
}
