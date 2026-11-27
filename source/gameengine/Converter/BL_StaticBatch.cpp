/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file gameengine/Converter/BL_StaticBatch.cpp
 *  \ingroup bgeconv
 */

#include "BL_StaticBatch.h"

#include <cmath>
#include <map>
#include <string>
#include <tuple>

#include "BKE_collection.hh"
#include "BKE_geometry_set.hh"
#include "BKE_instances.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_mesh.h"
#include "BKE_object.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_matrix.hh"
#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_layer_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "GEO_realize_instances.hh"
#include "MEM_guardedalloc.h"

#include "CM_Message.h"

using namespace blender;

namespace {

/* Objects of one batch: same materials, same cell. */
using BatchKey = std::tuple<std::vector<Material *>, int, int, int>;

bool is_batchable(const Object *ob)
{
  return ob->type == OB_MESH && (ob->gameflag2 & OB_STATIC_BATCH) &&
         !(ob->gameflag & OB_DUPLI_UPBGE) && !(ob->visibility_flag & OB_HIDE_RENDER) &&
         ob->instance_collection == nullptr;
}

}  // namespace

BL_StaticBatchResult BL_BuildStaticBatches(Main *bmain, Scene *scene, Depsgraph *depsgraph)
{
  BL_StaticBatchResult result;
  ViewLayer *view_layer = BKE_view_layer_default_view(scene);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);

  const float cell = scene->eevee.game_batch_cell_size;
  std::map<BatchKey, std::vector<Object *>> groups;
  for (Base &base : *BKE_view_layer_object_bases_get(view_layer)) {
    Object *ob = base.object;
    if (!(base.flag & BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT) || !is_batchable(ob)) {
      continue;
    }
    Object *ob_eval = DEG_get_evaluated(depsgraph, ob);
    if (!ob_eval || ob_eval == ob || !BKE_object_get_evaluated_mesh(ob_eval)) {
      continue;
    }
    std::vector<Material *> materials;
    const int material_len = BKE_object_material_count_eval(ob_eval);
    for (int i = 0; i < material_len; i++) {
      /* The batch mesh lives in Main: original materials, not their evaluated copies. */
      Material *mat = BKE_object_material_get_eval(ob_eval, short(i + 1));
      materials.push_back(mat ? DEG_get_original(mat) : nullptr);
    }
    int3 cell_index(0);
    if (cell > 0.0f) {
      /* The cell of the bounds center, so an object belongs to one cell only. */
      float3 center = ob_eval->object_to_world().location();
      if (const std::optional<Bounds<float3>> bounds = BKE_object_boundbox_eval_cached_get(ob_eval))
      {
        center = math::transform_point(ob_eval->object_to_world(),
                                       math::midpoint(bounds->min, bounds->max));
      }
      for (int k = 0; k < 3; k++) {
        cell_index[k] = int(std::floor(center[k] / cell));
      }
    }
    groups[BatchKey(std::move(materials), cell_index.x, cell_index.y, cell_index.z)].push_back(
        ob);
  }

  int batch_index = 0;
  for (auto &[key, objects] : groups) {
    if (objects.size() < 2) {
      continue;
    }
    const std::vector<Material *> &materials = std::get<0>(key);

    /* One instance per object: its evaluated mesh, with the object's own materials (they can be
     * linked to the object rather than the mesh), at its world transform. realize_instances()
     * merges the material lists and remaps the material indices. */
    auto instances = std::make_unique<bke::Instances>();
    instances->resize(int(objects.size()));
    MutableSpan<int> handles = instances->reference_handles_for_write();
    MutableSpan<float4x4> transforms = instances->transforms_for_write();
    for (int i = 0; i < int(objects.size()); i++) {
      const Object *ob_eval = DEG_get_evaluated(depsgraph, objects[i]);
      Mesh *mesh = BKE_mesh_copy_for_eval(*BKE_object_get_evaluated_mesh(ob_eval));
      MEM_SAFE_DELETE(mesh->mat);
      mesh->totcol = short(materials.size());
      mesh->mat = MEM_new_array_zeroed<Material *>(materials.size() + 1, __func__);
      for (size_t m = 0; m < materials.size(); m++) {
        mesh->mat[m] = materials[m];
      }
      handles[i] = instances->add_new_reference(
          bke::InstanceReference(bke::GeometrySet::from_mesh(mesh)));
      transforms[i] = ob_eval->object_to_world();
    }

    geometry::RealizeInstancesOptions options;
    options.keep_original_ids = true;
    options.realize_instance_attributes = false;
    bke::GeometrySet realized = geometry::realize_instances(
                                    bke::GeometrySet::from_instances(std::move(instances)),
                                    options)
                                    .geometry;
    Mesh *merged = realized.get_component_for_write<bke::MeshComponent>().release();
    if (!merged) {
      continue;
    }

    const std::string name = "StaticBatch." + std::to_string(batch_index++);
    Mesh *mesh = BKE_mesh_add(bmain, name.c_str());
    Vector<Material *> merged_materials(Span<Material *>(merged->mat, merged->totcol));
    BKE_mesh_nomain_to_mesh(merged, mesh, nullptr);

    Object *ob = BKE_object_add_only_object(bmain, OB_MESH, name.c_str());
    ob->data = &mesh->id;
    /* Drawn only: the merged objects keep their own physics and logic. */
    ob->body_type = OB_BODY_TYPE_NO_COLLISION;
    ob->gameflag &= ~OB_COLLISION;
    Material **material_array = merged_materials.data();
    BKE_object_material_array_assign(
        bmain, ob, &material_array, int(merged_materials.size()), false);
    BKE_collection_object_add(bmain, scene->master_collection, ob);

    result.batches.push_back(ob);
    result.merged.insert(result.merged.end(), objects.begin(), objects.end());
  }

  if (!result.batches.empty()) {
    BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
    DEG_relations_tag_update(bmain);
    CM_Message("Static batching: " << result.merged.size() << " objects merged into "
                                   << result.batches.size() << " batches");
  }
  return result;
}

void BL_FreeStaticBatches(Main *bmain, Scene *scene, std::vector<Object *> &batches)
{
  for (Object *ob : batches) {
    ID *mesh = static_cast<ID *>(ob->data);
    BKE_collection_object_remove(bmain, scene->master_collection, ob, false);
    BKE_id_free(bmain, ob);
    if (mesh) {
      BKE_id_free(bmain, mesh);
    }
  }
  batches.clear();
  DEG_relations_tag_update(bmain);
}
