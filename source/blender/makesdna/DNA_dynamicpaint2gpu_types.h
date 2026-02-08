/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 *
 * GPU brush DNA for dynamic paint 2.
 *
 * This modifier is GPU-only. Each brush owns its own raycast settings
 * (origin, target, direction, length), a radius, intensity, and an
 * optional procedural texture to modulate intensity.
 * Vertex group masking is done implicitly via the GPU raycast radius.
 */

#pragma once

#include "DNA_listBase.h"

namespace blender {

struct Object;
struct Tex;
struct CurveMapping;
struct DynamicPaint2GpuModifierData;
struct DynamicPaint2GpuCanvasSettings;
struct Collection;

/* Falloff type for GPU brush (reuses same values as eHook_Falloff for consistency). */
enum eDynPaint2Gpu_Falloff {
  DP2GPU_FALLOFF_NONE = 0,
  DP2GPU_FALLOFF_CURVE = 1,
  DP2GPU_FALLOFF_SHARP = 2,
  DP2GPU_FALLOFF_SMOOTH = 3,
  DP2GPU_FALLOFF_ROOT = 4,
  DP2GPU_FALLOFF_LINEAR = 5,
  DP2GPU_FALLOFF_CONST = 6,
  DP2GPU_FALLOFF_SPHERE = 7,
  DP2GPU_FALLOFF_INVSQUARE = 8,
};

/* High-level direction type: axis-based or object-based. */
enum eDynPaint2Gpu_DirectionType {
  DP2GPU_DIRTYPE_AXIS = 0,
  DP2GPU_DIRTYPE_OBJECT = 1,
};

/* Direction mode for GPU brush ray casting.
 * Values 0-5 match the draw_dynamicpaint2 GPU dispatch switch. */
enum eDynPaint2Gpu_Direction {
  DP2GPU_DIR_X = 0,
  DP2GPU_DIR_NEG_X = 1,
  DP2GPU_DIR_Y = 2,
  DP2GPU_DIR_NEG_Y = 3,
  DP2GPU_DIR_Z = 4,
  DP2GPU_DIR_NEG_Z = 5,
  DP2GPU_DIR_ORIGIN_TO_TARGET = 6,
  DP2GPU_DIR_ORIGIN_FORWARD = 7,
};

/* Per-brush GPU settings used by the Dynamic Paint GPU modifier.
 * Each brush performs one GPU compute dispatch with its own uniforms. */
struct DynamicPaint2GpuBrushSettings {
  struct DynamicPaint2GpuBrushSettings *next = nullptr, *prev = nullptr;

  /* Human readable name. */
  char name[/*MAX_NAME*/ 64] = "";

  /* Ray origin object (position used as ray start). */
  struct Object *origin = nullptr;
  /* Optional target object: when direction_mode == 6, ray goes from
   * origin to target. When ray_length == 0, distance origin-target is used. */
  struct Object *target = nullptr;

  /* High-level direction type: 0 = axis, 1 = object.
   * See eDynPaint2Gpu_DirectionType. */
  char direction_type = DP2GPU_DIRTYPE_AXIS;
  /* Direction mode: see eDynPaint2Gpu_Direction.
   *   Axis type: 0-5 (+X/-X/+Y/-Y/+Z/-Z).
   *   Object type: 6 = origin-to-target, 7 = origin forward. */
  char direction_mode = DP2GPU_DIR_NEG_Z;
  /* When true, displacement follows vertex normals instead of the ray axis. */
  char use_vertex_normals = 1;
  char _pad0[1] = {};

  /* Ray length (0 = automatic: use origin-target distance). */
  float ray_length = 0.0f;
  /* Brush influence radius around hit point. */
  float radius = 1.0f;
  /* Brush intensity / strength [0..1]. */
  float intensity = 1.0f;

  /* Falloff: how intensity decays with distance from the ray.
   * `falloff_type` selects the curve shape (see eDynPaint2Gpu_Falloff).
   * `falloff` is the distance beyond which falloff reaches zero.
   * `curfalloff` is an optional CurveMapping for DP2GPU_FALLOFF_CURVE. */
  char falloff_type = DP2GPU_FALLOFF_SMOOTH;
  char _pad[3] = {};
  float falloff = 0.0f; /* 0 = use radius as falloff distance */
  struct CurveMapping *curfalloff = nullptr;

  /* Optional procedural texture to modulate intensity. */
  struct Tex *mask_texture = nullptr;
  /* Texture coordinate mapping (reuses DisplaceModifierTexMapping enum values:
   *   MOD_DISP_MAP_LOCAL=0, MOD_DISP_MAP_GLOBAL=1, MOD_DISP_MAP_OBJECT=2, MOD_DISP_MAP_UV=3). */
  int texmapping = 0;
  int _pad2 = 0;
  /** Object used as reference for OBJECT texture mapping. */
  struct Object *map_object = nullptr;
  /** UV layer name for UV texture mapping. */
  char uvlayer_name[/*MAX_CUSTOMDATA_LAYER_NAME*/ 64] = "";

  int flag = 0;
  int _pad1[3] = {};
};

/* Surface (canvas layer) for the GPU dynamic paint modifier.
 * Kept minimal: animation is handled by standard Blender keyframes
 * on the brush origin/target objects. */
struct DynamicPaint2GpuSurface {
  struct DynamicPaint2GpuSurface *next = nullptr, *prev = nullptr;

  /* Back-ref to canvas settings (for RNA path helpers). */
  struct DynamicPaint2GpuCanvasSettings *canvas = nullptr;

  /* Optional collection filter: only process brushes from objects in this
   * collection (nullptr = process all brushes on the modifier). */
  struct Collection *brush_group = nullptr;

  char name[64] = "";
  int flags = 0;
  char _pad[4] = {};
};

/* NOTE: DynamicPaint2GpuModifierData is defined in DNA_modifier_types.h.
 * Do not redefine it here to avoid duplicate symbols. */

/* Canvas settings for the GPU dynamic paint modifier. */
struct DynamicPaint2GpuCanvasSettings {
  struct DynamicPaint2GpuModifierData *pmd = nullptr; /* back-ref for RNA */

  ListBase surfaces = {nullptr, nullptr};
  short active_sur = 0;
  short flags = 0;
  char _pad[4] = {};

  char error[64] = "";
};

}  // namespace blender
