/* SPDX-FileCopyrightText: 2016 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

/* Private functions / structs of the draw manager */

#pragma once

#include <unordered_set>
#include <unordered_map>
#include <vector>

#include "DRW_engine.hh"
#include "DRW_render.hh"

#include "BLI_task.h"
#include "BLI_threads.h"
#include "BLI_vector_set.hh"

#include "GPU_batch.hh"
#include "GPU_context.hh"
#include "GPU_framebuffer.hh"
#include "GPU_viewport.hh"

#include "draw_modifier_gpu_pipeline.hh"

struct DRWDebugModule;
struct DRWUniformChunk;
struct DRWViewData;
struct DRWTextStore;
struct DupliObject;
struct Object;
struct Mesh;
struct ModifierData;
namespace blender::draw {
struct CurvesModule;
struct VolumeModule;
struct PointCloudModule;
struct DRW_MeshCDMask;
class CurveRefinePass;
class View;
}  // namespace blender::draw
struct GPUMaterial;

/* -------------------------------------------------------------------- */
/** \name Memory Pools
 * \{ */

/* Decision / reason for GPU playback refusal (local enum). */
enum class PlaybackRefuseReason : uint8_t {
  None = 0,
  TopologyMismatch,
  TopologyModifier,
  MixedCPUAndGPU,
  Unknown,
};

/* Refusal info stored in GPUPlaybackDecision. */
struct PlayBackRefuseInfo {
  PlaybackRefuseReason reason = PlaybackRefuseReason::None;
  bool python_requests_gpu = false;
  bool key_requests_gpu = false;
  bool modifier_requests_gpu = false;
  bool allow_gpu = false; /* final decision */
  struct Object *ob =
      nullptr; /* not optional: object for this modifier */
  struct ModifierData *refusal_modifier = nullptr; /* optional: specific modifier that caused refusal */
  std::vector<ModifierData *> modifiers_gpu = {};
  std::vector<ModifierData *> modifiers_cpu = {};
};


/** Set of Mesh* scheduled to free GPU resources from non-GL contexts. */
struct MeshProcessEntry {
  struct Object *eval_obj_for_skinning = nullptr;
  bool scheduled_free = false;
  /* GPU modifier pipeline (persistent across frames to preserve pipeline_hash_) */
  std::unique_ptr<blender::draw::GPUModifierPipeline> gpu_pipeline = nullptr;
};

/** Contains memory pools information. */
struct DRWData {
  /** Instance data. */
  DRWInstanceDataList *idatalist;
  /** List of smoke textures to free after drawing. */
  ListBase smoke_textures;
  /** Per stereo view data. Contains engine data and default frame-buffers. */
  DRWViewData *view_data[2];
  /** Module storage. */
  blender::draw::CurvesModule *curves_module;
  blender::draw::VolumeModule *volume_module;
  blender::draw::PointCloudModule *pointcloud_module;
  /** Default view that feeds every engine. */
  blender::draw::View *default_view;

  /** Map of original Mesh* -> processing info. Contains both scheduled free flag and optional
   * evaluated object used for GPU skinning. This replaces the previous two containers. */
  std::unordered_map<struct Mesh *, MeshProcessEntry> *meshes_to_process;

  /* Ensure modules are created. */
  void modules_init();
  /* Callbacks before each sync cycle. */
  void modules_begin_sync();
  /* Callbacks after one draw to clear transient data. */
  void modules_exit();
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Functions
 * \{ */

/* Get thread local draw context. */
inline DRWContext &drw_get()
{
  return DRWContext::get_active();
}

namespace blender::draw {

void drw_batch_cache_validate(Object *ob);
void drw_batch_cache_generate_requested(Object *ob, TaskGraph &task_graph);

/**
 * \warning Only evaluated mesh data is handled by this delayed generation.
 */
void drw_batch_cache_generate_requested_delayed(Object *ob);
void drw_batch_cache_generate_requested_evaluated_mesh_or_curve(Object *ob, TaskGraph &task_graph);

void DRW_mesh_get_attributes(const Object &object,
                             const Mesh &mesh,
                             Span<const GPUMaterial *> materials,
                             VectorSet<std::string> *r_attrs,
                             DRW_MeshCDMask *r_cd_needed);

}  // namespace blender::draw

/** \} */
