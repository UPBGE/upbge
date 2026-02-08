/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 *
 * GPU brush DNA for dynamic paint 2 (minimal struct definitions).
 */

#pragma once

#include "DNA_listBase.h"
#include "DNA_modifier_types.h"
#include "DNA_texture_types.h"

namespace blender {

struct Object;
struct Tex;
struct DynamicPaint2GpuModifierData;

/* Per-brush GPU settings used by the Dynamic Paint GPU modifier. */
struct DynamicPaint2GpuBrushSettings {
  struct DynamicPaint2GpuBrushSettings *next = nullptr, *prev = nullptr;

  /* Human readable name. */
  char name[/*MAX_NAME*/ 64] = "";

  /* Control objects */
  struct Object *origin = nullptr;
  struct Object *target = nullptr;

  /* Direction selection:
   * 0..5 = +X/-X/+Y/-Y/+Z/-Z, 6 = object->object direction, 7 = origin local axis
   */
  char direction_mode = 0;
  /* 0 = use vertex normals for displacement direction, 1 = use axis */
  char use_vertex_normals = 1;
  char _pad0[2] = {};

  /* Parameters */
  float ray_length = 0.0f; /* 0 -> automatic (use distance to target or global) */
  float radius = 1.0f;
  float intensity = 1.0f;

  /* Optional vertex group to mask influence */
  char vgroup_name[/*MAX_VGROUP_NAME*/ 64] = "";

  /* Optional procedural texture to modulate intensity. */
  struct Tex *mask_texture = nullptr;

  int flag = 0;
  char _pad1[4] = {};
  void *_pad2 = nullptr;
};

/* Minimal surface structure for GPU canvas (mirrors DynamicPaintSurface fields needed). */
struct DynamicPaint2GpuSurface {
  struct DynamicPaint2GpuSurface *next = nullptr, *prev = nullptr;

  char name[64] = "";
  short format = 0; /* e.g. vertex / image */
  short type = 0;   /* paint / displace / weight / wave */
  int flags = 0;

  /* initial color / texture */
  float init_color[4] = {};
  struct Tex *init_texture = nullptr;
  char init_layername[/*MAX_CUSTOMDATA_LAYER_NAME*/ 64] = "";

  /* simple parameters useful for GPU processing */
  int image_resolution = 0;
  int start_frame = 0, end_frame = 0;
};

/* Canvas settings for the GPU dynamic paint modifier. */
struct DynamicPaint2GpuCanvasSettings {
  struct DynamicPaint2GpuModifierData *pmd = nullptr; /* back-ref for RNA */

  ListBase surfaces = {nullptr, nullptr};
  short active_sur = 0;
  short flags = 0;
  char _pad[4] = {}; /* pad to 8 bytes alignment */

  /* Bake / error description */
  char error[64] = "";
};

}  // namespace blender
