/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"
#include "node_util.hh"

#include "BKE_context.hh"

#include "DNA_customdata_types.h"

#include "DEG_depsgraph_query.hh"

#include "RNA_access.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_shader_uvmap_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Vector>("UV");
}

static void node_shader_buts_uvmap(uiLayout *layout, bContext *C, PointerRNA *ptr)
{
  layout->prop(ptr, "from_instancer", UI_ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);

  if (!RNA_boolean_get(ptr, "from_instancer")) {
    PointerRNA obptr = CTX_data_pointer_get(C, "active_object");
    Object *object = static_cast<Object *>(obptr.data);

    if (object && object->type == OB_MESH) {
      Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);

      if (depsgraph) {
        Object *object_eval = DEG_get_evaluated(depsgraph, object);
        PointerRNA dataptr = RNA_id_pointer_create(static_cast<ID *>(object_eval->data));
        layout->prop_search(ptr, "uv_map", &dataptr, "uv_layers", "", ICON_GROUP_UVS);
        return;
      }
    }

    layout->prop(ptr, "uv_map", UI_ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_GROUP_UVS);
  }
}

static void node_shader_init_uvmap(bNodeTree * /*ntree*/, bNode *node)
{
  NodeShaderUVMap *attr = MEM_callocN<NodeShaderUVMap>("NodeShaderUVMap");
  node->storage = attr;
}

static int node_shader_gpu_uvmap(GPUMaterial *mat,
                                 bNode *node,
                                 bNodeExecData * /*execdata*/,
                                 GPUNodeStack *in,
                                 GPUNodeStack *out)
{
  NodeShaderUVMap *attr = static_cast<NodeShaderUVMap *>(node->storage);

  /* NOTE: using CD_AUTO_FROM_NAME instead of CD_MTFACE as geometry nodes may overwrite data which
   * will also change the eCustomDataType. This will also make EEVEE and Cycles consistent. See
   * #93179. */
  GPUNodeLink *mtface = GPU_attribute(mat, CD_AUTO_FROM_NAME, attr->uv_map);

  GPU_stack_link(mat, node, "node_uvmap", in, out, mtface);

  node_shader_gpu_bump_tex_coord(mat, node, &out[0].link);

  return 1;
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  /* NODE: "From Instances" not implemented */
  NodeShaderUVMap *attr = static_cast<NodeShaderUVMap *>(node_->storage);
  NodeItem res = texcoord_node(NodeItem::Type::Vector2, attr->uv_map);
  return res;
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace blender::nodes::node_shader_uvmap_cc

/* node type definition */
void register_node_type_sh_uvmap()
{
  namespace file_ns = blender::nodes::node_shader_uvmap_cc;

  static blender::bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeUVMap", SH_NODE_UVMAP);
  ntype.ui_name = "UV Map";
  ntype.ui_description =
      "Retrieve a UV map from the geometry, or the default fallback if none is specified";
  ntype.enum_name_legacy = "UVMAP";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts_uvmap;
  blender::bke::node_type_size_preset(ntype, blender::bke::eNodeSizePreset::Middle);
  ntype.initfunc = file_ns::node_shader_init_uvmap;
  blender::bke::node_type_storage(
      ntype, "NodeShaderUVMap", node_free_standard_storage, node_copy_standard_storage);
  ntype.gpu_fn = file_ns::node_shader_gpu_uvmap;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  blender::bke::node_register_type(ntype);
}
