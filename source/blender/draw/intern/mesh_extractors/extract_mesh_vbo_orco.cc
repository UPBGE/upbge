/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup draw
 */

#include "extract_mesh.hh"

namespace blender::draw {

/* ---------------------------------------------------------------------- */
/** \name Extract Orco
 * \{ */

struct MeshExtract_Orco_Data {
  float (*vbo_data)[4];
  const float (*orco)[3];
};

static void extract_orco_init(const MeshRenderData *mr,
                              MeshBatchCache *UNUSED(cache),
                              void *buf,
                              void *tls_data)
{
  GPUVertBuf *vbo = static_cast<GPUVertBuf *>(buf);
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    /* FIXME(fclem): We use the last component as a way to differentiate from generic vertex
     * attributes. This is a substantial waste of video-ram and should be done another way.
     * Unfortunately, at the time of writing, I did not found any other "non disruptive"
     * alternative. */
    GPU_vertformat_attr_add(&format, "orco", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);
  }

  GPU_vertbuf_init_with_format(vbo, &format);
  GPU_vertbuf_data_alloc(vbo, mr->loop_len);

  CustomData *cd_vdata = &mr->me->vdata;

  MeshExtract_Orco_Data *data = static_cast<MeshExtract_Orco_Data *>(tls_data);
  data->vbo_data = (float(*)[4])GPU_vertbuf_get_data(vbo);
  data->orco = static_cast<const float(*)[3]>(CustomData_get_layer(cd_vdata, CD_ORCO));
  /* Make sure `orco` layer was requested only if needed! */
  BLI_assert(data->orco);
}

static void extract_orco_iter_poly_bm(const MeshRenderData *UNUSED(mr),
                                      const BMFace *f,
                                      const int UNUSED(f_index),
                                      void *data)
{
  MeshExtract_Orco_Data *orco_data = (MeshExtract_Orco_Data *)data;
  BMLoop *l_iter, *l_first;
  l_iter = l_first = BM_FACE_FIRST_LOOP(f);
  do {
    const int l_index = BM_elem_index_get(l_iter);
    float *loop_orco = orco_data->vbo_data[l_index];
    copy_v3_v3(loop_orco, orco_data->orco[BM_elem_index_get(l_iter->v)]);
    loop_orco[3] = 0.0; /* Tag as not a generic attribute. */
  } while ((l_iter = l_iter->next) != l_first);
}

static void extract_orco_iter_poly_mesh(const MeshRenderData *mr,
                                        const MPoly *mp,
                                        const int UNUSED(mp_index),
                                        void *data)
{
  const MLoop *mloop = mr->mloop;
  const int ml_index_end = mp->loopstart + mp->totloop;
  for (int ml_index = mp->loopstart; ml_index < ml_index_end; ml_index += 1) {
    const MLoop *ml = &mloop[ml_index];
    MeshExtract_Orco_Data *orco_data = (MeshExtract_Orco_Data *)data;
    float *loop_orco = orco_data->vbo_data[ml_index];
    copy_v3_v3(loop_orco, orco_data->orco[ml->v]);
    loop_orco[3] = 0.0; /* Tag as not a generic attribute. */
  }
}

constexpr MeshExtract create_extractor_orco()
{
  MeshExtract extractor = {nullptr};
  extractor.init = extract_orco_init;
  extractor.iter_poly_bm = extract_orco_iter_poly_bm;
  extractor.iter_poly_mesh = extract_orco_iter_poly_mesh;
  extractor.data_type = MR_DATA_NONE;
  extractor.data_size = sizeof(MeshExtract_Orco_Data);
  extractor.use_threading = true;
  extractor.mesh_buffer_offset = offsetof(MeshBufferList, vbo.orco);
  return extractor;
}

/** \} */

}  // namespace blender::draw

const MeshExtract extract_orco = blender::draw::create_extractor_orco();
