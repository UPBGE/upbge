/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "BKE_paint.hh"
#include "DNA_volume_types.h"

#include "DRW_render.hh"
#include "draw_common.hh"
#include "draw_sculpt.hh"

#include "overlay_base.hh"
#include "overlay_mesh.hh"

namespace blender::draw::overlay {

/**
 * Draw wireframe of objects.
 *
 * The object wireframe can be drawn because of:
 * - display option (Object > Viewport Display > Wireframe)
 * - overlay option (Viewport Overlays > Geometry > Wireframe)
 * - display as (Object > Viewport Display > Wire)
 * - wireframe shading mode
 */
class Wireframe : Overlay {
 private:
  PassMain wireframe_ps_ = {"Wireframe"};
  struct ColoringPass {
    PassMain::Sub *curves_ps_ = nullptr;
    PassMain::Sub *mesh_ps_ = nullptr;
    PassMain::Sub *pointcloud_ps_ = nullptr;
    /* Variant for meshes that force drawing all edges. */
    PassMain::Sub *mesh_all_edges_ps_ = nullptr;
  } colored, non_colored;

  /* Copy of the depth buffer to be able to read it during wireframe rendering. */
  TextureFromPool tmp_depth_tx_ = {"tmp_depth_tx"};
  bool do_depth_copy_workaround_ = false;

  /* Force display of wireframe on surface objects, regardless of the object display settings. */
  bool show_wire_ = false;

 public:
  void begin_sync(Resources &res, const State &state) final
  {
    enabled_ = state.is_space_v3d() && (state.is_wireframe_mode || !state.hide_overlays);
    if (!enabled_) {
      return;
    }

    show_wire_ = state.is_wireframe_mode || state.show_wireframes();

    const bool is_selection = res.is_selection();
    const bool do_smooth_lines = (U.gpu_flag & USER_GPU_FLAG_OVERLAY_SMOOTH_WIRE) != 0;
    const bool is_transform = (G.moving & G_TRANSFORM_OBJ) != 0;
    const float wire_threshold = wire_discard_threshold_get(state.overlay.wireframe_threshold);

    gpu::Texture **depth_tex = (state.xray_enabled) ? &res.depth_tx : &tmp_depth_tx_;
    if (is_selection) {
      depth_tex = &res.dummy_depth_tx;
    }

    /* Note: Depth buffer has different format when doing selection. Avoid copy in this case. */
    do_depth_copy_workaround_ = !is_selection && (depth_tex == &tmp_depth_tx_);

    {
      auto &pass = wireframe_ps_;
      pass.init();
      pass.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
      pass.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
      pass.state_set(DRW_STATE_FIRST_VERTEX_CONVENTION | DRW_STATE_WRITE_COLOR |
                         DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL,
                     state.clipping_plane_count);
      res.select_bind(pass);

      auto shader_pass =
          [&](gpu::Shader *shader, const char *name, bool use_coloring, float wire_threshold) {
            auto &sub = pass.sub(name);
            if (res.shaders->wireframe_mesh.get() == shader) {
              sub.specialize_constant(shader, "use_custom_depth_bias", do_smooth_lines);
            }
            sub.shader_set(shader);
            sub.bind_texture("depth_tx", depth_tex);
            sub.push_constant("wire_opacity", state.overlay.wireframe_opacity);
            sub.push_constant("is_transform", is_transform);
            sub.push_constant("color_type", state.v3d->shading.wire_color_type);
            sub.push_constant("use_coloring", use_coloring);
            sub.push_constant("wire_step_param", wire_threshold);
            sub.push_constant("ndc_offset_factor", &state.ndc_offset_factor);
            sub.push_constant("is_hair", false);
            return &sub;
          };

      auto coloring_pass = [&](ColoringPass &ps, bool use_color) {
        overlay::ShaderModule &sh = *res.shaders;
        ps.mesh_ps_ = shader_pass(sh.wireframe_mesh.get(), "Mesh", use_color, wire_threshold);
        ps.mesh_all_edges_ps_ = shader_pass(sh.wireframe_mesh.get(), "Wire", use_color, 1.0f);
        ps.pointcloud_ps_ = shader_pass(sh.wireframe_points.get(), "PtCloud", use_color, 1.0f);
        ps.curves_ps_ = shader_pass(sh.wireframe_curve.get(), "Curve", use_color, 1.0f);
      };

      coloring_pass(non_colored, false);
      coloring_pass(colored, true);
    }
  }

  void object_sync_ex(Manager &manager,
                      const ObjectRef &ob_ref,
                      Resources &res,
                      const State &state,
                      const bool in_edit_paint_mode,
                      const bool in_edit_mode)
  {
    if (!enabled_) {
      return;
    }

    if (ob_ref.object->dt < OB_WIRE) {
      return;
    }

    const bool all_edges = (ob_ref.object->dtx & OB_DRAW_ALL_EDGES) != 0;
    const bool show_surface_wire = show_wire_ || (ob_ref.object->dtx & OB_DRAWWIRE) ||
                                   (ob_ref.object->dt == OB_WIRE);

    ColoringPass &coloring = in_edit_paint_mode ? non_colored : colored;
    switch (ob_ref.object->type) {
      case OB_CURVES_LEGACY: {
        gpu::Batch *geom = DRW_cache_curve_edge_wire_get(ob_ref.object);
        coloring.curves_ps_->draw(
            geom, manager.unique_handle(ob_ref), res.select_id(ob_ref).get());
        break;
      }
      case OB_FONT: {
        gpu::Batch *geom = DRW_cache_text_edge_wire_get(ob_ref.object);
        coloring.curves_ps_->draw(
            geom, manager.unique_handle(ob_ref), res.select_id(ob_ref).get());
        break;
      }
      case OB_SURF: {
        gpu::Batch *geom = DRW_cache_surf_edge_wire_get(ob_ref.object);
        coloring.curves_ps_->draw(
            geom, manager.unique_handle(ob_ref), res.select_id(ob_ref).get());
        break;
      }
      case OB_CURVES:
        /* TODO(fclem): Not yet implemented. */
        break;
      case OB_GREASE_PENCIL: {
        if (show_surface_wire) {
          gpu::Batch *geom = DRW_cache_grease_pencil_face_wireframe_get(state.scene,
                                                                        ob_ref.object);
          coloring.curves_ps_->draw(
              geom, manager.unique_handle(ob_ref), res.select_id(ob_ref).get());
        }
        break;
      }
      case OB_MESH: {
        /* Force display in edit mode when overlay is off in wireframe mode (see #78484). */
        const bool wireframe_no_overlay = state.hide_overlays && state.is_wireframe_mode;

        /* In some cases the edit mode wireframe overlay is already drawn for the same edges.
         * We want to avoid this redundant work and avoid Z-fighting, but detecting this case is
         * relatively complicated. Whether edit mode draws edges on the evaluated mesh depends on
         * whether there is a separate cage and whether there is a valid mapping between the
         * evaluated and original edit mesh. */
        const bool edit_wires_overlap_all = mesh_edit_wires_overlap(ob_ref, in_edit_mode);

        const bool bypass_mode_check = wireframe_no_overlay || !edit_wires_overlap_all;

        if (show_surface_wire) {
          if (BKE_sculptsession_use_pbvh_draw(ob_ref.object, state.rv3d)) {
            ResourceHandleRange handle = manager.unique_handle(ob_ref);

            for (SculptBatch &batch : sculpt_batches_get(ob_ref.object, SCULPT_BATCH_WIREFRAME)) {
              coloring.mesh_all_edges_ps_->draw(batch.batch, handle);
            }
          }
          else if (!in_edit_mode || bypass_mode_check) {
            /* Only draw the wireframe in edit mode if object has edit cage.
             * Otherwise the wireframe will conflict with the edit cage drawing and produce
             * unpleasant aliasing. */
            gpu::Batch *geom = DRW_cache_mesh_face_wireframe_get(ob_ref.object);
            (all_edges ? coloring.mesh_all_edges_ps_ : coloring.mesh_ps_)
                ->draw(geom, manager.unique_handle(ob_ref), res.select_id(ob_ref).get());
          }
        }

        /* Draw loose geometry. */
        if (!in_edit_paint_mode || bypass_mode_check) {
          const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(*ob_ref.object);
          gpu::Batch *geom;
          if ((mesh.edges_num == 0) && (mesh.verts_num > 0)) {
            geom = DRW_cache_mesh_all_verts_get(ob_ref.object);
            coloring.pointcloud_ps_->draw(
                geom, manager.unique_handle(ob_ref), res.select_id(ob_ref).get());
          }
          else if ((geom = DRW_cache_mesh_loose_edges_get(ob_ref.object))) {
            coloring.mesh_all_edges_ps_->draw(
                geom, manager.unique_handle(ob_ref), res.select_id(ob_ref).get());
          }
        }
        break;
      }
      case OB_POINTCLOUD: {
        if (show_surface_wire) {
          gpu::Batch *geom = DRW_pointcloud_batch_cache_get_dots(ob_ref.object);
          coloring.pointcloud_ps_->draw(
              geom, manager.unique_handle(ob_ref), res.select_id(ob_ref).get());
        }
        break;
      }
      case OB_VOLUME: {
        if (show_surface_wire) {
          gpu::Batch *geom = DRW_cache_volume_face_wireframe_get(ob_ref.object);
          if (geom == nullptr) {
            break;
          }
          if (DRW_object_get_data_for_drawing<Volume>(*ob_ref.object).display.wireframe_type ==
              VOLUME_WIREFRAME_POINTS)
          {
            coloring.pointcloud_ps_->draw(
                geom, manager.unique_handle(ob_ref), res.select_id(ob_ref).get());
          }
          else {
            coloring.mesh_ps_->draw(
                geom, manager.unique_handle(ob_ref), res.select_id(ob_ref).get());
          }
        }
        break;
      }
      default:
        /* Would be good to have. */
        // BLI_assert_unreachable();
        break;
    }
  }

  void pre_draw(Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }

    manager.generate_commands(wireframe_ps_, view);
  }

  void copy_depth(TextureRef &depth_tx)
  {
    if (!enabled_ || !do_depth_copy_workaround_) {
      return;
    }

    eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT;
    int2 render_size = int2(depth_tx.size());
    tmp_depth_tx_.acquire(render_size, gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8, usage);

    /* WORKAROUND: Nasty framebuffer copy.
     * We should find a way to have nice wireframe without this. */
    GPU_texture_copy(tmp_depth_tx_, depth_tx);
  }

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }

    GPU_framebuffer_bind(framebuffer);
    manager.submit_only(wireframe_ps_, view);

    tmp_depth_tx_.release();
  }

 private:
  float wire_discard_threshold_get(float threshold)
  {
    /* Use `sqrt` since the value stored in the edge is a variation of the cosine, so its square
     * becomes more proportional with a variation of angle. */
    threshold = sqrt(abs(threshold));
    /* The maximum value (255 in the VBO) is used to force hide the edge. */
    return math::interpolate(0.0f, 1.0f - (1.0f / 255.0f), threshold);
  }

  static bool mesh_edit_wires_overlap(const ObjectRef &ob_ref, const bool in_edit_mode)
  {
    if (!in_edit_mode) {
      return false;
    }
    const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(*ob_ref.object);
    const Mesh *orig_edit_mesh = BKE_object_get_pre_modified_mesh(ob_ref.object);
    const bool edit_mapping_valid = BKE_editmesh_eval_orig_map_available(mesh, orig_edit_mesh);
    if (!edit_mapping_valid) {
      /* The mesh edit mode overlay doesn't include wireframe for the evaluated mesh when it
       * doesn't correspond with the original edit mesh. So the main wireframe overlay should draw
       * wires for the evaluated mesh instead. */
      return false;
    }
    if (Meshes::mesh_has_edit_cage(ob_ref.object)) {
      /* If a cage exists, the edit overlay might not display every edge. */
      return false;
    }
    /* The edit mode overlay displays all of the edges of the evaluated mesh; drawing the edges
     * again would be redundant. */
    return true;
  }
};

}  // namespace blender::draw::overlay
