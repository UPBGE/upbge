/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "DEG_depsgraph_query.h"
#include "node_geometry_util.hh"

#include "BKE_lib_id.h"
#include "BKE_mesh.h"
#include "BKE_mesh_runtime.h"
#include "BKE_mesh_wrapper.h"
#include "BKE_object.h"
#include "BKE_volume.h"

#include "GEO_mesh_to_volume.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "UI_interface.h"
#include "UI_resources.h"

namespace blender::nodes::node_geo_mesh_to_volume_cc {

NODE_STORAGE_FUNCS(NodeGeometryMeshToVolume)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>(N_("Mesh")).supported_type(GEO_COMPONENT_TYPE_MESH);
  b.add_input<decl::Float>(N_("Density")).default_value(1.0f).min(0.01f).max(FLT_MAX);
  b.add_input<decl::Float>(N_("Voxel Size"))
      .default_value(0.3f)
      .min(0.01f)
      .max(FLT_MAX)
      .subtype(PROP_DISTANCE);
  b.add_input<decl::Float>(N_("Voxel Amount")).default_value(64.0f).min(0.0f).max(FLT_MAX);
  b.add_input<decl::Float>(N_("Exterior Band Width"))
      .default_value(0.1f)
      .min(0.0f)
      .max(FLT_MAX)
      .subtype(PROP_DISTANCE)
      .description(N_("Width of the volume outside of the mesh"));
  b.add_input<decl::Float>(N_("Interior Band Width"))
      .default_value(0.0f)
      .min(0.0f)
      .max(FLT_MAX)
      .subtype(PROP_DISTANCE)
      .description(N_("Width of the volume inside of the mesh"));
  b.add_input<decl::Bool>(N_("Fill Volume"))
      .default_value(true)
      .description(N_("Initialize the density grid in every cell inside the enclosed volume"));
  b.add_output<decl::Geometry>(N_("Volume"));
}

static void node_layout(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);
  uiItemR(layout, ptr, "resolution_mode", 0, IFACE_("Resolution"), ICON_NONE);
}

static void node_init(bNodeTree *UNUSED(tree), bNode *node)
{
  NodeGeometryMeshToVolume *data = (NodeGeometryMeshToVolume *)MEM_callocN(
      sizeof(NodeGeometryMeshToVolume), __func__);
  data->resolution_mode = MESH_TO_VOLUME_RESOLUTION_MODE_VOXEL_AMOUNT;
  node->storage = data;
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  NodeGeometryMeshToVolume *data = (NodeGeometryMeshToVolume *)node->storage;

  bNodeSocket *voxel_size_socket = nodeFindSocket(node, SOCK_IN, "Voxel Size");
  bNodeSocket *voxel_amount_socket = nodeFindSocket(node, SOCK_IN, "Voxel Amount");
  nodeSetSocketAvailability(ntree,
                            voxel_amount_socket,
                            data->resolution_mode == MESH_TO_VOLUME_RESOLUTION_MODE_VOXEL_AMOUNT);
  nodeSetSocketAvailability(ntree,
                            voxel_size_socket,
                            data->resolution_mode == MESH_TO_VOLUME_RESOLUTION_MODE_VOXEL_SIZE);
}

#ifdef WITH_OPENVDB

static Volume *create_volume_from_mesh(const Mesh &mesh, GeoNodeExecParams &params)
{
  const NodeGeometryMeshToVolume &storage =
      *(const NodeGeometryMeshToVolume *)params.node().storage;

  const float density = params.get_input<float>("Density");
  const float exterior_band_width = params.get_input<float>("Exterior Band Width");
  const float interior_band_width = params.get_input<float>("Interior Band Width");
  const bool fill_volume = params.get_input<bool>("Fill Volume");

  geometry::MeshToVolumeResolution resolution;
  resolution.mode = (MeshToVolumeModifierResolutionMode)storage.resolution_mode;
  if (resolution.mode == MESH_TO_VOLUME_RESOLUTION_MODE_VOXEL_AMOUNT) {
    resolution.settings.voxel_amount = params.get_input<float>("Voxel Amount");
    if (resolution.settings.voxel_amount <= 0.0f) {
      return nullptr;
    }
  }
  else if (resolution.mode == MESH_TO_VOLUME_RESOLUTION_MODE_VOXEL_SIZE) {
    resolution.settings.voxel_size = params.get_input<float>("Voxel Size");
    if (resolution.settings.voxel_size <= 0.0f) {
      return nullptr;
    }
  }

  if (mesh.totvert == 0 || mesh.totpoly == 0) {
    return nullptr;
  }

  const float4x4 mesh_to_volume_space_transform = float4x4::identity();

  auto bounds_fn = [&](float3 &r_min, float3 &r_max) {
    float3 min{std::numeric_limits<float>::max()};
    float3 max{-std::numeric_limits<float>::max()};
    BKE_mesh_wrapper_minmax(&mesh, min, max);
    r_min = min;
    r_max = max;
  };

  const float voxel_size = geometry::volume_compute_voxel_size(params.depsgraph(),
                                                               bounds_fn,
                                                               resolution,
                                                               exterior_band_width,
                                                               mesh_to_volume_space_transform);

  Volume *volume = (Volume *)BKE_id_new_nomain(ID_VO, nullptr);
  BKE_volume_init_grids(volume);

  /* Convert mesh to grid and add to volume. */
  geometry::volume_grid_add_from_mesh(volume,
                                      "density",
                                      &mesh,
                                      mesh_to_volume_space_transform,
                                      voxel_size,
                                      fill_volume,
                                      exterior_band_width,
                                      interior_band_width,
                                      density);

  return volume;
}

#endif /* WITH_OPENVDB */

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
  GeometrySet geometry_set(params.extract_input<GeometrySet>("Mesh"));

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    if (geometry_set.has_mesh()) {
      Volume *volume = create_volume_from_mesh(*geometry_set.get_mesh_for_read(), params);
      geometry_set.replace_volume(volume);
      geometry_set.keep_only_during_modify({GEO_COMPONENT_TYPE_VOLUME});
    }
  });
  params.set_output("Volume", std::move(geometry_set));
#else
  params.error_message_add(NodeWarningType::Error,
                           TIP_("Disabled, Blender was compiled without OpenVDB"));
  params.set_default_remaining_outputs();
  return;
#endif
}

}  // namespace blender::nodes::node_geo_mesh_to_volume_cc

void register_node_type_geo_mesh_to_volume()
{
  namespace file_ns = blender::nodes::node_geo_mesh_to_volume_cc;

  static bNodeType ntype;

  geo_node_type_base(&ntype, GEO_NODE_MESH_TO_VOLUME, "Mesh to Volume", NODE_CLASS_GEOMETRY);
  ntype.declare = file_ns::node_declare;
  node_type_size(&ntype, 200, 120, 700);
  node_type_init(&ntype, file_ns::node_init);
  node_type_update(&ntype, file_ns::node_update);
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  ntype.draw_buttons = file_ns::node_layout;
  node_type_storage(
      &ntype, "NodeGeometryMeshToVolume", node_free_standard_storage, node_copy_standard_storage);
  nodeRegisterType(&ntype);
}
