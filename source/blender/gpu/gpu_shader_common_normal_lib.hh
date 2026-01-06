/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * \file gpu_shader_common_normal_lib.hh
 * \ingroup gpu
 *
 * Common normal calculation functions for GPU shaders (GLSL string generation).
 * Shared between mesh_gpu.cc scatter shader and draw_displace.cc compute shader.
 *
 * Port of CPU functions from:
 * - BLI_math_base.hh (safe_acos_approx)
 * - gpu_shader_math_vector_safe_lib.glsl (safe_normalize)
 * - mesh_normals.cc (face_normal_object)
 */

#pragma once

#include <string>

namespace blender::gpu {

/* -------------------------------------------------------------------- */
/** \name Normal Packing Utilities (10_10_10_2 and 16-bit formats)
 * \{ */

/**
 * Returns GLSL code for normal packing utilities.
 * Supports two formats:
 * - 10_10_10_2: Compact format (1x uint, 32 bits total)
 * - 16-bit pairs: High-quality format (2x uint, 64 bits total)
 * 
 * Used by scatter shader and other systems that need to pack normals into GPU buffers.
 */
static std::string get_normal_packing_glsl()
{
  return R"GLSL(
/* Normal Packing Utilities - GPU port matching Blender's packing formats */

/* 10_10_10_2 format packing (compact, 32 bits total)
 * Each component is packed into 10 bits (signed range -512 to 511)
 * Format: [X:10 bits][Y:10 bits][Z:10 bits][W:2 bits unused] */

int pack_i10_trunc(float x) {
  const int signed_int_10_max = 511;
  const int signed_int_10_min = -512;
  float s = x * float(signed_int_10_max);
  int q = int(s);
  q = clamp(q, signed_int_10_min, signed_int_10_max);
  return q & 0x3FF;
}

uint pack_norm(vec3 n) {
  int nx = pack_i10_trunc(n.x);
  int ny = pack_i10_trunc(n.y);
  int nz = pack_i10_trunc(n.z);
  return uint(nx) | (uint(ny) << 10) | (uint(nz) << 20);
}

/* 16-bit format packing (high-quality, 64 bits total)
 * Each component is packed into 16 bits (signed range -32768 to 32767)
 * Returns two uints for a complete normal (X,Y in first, Z,W in second) */

int pack_i16_trunc(float x) {
  return clamp(int(round(x * 32767.0)), -32768, 32767);
}

uint pack_i16_pair(float a, float b) {
  return (uint(pack_i16_trunc(a)) & 0xFFFFu) | ((uint(pack_i16_trunc(b)) & 0xFFFFu) << 16);
}
)GLSL";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Safe Normalize (GLSL)
 * \{ */

/**
 * Returns GLSL code for safe_normalize function.
 * Uses length_squared to avoid double sqrt() and match CPU threshold semantics.
 */
static std::string get_safe_normalize_glsl()
{
  return R"GLSL(
/* Safe normalize: returns vec3(1,0,0) if input is zero.
 * Matches Blender's safe_normalize_and_get_length().
 * Uses length_squared to avoid double sqrt() and match CPU threshold semantics exactly.
 * Reference: gpu_shader_math_vector_safe_lib.glsl */
vec3 safe_normalize(vec3 v) {
  float length_squared = dot(v, v);
  const float threshold = 1e-35;
  if (length_squared > threshold) {
    float len = sqrt(length_squared);
    return v / len;
  }
  /* Either the vector is small or one of its values contained `nan`. */
  return vec3(1.0, 0.0, 0.0);
}
)GLSL";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Safe Arccos Approximation (GLSL)
 * \{ */

/**
 * Returns GLSL code for safe_acos_approx function.
 * GPU port of BLI_math_base.hh safe_acos_approx.
 * Max error 4.51803e-05 (0.00258 degrees).
 */
static std::string get_safe_acos_approx_glsl()
{
  return R"GLSL(
/* GPU port of safe_acos_approx from BLI_math_base.hh (line ~197).
 * Faster/approximate version of safe_acos. Max error 4.51803e-05 (0.00258 degrees).
 * Based on http://www.pouet.net/topic.php?which=9132&page=2
 * Examined 2130706434 values of acos:
 * - 15.2007108 avg ULP diff, 4492 max ULP, 4.51803e-05 max error (85% accurate ULP 0) */
float safe_acos_approx(float x) {
  const float f = abs(x);
  /* Clamp and crush denormals. */
  const float m = (f < 1.0) ? 1.0 - (1.0 - f) : 1.0;
  /* Polynomial approximation (optimized for [0, 1] range) */
  const float a = sqrt(1.0 - m) *
                  (1.5707963267 + m * (-0.213300989 + m * (0.077980478 + m * -0.02164095)));
  /* Mirror for negative values: acos(-x) = pi - acos(x) */
  return (x < 0.0) ? (3.14159265359 - a) : a;
}
)GLSL";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Face Normal Calculation (Unified with Position Buffer Macro)
 * \{ */

/**
 * Returns GLSL code for face_normal_object function.
 * GPU port of mesh_normals.cc face_normal_object.
 * Optimized paths for tris/quads/ngons with len_sq threshold check.
 *
 * Requirements:
 * - Topology accessors: int face_offsets(int i); int corner_verts(int i);
 * - Position buffer macro: POSITION_BUFFER must be defined (e.g., input_positions or positions_in).
 */
static std::string get_face_normal_object_glsl()
{
  return R"GLSL(
/* GPU port of face_normal_object from mesh_normals.cc.
 * Computes face normal using optimized paths for tris/quads/ngons.
 * Requirements:
 * - Topology accessors: int face_offsets(int i); int corner_verts(int i);
 * - Position buffer: POSITION_BUFFER macro must be defined before including this code.
 * Returns: Normalized face normal (X+ fallback for degenerate faces) */
vec3 face_normal_object(int f) {
  int beg = face_offsets(f);
  int end = face_offsets(f + 1);
  int count = end - beg;

  /* Fast path: Triangles (most common case) */
  if (count == 3) {
    vec3 a = POSITION_BUFFER[corner_verts(beg + 0)].xyz;
    vec3 b = POSITION_BUFFER[corner_verts(beg + 1)].xyz;
    vec3 c = POSITION_BUFFER[corner_verts(beg + 2)].xyz;
    vec3 n = cross(b - a, c - a);
    float len_sq = dot(n, n);
    const float threshold = 1e-35;
    if (len_sq <= threshold) {
      return vec3(1.0, 0.0, 0.0);  /* Degenerate triangle -> X+ fallback */
    }
    return n / sqrt(len_sq);
  }

  /* Fast path: Quads (second most common) */
  if (count == 4) {
    vec3 v1 = POSITION_BUFFER[corner_verts(beg + 0)].xyz;
    vec3 v2 = POSITION_BUFFER[corner_verts(beg + 1)].xyz;
    vec3 v3 = POSITION_BUFFER[corner_verts(beg + 2)].xyz;
    vec3 v4 = POSITION_BUFFER[corner_verts(beg + 3)].xyz;
    /* Use diagonal cross-product method to match CPU normal_quad_v3(). */
    vec3 d1 = v1 - v3;
    vec3 d2 = v2 - v4;
    vec3 n = cross(d1, d2);
    float len_sq = dot(n, n);
    const float threshold = 1e-35;
    if (len_sq <= threshold) {
      return vec3(1.0, 0.0, 0.0);  /* Degenerate quad -> X+ fallback */
    }
    return n / sqrt(len_sq);
  }

  /* Fallback: Newell's method for ngons (5+ vertices) */
  vec3 n = vec3(0.0);
  int v_prev_idx = corner_verts(end - 1);
  vec3 v_prev = POSITION_BUFFER[v_prev_idx].xyz;
  for (int i = beg; i < end; ++i) {
    int v_curr_idx = corner_verts(i);
    vec3 v_curr = POSITION_BUFFER[v_curr_idx].xyz;
    n += cross(v_prev, v_curr);
    v_prev = v_curr;
  }
  float len_sq = dot(n, n);
  const float threshold = 1e-35;
  if (len_sq <= threshold) {
    return vec3(1.0, 0.0, 0.0);  /* Degenerate ngon -> X+ fallback */
  }
  return n / sqrt(len_sq);
}
)GLSL";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Simplified Newell Face Normal (DEPRECATED - Use face_normal_object with POSITION_BUFFER macro)
 * \{ */

/**
 * DEPRECATED: Use face_normal_object() with POSITION_BUFFER macro instead.
 * 
 * Returns GLSL code for newell_face_normal_object function.
 * Simplified version using safe_normalize (for shaders without tri/quad optimizations).
 * Kept for backward compatibility - will be removed in future versions.
 */
static std::string get_newell_face_normal_object_glsl()
{
  return R"GLSL(
/* DEPRECATED: Use face_normal_object() with POSITION_BUFFER=positions_in instead.
 * Simplified version using safe_normalize (for shaders without tri/quad optimizations).
 * Requirements:
 * - Topology accessors: int face_offsets(int i); int corner_verts(int i);
 * - Position buffer: vec4 positions_in[];
 * Returns: Normalized face normal (X+ fallback via safe_normalize) */
vec3 newell_face_normal_object(int f) {
  int beg = face_offsets(f);
  int end = face_offsets(f + 1);
  vec3 n = vec3(0.0);
  int v_prev_idx = corner_verts(end - 1);
  vec3 v_prev = positions_in[v_prev_idx].xyz;
  for (int i = beg; i < end; ++i) {
    int v_curr_idx = corner_verts(i);
    vec3 v_curr = positions_in[v_curr_idx].xyz;
    n += cross(v_prev, v_curr);
    v_prev = v_curr;
  }
  return safe_normalize(n);  /* Returns vec3(1,0,0) for degenerate faces */
}
)GLSL";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Face Adjacent Vertices Helper (for Smooth Normal Calculation)
 * \{ */

/**
 * Returns GLSL code for face_find_adjacent_verts helper function.
 * GPU port of mesh_normals.cc face_find_adjacent_verts (optimized tri/quad/ngon).
 * Used by compute_vertex_normal_smooth() to find adjacent vertices in a face.
 *
 * Requirements:
 * - Topology accessors: int face_offsets(int i); int corner_verts(int i);
 */
static std::string get_face_find_adjacent_verts_glsl()
{
  return R"GLSL(
/* Helper: Find the two adjacent vertices in a face for a given vertex.
 * Returns int2(prev_vert, next_vert) indices.
 * Matches CPU face_find_adjacent_verts() from mesh_normals.cc
 * Optimized for common polygon sizes (tri/quad). */
int2 face_find_adjacent_verts(int f, int v) {
  int beg = face_offsets(f);
  int end = face_offsets(f + 1);
  int count = end - beg;
  
  /* Fast path for triangles (no search needed) */
  if (count == 3) {
    int v0 = corner_verts(beg);
    int v1 = corner_verts(beg + 1);
    int v2 = corner_verts(beg + 2);
    if (v == v0) return int2(v2, v1);
    if (v == v1) return int2(v0, v2);
    return int2(v1, v0);  /* v == v2 */
  }
  
  /* Fast path for quads (common case) */
  if (count == 4) {
    int v0 = corner_verts(beg);
    int v1 = corner_verts(beg + 1);
    int v2 = corner_verts(beg + 2);
    int v3 = corner_verts(beg + 3);
    if (v == v0) return int2(v3, v1);
    if (v == v1) return int2(v0, v2);
    if (v == v2) return int2(v1, v3);
    return int2(v2, v0);  /* v == v3 */
  }
  
  /* General case: linear search for ngons */
  int pos = -1;
  for (int i = beg; i < end; ++i) {
    if (corner_verts(i) == v) {
      pos = i;
      break;
    }
  }
  
  if (pos == -1) {
    /* Vertex not found in face (should not happen) - return dummy */
    return int2(v, v);
  }
  
  /* Get previous and next corners (wrap around) */
  int prev_corner = (pos == beg) ? (end - 1) : (pos - 1);
  int next_corner = (pos == end - 1) ? beg : (pos + 1);
  
  return int2(corner_verts(prev_corner), corner_verts(next_corner));
}
)GLSL";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex Normal Calculation (Angle-Weighted, Unified)
 * \{ */

/**
 * Returns GLSL code for compute_vertex_normal_smooth helper function.
 * GPU port of mesh_normals.cc normals_calc_verts (angle-weighted).
 * Unified implementation used by both scatter shader (mesh_gpu.cc) and displace shader (draw_displace.cc).
 *
 * Requirements:
 * - Topology accessors: int vert_to_face_offsets(int i); int vert_to_face(int i);
 *                       int face_offsets(int i); int corner_verts(int i);
 * - Position buffer: POSITION_BUFFER macro must be defined.
 * - safe_normalize(), safe_acos_approx(), face_normal_object(), face_find_adjacent_verts() must be defined.
 */
static std::string get_compute_vertex_normal_smooth_glsl()
{
  return get_face_find_adjacent_verts_glsl() + R"GLSL(
/* Compute smooth vertex normal using angle-weighted accumulation.
 * Matches CPU mesh_normals.cc behavior.
 * Returns: Normalized smooth normal for vertex v */
vec3 compute_vertex_normal_smooth(int v) {
  int beg = vert_to_face_offsets(v);
  int end = vert_to_face_offsets(v + 1);
  int face_count = end - beg;
  
  /* Early-out: single face vertex uses face normal directly */
  if (face_count == 1) {
    int f = vert_to_face(beg);
    return face_normal_object(f);
  }
  else if (face_count == 0) {
    /* Isolated vertex: use input positions like in CPU */
    return safe_normalize(POSITION_BUFFER[v].xyz);
  }
  
  vec3 n_accum = vec3(0.0);
  
  /* Angle-weighted normal accumulation (matches CPU mesh_normals.cc) */
  for (int i = beg; i < end; ++i) {
    int f = vert_to_face(i);
    vec3 face_normal = face_normal_object(f);
    
    /* Find adjacent vertices in this face */
    int2 adj = face_find_adjacent_verts(f, v);
    
    /* Compute angle at vertex (same as CPU) */
    vec3 v_pos = POSITION_BUFFER[v].xyz;
    
    /* Use safe_normalize for edge directions (protects against degenerate edges) */
    vec3 dir_prev = safe_normalize(POSITION_BUFFER[adj.x].xyz - v_pos);
    vec3 dir_next = safe_normalize(POSITION_BUFFER[adj.y].xyz - v_pos);
    
    /* Compute angle factor */
    float angle = safe_acos_approx(dot(dir_prev, dir_next));
    
    /* Skip degenerate angles (NaN or near-zero) */
    if (isnan(angle) || angle < 1e-10) {
      continue;
    }
    
    /* Weight face normal by angle */
    n_accum += face_normal * angle;
  }
  
  /* Fallback if accumulated normal is zero (fully degenerate topology) */
  float normal_len = length(n_accum);
  if (normal_len < 1e-35) {
    return safe_normalize(POSITION_BUFFER[v].xyz);
  }
  
  return safe_normalize(n_accum);
}
)GLSL";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Combined Normal Calculation Library
 * \{ */

/**
 * Returns all common normal calculation functions concatenated.
 * Use this for shaders that need vertex normal calculation (e.g., displace, scatter).
 */
static std::string get_common_normal_lib_glsl()
{
  return get_normal_packing_glsl() + get_safe_normalize_glsl() + get_safe_acos_approx_glsl() +
         get_face_normal_object_glsl() + get_compute_vertex_normal_smooth_glsl();
}

/** \} */

}  // namespace blender::gpu
