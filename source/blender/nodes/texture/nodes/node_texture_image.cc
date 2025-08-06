/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup texnodes
 */

#include "BLI_math_vector.h"
#include "BLI_threads.h"

#include "IMB_imbuf.hh"

#include "BKE_image.hh"

#include "node_texture_util.hh"
#include "node_util.hh"

static blender::bke::bNodeSocketTemplate outputs[] = {
    {SOCK_RGBA, N_("Image")},
    {-1, ""},
};

static void colorfn(float *out, TexParams *p, bNode *node, bNodeStack ** /*in*/, short /*thread*/)
{
  float x = p->co[0];
  float y = p->co[1];
  Image *ima = (Image *)node->id;
  ImageUser *iuser = (ImageUser *)node->storage;

  if (ima) {
    ImBuf *ibuf = BKE_image_acquire_ibuf(ima, iuser, nullptr);
    if (ibuf) {
      float xsize, ysize;
      float xoff, yoff;
      int px, py;

      const float *result;

      xsize = ibuf->x / 2;
      ysize = ibuf->y / 2;
      xoff = yoff = -1;

      px = int((x - xoff) * xsize);
      py = int((y - yoff) * ysize);

      if ((!xsize) || (!ysize)) {
        return;
      }

      if (!ibuf->float_buffer.data) {
        BLI_thread_lock(LOCK_IMAGE);
        if (!ibuf->float_buffer.data) {
          IMB_float_from_byte(ibuf);
        }
        BLI_thread_unlock(LOCK_IMAGE);
      }

      while (px < 0) {
        px += ibuf->x;
      }
      while (py < 0) {
        py += ibuf->y;
      }
      while (px >= ibuf->x) {
        px -= ibuf->x;
      }
      while (py >= ibuf->y) {
        py -= ibuf->y;
      }

      result = ibuf->float_buffer.data + py * ibuf->x * 4 + px * 4;
      copy_v4_v4(out, result);

      BKE_image_release_ibuf(ima, ibuf, nullptr);
    }
  }
}

static void exec(void *data,
                 int /*thread*/,
                 bNode *node,
                 bNodeExecData *execdata,
                 bNodeStack **in,
                 bNodeStack **out)
{
  tex_output(node, execdata, in, out[0], &colorfn, static_cast<TexCallData *>(data));
}

static void init(bNodeTree * /*ntree*/, bNode *node)
{
  ImageUser *iuser = MEM_callocN<ImageUser>("node image user");
  node->storage = iuser;
  iuser->sfra = 1;
  iuser->flag |= IMA_ANIM_ALWAYS;
}

void register_node_type_tex_image()
{
  static blender::bke::bNodeType ntype;

  tex_node_type_base(&ntype, "TextureNodeImage", TEX_NODE_IMAGE);
  ntype.ui_name = "Image";
  ntype.enum_name_legacy = "IMAGE";
  ntype.nclass = NODE_CLASS_INPUT;
  blender::bke::node_type_socket_templates(&ntype, nullptr, outputs);
  ntype.initfunc = init;
  blender::bke::node_type_storage(
      ntype, "ImageUser", node_free_standard_storage, node_copy_standard_storage);
  ntype.exec_fn = exec;
  ntype.labelfunc = node_image_label;
  ntype.flag |= NODE_PREVIEW;

  blender::bke::node_register_type(ntype);
}
