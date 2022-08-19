/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2017 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup draw
 */

#pragma once

#include "BKE_attribute.h"
#include "GPU_shader.h"

#include "draw_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Curves;
struct GPUVertBuf;
struct GPUIndexBuf;
struct GPUBatch;
struct GPUTexture;

#define MAX_THICKRES 2    /* see eHairType */
#define MAX_HAIR_SUBDIV 4 /* see hair_subdiv rna */

typedef enum CurvesEvalShader {
  CURVES_EVAL_CATMULL_ROM = 0,
  CURVES_EVAL_BEZIER = 1,
} CurvesEvalShader;
#define CURVES_EVAL_SHADER_NUM 3

typedef struct CurvesEvalFinalCache {
  /* Output of the subdivision stage: vertex buffer sized to subdiv level. */
  GPUVertBuf *proc_buf;
  GPUTexture *proc_tex;

  /** Just contains a huge index buffer used to draw the final curves. */
  GPUBatch *proc_hairs[MAX_THICKRES];

  /** Points per curve, at least 2. */
  int strands_res;

  /** Attributes currently being drawn or about to be drawn. */
  DRW_Attributes attr_used;

  /**
   * Attributes that were used at some point. This is used for garbage collection, to remove
   * attributes that are not used in shaders anymore due to user edits.
   */
  DRW_Attributes attr_used_over_time;

  /**
   * The last time in seconds that the `attr_used` and `attr_used_over_time` were exactly the same.
   * If the delta between this time and the current scene time is greater than the timeout set in
   * user preferences (`U.vbotimeout`) then garbage collection is performed.
   */
  int last_attr_matching_time;

  /* Output of the subdivision stage: vertex buffers sized to subdiv level. This is only attributes
   * on point domain. */
  GPUVertBuf *attributes_buf[GPU_MAX_ATTR];
  GPUTexture *attributes_tex[GPU_MAX_ATTR];
} CurvesEvalFinalCache;

/* Curves procedural display: Evaluation is done on the GPU. */
typedef struct CurvesEvalCache {
  /* Input control point positions combined with parameter data. */
  GPUVertBuf *proc_point_buf;
  GPUTexture *point_tex;

  /** Info of control points strands (segment count and base index) */
  GPUVertBuf *proc_strand_buf;
  GPUTexture *strand_tex;

  /* Curve length data. */
  GPUVertBuf *proc_length_buf;
  GPUTexture *length_tex;

  GPUVertBuf *proc_strand_seg_buf;
  GPUTexture *strand_seg_tex;

  CurvesEvalFinalCache final[MAX_HAIR_SUBDIV];

  /* For point attributes, which need subdivision, these buffers contain the input data.
   * For curve domain attributes, which do not need subdivision, these are the final data. */
  GPUVertBuf *proc_attributes_buf[GPU_MAX_ATTR];
  GPUTexture *proc_attributes_tex[GPU_MAX_ATTR];

  int strands_len;
  int elems_len;
  int point_len;
} CurvesEvalCache;

/**
 * Ensure all necessary textures and buffers exist for GPU accelerated drawing.
 */
bool curves_ensure_procedural_data(struct Curves *curves,
                                   struct CurvesEvalCache **r_hair_cache,
                                   struct GPUMaterial *gpu_material,
                                   int subdiv,
                                   int thickness_res);

void drw_curves_get_attribute_sampler_name(const char *layer_name, char r_sampler_name[32]);

#ifdef __cplusplus
}
#endif
