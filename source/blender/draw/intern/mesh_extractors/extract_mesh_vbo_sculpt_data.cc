/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup draw
 */

#include "MEM_guardedalloc.h"

#include "BLI_string.h"

#include "BKE_paint.h"

#include "draw_subdivision.h"
#include "extract_mesh.hh"

namespace blender::draw {

/* ---------------------------------------------------------------------- */
/** \name Extract Sculpt Data
 * \{ */

static GPUVertFormat *get_sculpt_data_format()
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "fset", GPU_COMP_U8, 4, GPU_FETCH_INT_TO_FLOAT_UNIT);
    GPU_vertformat_attr_add(&format, "msk", GPU_COMP_F32, 1, GPU_FETCH_FLOAT);
  }
  return &format;
}

static void extract_sculpt_data_init(const MeshRenderData *mr,
                                     MeshBatchCache *UNUSED(cache),
                                     void *buf,
                                     void *UNUSED(tls_data))
{
  GPUVertBuf *vbo = static_cast<GPUVertBuf *>(buf);
  GPUVertFormat *format = get_sculpt_data_format();

  CustomData *cd_ldata = (mr->extract_type == MR_EXTRACT_BMESH) ? &mr->bm->ldata : &mr->me->ldata;
  CustomData *cd_vdata = (mr->extract_type == MR_EXTRACT_BMESH) ? &mr->bm->vdata : &mr->me->vdata;
  CustomData *cd_pdata = (mr->extract_type == MR_EXTRACT_BMESH) ? &mr->bm->pdata : &mr->me->pdata;

  const float *cd_mask = (const float *)CustomData_get_layer(cd_vdata, CD_PAINT_MASK);
  const int *cd_face_set = (const int *)CustomData_get_layer(cd_pdata, CD_SCULPT_FACE_SETS);

  GPU_vertbuf_init_with_format(vbo, format);
  GPU_vertbuf_data_alloc(vbo, mr->loop_len);

  struct gpuSculptData {
    uint8_t face_set_color[4];
    float mask;
  };

  gpuSculptData *vbo_data = (gpuSculptData *)GPU_vertbuf_get_data(vbo);
  const MLoop *loops = (const MLoop *)CustomData_get_layer(cd_ldata, CD_MLOOP);

  if (mr->extract_type == MR_EXTRACT_BMESH) {
    int cd_mask_ofs = CustomData_get_offset(cd_vdata, CD_PAINT_MASK);
    int cd_face_set_ofs = CustomData_get_offset(cd_pdata, CD_SCULPT_FACE_SETS);
    BMIter f_iter;
    BMFace *efa;
    BM_ITER_MESH (efa, &f_iter, mr->bm, BM_FACES_OF_MESH) {
      BMLoop *l_iter, *l_first;
      l_iter = l_first = BM_FACE_FIRST_LOOP(efa);
      do {
        float v_mask = 0.0f;
        if (cd_mask) {
          v_mask = BM_ELEM_CD_GET_FLOAT(l_iter->v, cd_mask_ofs);
        }
        vbo_data->mask = v_mask;
        uchar face_set_color[4] = {UCHAR_MAX, UCHAR_MAX, UCHAR_MAX, UCHAR_MAX};
        if (cd_face_set) {
          const int face_set_id = BM_ELEM_CD_GET_INT(l_iter->f, cd_face_set_ofs);
          if (face_set_id != mr->me->face_sets_color_default) {
            BKE_paint_face_set_overlay_color_get(
                face_set_id, mr->me->face_sets_color_seed, face_set_color);
          }
        }
        copy_v3_v3_uchar(vbo_data->face_set_color, face_set_color);
        vbo_data++;
      } while ((l_iter = l_iter->next) != l_first);
    }
  }
  else {
    int mp_loop = 0;
    for (int mp_index = 0; mp_index < mr->poly_len; mp_index++) {
      const MPoly *p = &mr->mpoly[mp_index];
      for (int l = 0; l < p->totloop; l++) {
        float v_mask = 0.0f;
        if (cd_mask) {
          v_mask = cd_mask[loops[mp_loop].v];
        }
        vbo_data->mask = v_mask;

        uchar face_set_color[4] = {UCHAR_MAX, UCHAR_MAX, UCHAR_MAX, UCHAR_MAX};
        if (cd_face_set) {
          const int face_set_id = cd_face_set[mp_index];
          /* Skip for the default color Face Set to render it white. */
          if (face_set_id != mr->me->face_sets_color_default) {
            BKE_paint_face_set_overlay_color_get(
                face_set_id, mr->me->face_sets_color_seed, face_set_color);
          }
        }
        copy_v3_v3_uchar(vbo_data->face_set_color, face_set_color);
        mp_loop++;
        vbo_data++;
      }
    }
  }
}

static void extract_sculpt_data_init_subdiv(const DRWSubdivCache *subdiv_cache,
                                            const MeshRenderData *mr,
                                            MeshBatchCache *UNUSED(cache),
                                            void *buffer,
                                            void *UNUSED(data))
{
  GPUVertBuf *vbo = static_cast<GPUVertBuf *>(buffer);

  Mesh *coarse_mesh = mr->me;
  CustomData *cd_vdata = &coarse_mesh->vdata;
  CustomData *cd_pdata = &coarse_mesh->pdata;

  /* First, interpolate mask if available. */
  GPUVertBuf *mask_vbo = nullptr;
  GPUVertBuf *subdiv_mask_vbo = nullptr;
  const float *cd_mask = (const float *)CustomData_get_layer(cd_vdata, CD_PAINT_MASK);

  if (cd_mask) {
    GPUVertFormat mask_format = {0};
    GPU_vertformat_attr_add(&mask_format, "msk", GPU_COMP_F32, 1, GPU_FETCH_FLOAT);

    mask_vbo = GPU_vertbuf_calloc();
    GPU_vertbuf_init_with_format(mask_vbo, &mask_format);
    GPU_vertbuf_data_alloc(mask_vbo, coarse_mesh->totloop);
    float *v_mask = static_cast<float *>(GPU_vertbuf_get_data(mask_vbo));

    for (int i = 0; i < coarse_mesh->totpoly; i++) {
      const MPoly *mpoly = &coarse_mesh->mpoly[i];

      for (int loop_index = mpoly->loopstart; loop_index < mpoly->loopstart + mpoly->totloop;
           loop_index++) {
        const MLoop *ml = &coarse_mesh->mloop[loop_index];
        *v_mask++ = cd_mask[ml->v];
      }
    }

    subdiv_mask_vbo = GPU_vertbuf_calloc();
    GPU_vertbuf_init_build_on_device(
        subdiv_mask_vbo, &mask_format, subdiv_cache->num_subdiv_loops);

    draw_subdiv_interp_custom_data(subdiv_cache, mask_vbo, subdiv_mask_vbo, 1, 0, false);
  }

  /* Then, gather face sets. */
  GPUVertFormat face_set_format = {0};
  GPU_vertformat_attr_add(&face_set_format, "msk", GPU_COMP_U8, 4, GPU_FETCH_INT_TO_FLOAT_UNIT);

  GPUVertBuf *face_set_vbo = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format(face_set_vbo, &face_set_format);
  GPU_vertbuf_data_alloc(face_set_vbo, subdiv_cache->num_subdiv_loops);

  struct gpuFaceSet {
    uint8_t color[4];
  };

  gpuFaceSet *face_sets = (gpuFaceSet *)GPU_vertbuf_get_data(face_set_vbo);
  const int *cd_face_set = (const int *)CustomData_get_layer(cd_pdata, CD_SCULPT_FACE_SETS);

  GPUVertFormat *format = get_sculpt_data_format();
  GPU_vertbuf_init_build_on_device(vbo, format, subdiv_cache->num_subdiv_loops);
  int *subdiv_loop_poly_index = subdiv_cache->subdiv_loop_poly_index;

  for (uint i = 0; i < subdiv_cache->num_subdiv_loops; i++) {
    const int mp_index = subdiv_loop_poly_index[i];

    uchar face_set_color[4] = {UCHAR_MAX, UCHAR_MAX, UCHAR_MAX, UCHAR_MAX};
    if (cd_face_set) {
      const int face_set_id = cd_face_set[mp_index];
      /* Skip for the default color Face Set to render it white. */
      if (face_set_id != coarse_mesh->face_sets_color_default) {
        BKE_paint_face_set_overlay_color_get(
            face_set_id, coarse_mesh->face_sets_color_seed, face_set_color);
      }
    }
    copy_v3_v3_uchar(face_sets->color, face_set_color);
    face_sets++;
  }

  /* Finally, interleave mask and face sets. */
  draw_subdiv_build_sculpt_data_buffer(subdiv_cache, subdiv_mask_vbo, face_set_vbo, vbo);

  if (mask_vbo) {
    GPU_vertbuf_discard(mask_vbo);
    GPU_vertbuf_discard(subdiv_mask_vbo);
  }
  GPU_vertbuf_discard(face_set_vbo);
}

constexpr MeshExtract create_extractor_sculpt_data()
{
  MeshExtract extractor = {nullptr};
  extractor.init = extract_sculpt_data_init;
  extractor.init_subdiv = extract_sculpt_data_init_subdiv;
  extractor.data_type = MR_DATA_NONE;
  extractor.data_size = 0;
  extractor.use_threading = false;
  extractor.mesh_buffer_offset = offsetof(MeshBufferList, vbo.sculpt_data);
  return extractor;
}

/** \} */

}  // namespace blender::draw

const MeshExtract extract_sculpt_data = blender::draw::create_extractor_sculpt_data();
