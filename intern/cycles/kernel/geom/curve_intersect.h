/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2009-2020 Intel Corporation. Adapted from Embree with
 * with modifications. */

#pragma once

CCL_NAMESPACE_BEGIN

/* Curve primitive intersection functions.
 *
 * The code here was adapted from curve_intersector_sweep.h in Embree, to get
 * an exact match between Embree CPU ray-tracing and our GPU ray-tracing. */

#define CURVE_NUM_BEZIER_SUBDIVISIONS 3
#define CURVE_NUM_BEZIER_SUBDIVISIONS_UNSTABLE (CURVE_NUM_BEZIER_SUBDIVISIONS + 1)
#define CURVE_NUM_BEZIER_STEPS 2
#define CURVE_NUM_JACOBIAN_ITERATIONS 5

#ifdef __HAIR__

/* Catmull-rom curve evaluation. */

ccl_device_inline float4 catmull_rom_basis_eval(const float4 curve[4], float u)
{
  const float t = u;
  const float s = 1.0f - u;
  const float n0 = -t * s * s;
  const float n1 = 2.0f + t * t * (3.0f * t - 5.0f);
  const float n2 = 2.0f + s * s * (3.0f * s - 5.0f);
  const float n3 = -s * t * t;
  return 0.5f * (curve[0] * n0 + curve[1] * n1 + curve[2] * n2 + curve[3] * n3);
}

ccl_device_inline float4 catmull_rom_basis_derivative(const float4 curve[4], float u)
{
  const float t = u;
  const float s = 1.0f - u;
  const float n0 = -s * s + 2.0f * s * t;
  const float n1 = 2.0f * t * (3.0f * t - 5.0f) + 3.0f * t * t;
  const float n2 = 2.0f * s * (3.0f * t + 2.0f) - 3.0f * s * s;
  const float n3 = -2.0f * s * t + t * t;
  return 0.5f * (curve[0] * n0 + curve[1] * n1 + curve[2] * n2 + curve[3] * n3);
}

ccl_device_inline float4 catmull_rom_basis_derivative2(const float4 curve[4], float u)
{

  const float t = u;
  const float n0 = -3.0f * t + 2.0f;
  const float n1 = 9.0f * t - 5.0f;
  const float n2 = -9.0f * t + 4.0f;
  const float n3 = 3.0f * t - 1.0f;
  return (curve[0] * n0 + curve[1] * n1 + curve[2] * n2 + curve[3] * n3);
}

/* Thick Curve */

ccl_device_inline float3 dnormalize(const float3 p, const float3 dp)
{
  const float pp = dot(p, p);
  const float pdp = dot(p, dp);
  return (pp * dp - pdp * p) / (pp * sqrtf(pp));
}

ccl_device_inline float sqr_point_to_line_distance(const float3 PmQ0, const float3 Q1mQ0)
{
  const float3 N = cross(PmQ0, Q1mQ0);
  const float3 D = Q1mQ0;
  return dot(N, N) / dot(D, D);
}

ccl_device_inline bool cylinder_intersect(const float3 cylinder_start,
                                          const float3 cylinder_end,
                                          const float cylinder_radius,
                                          const float3 ray_D,
                                          ccl_private float2 *t_o,
                                          ccl_private float *u0_o,
                                          ccl_private float3 *Ng0_o,
                                          ccl_private float *u1_o,
                                          ccl_private float3 *Ng1_o)
{
  /* Calculate quadratic equation to solve. */
  const float rl = 1.0f / len(cylinder_end - cylinder_start);
  const float3 P0 = cylinder_start, dP = (cylinder_end - cylinder_start) * rl;
  const float3 O = -P0, dO = ray_D;

  const float dOdO = dot(dO, dO);
  const float OdO = dot(dO, O);
  const float OO = dot(O, O);
  const float dOz = dot(dP, dO);
  const float Oz = dot(dP, O);

  const float A = dOdO - sqr(dOz);
  const float B = 2.0f * (OdO - dOz * Oz);
  const float C = OO - sqr(Oz) - sqr(cylinder_radius);

  /* We miss the cylinder if determinant is smaller than zero. */
  const float D = B * B - 4.0f * A * C;
  if (!(D >= 0.0f)) {
    *t_o = make_float2(FLT_MAX, -FLT_MAX);
    return false;
  }

  /* Special case for rays that are parallel to the cylinder. */
  const float eps = 16.0f * FLT_EPSILON * max(fabsf(dOdO), fabsf(sqr(dOz)));
  if (fabsf(A) < eps) {
    if (C <= 0.0f) {
      *t_o = make_float2(-FLT_MAX, FLT_MAX);
      return true;
    }
    else {
      *t_o = make_float2(-FLT_MAX, FLT_MAX);
      return false;
    }
  }

  /* Standard case for rays that are not parallel to the cylinder. */
  const float Q = sqrtf(D);
  const float rcp_2A = 1.0f / (2.0f * A);
  const float t0 = (-B - Q) * rcp_2A;
  const float t1 = (-B + Q) * rcp_2A;

  /* Calculates u and Ng for near hit. */
  {
    *u0_o = (t0 * dOz + Oz) * rl;
    const float3 Pr = t0 * ray_D;
    const float3 Pl = (*u0_o) * (cylinder_end - cylinder_start) + cylinder_start;
    *Ng0_o = Pr - Pl;
  }

  /* Calculates u and Ng for far hit. */
  {
    *u1_o = (t1 * dOz + Oz) * rl;
    const float3 Pr = t1 * ray_D;
    const float3 Pl = (*u1_o) * (cylinder_end - cylinder_start) + cylinder_start;
    *Ng1_o = Pr - Pl;
  }

  *t_o = make_float2(t0, t1);

  return true;
}

ccl_device_inline float2 half_plane_intersect(const float3 P, const float3 N, const float3 ray_D)
{
  const float3 O = -P;
  const float3 D = ray_D;
  const float ON = dot(O, N);
  const float DN = dot(D, N);
  const float min_rcp_input = 1e-18f;
  const bool eps = fabsf(DN) < min_rcp_input;
  const float t = -ON / DN;
  const float lower = (eps || DN < 0.0f) ? -FLT_MAX : t;
  const float upper = (eps || DN > 0.0f) ? FLT_MAX : t;
  return make_float2(lower, upper);
}

ccl_device bool curve_intersect_iterative(const float3 ray_D,
                                          const float ray_tmin,
                                          ccl_private float *ray_tmax,
                                          const float dt,
                                          const float4 curve[4],
                                          float u,
                                          float t,
                                          const bool use_backfacing,
                                          ccl_private Intersection *isect)
{
  const float length_ray_D = len(ray_D);

  /* Error of curve evaluations is proportional to largest coordinate. */
  const float4 box_min = min(min(curve[0], curve[1]), min(curve[2], curve[3]));
  const float4 box_max = max(min(curve[0], curve[1]), max(curve[2], curve[3]));
  const float4 box_abs = max(fabs(box_min), fabs(box_max));
  const float P_err = 16.0f * FLT_EPSILON *
                      max(box_abs.x, max(box_abs.y, max(box_abs.z, box_abs.w)));
  const float radius_max = box_max.w;

  for (int i = 0; i < CURVE_NUM_JACOBIAN_ITERATIONS; i++) {
    const float3 Q = ray_D * t;
    const float3 dQdt = ray_D;
    const float Q_err = 16.0f * FLT_EPSILON * length_ray_D * t;

    const float4 P4 = catmull_rom_basis_eval(curve, u);
    const float4 dPdu4 = catmull_rom_basis_derivative(curve, u);

    const float3 P = float4_to_float3(P4);
    const float3 dPdu = float4_to_float3(dPdu4);
    const float radius = P4.w;
    const float dradiusdu = dPdu4.w;

    const float3 ddPdu = float4_to_float3(catmull_rom_basis_derivative2(curve, u));

    const float3 R = Q - P;
    const float len_R = len(R);
    const float R_err = max(Q_err, P_err);
    const float3 dRdu = -dPdu;
    const float3 dRdt = dQdt;

    const float3 T = normalize(dPdu);
    const float3 dTdu = dnormalize(dPdu, ddPdu);
    const float cos_err = P_err / len(dPdu);

    const float f = dot(R, T);
    const float f_err = len_R * P_err + R_err + cos_err * (1.0f + len_R);
    const float dfdu = dot(dRdu, T) + dot(R, dTdu);
    const float dfdt = dot(dRdt, T);

    const float K = dot(R, R) - sqr(f);
    const float dKdu = (dot(R, dRdu) - f * dfdu);
    const float dKdt = (dot(R, dRdt) - f * dfdt);
    const float rsqrt_K = inversesqrtf(K);

    const float g = sqrtf(K) - radius;
    const float g_err = R_err + f_err + 16.0f * FLT_EPSILON * radius_max;
    const float dgdu = dKdu * rsqrt_K - dradiusdu;
    const float dgdt = dKdt * rsqrt_K;

    const float invdet = 1.0f / (dfdu * dgdt - dgdu * dfdt);
    u -= (dgdt * f - dfdt * g) * invdet;
    t -= (-dgdu * f + dfdu * g) * invdet;

    if (fabsf(f) < f_err && fabsf(g) < g_err) {
      t += dt;
      if (!(t >= ray_tmin && t <= *ray_tmax)) {
        return false; /* Rejects NaNs */
      }
      if (!(u >= 0.0f && u <= 1.0f)) {
        return false; /* Rejects NaNs */
      }

      /* Back-face culling. */
      const float3 R = normalize(Q - P);
      const float3 U = dradiusdu * R + dPdu;
      const float3 V = cross(dPdu, R);
      const float3 Ng = cross(V, U);
      if (!use_backfacing && dot(ray_D, Ng) > 0.0f) {
        return false;
      }

      /* Record intersection. */
      *ray_tmax = t;
      isect->t = t;
      isect->u = u;
      isect->v = 0.0f;

      return true;
    }
  }
  return false;
}

ccl_device bool curve_intersect_recursive(const float3 ray_P,
                                          const float3 ray_D,
                                          const float ray_tmin,
                                          float ray_tmax,
                                          float4 curve[4],
                                          ccl_private Intersection *isect)
{
  /* Move ray closer to make intersection stable. */
  const float3 center = float4_to_float3(0.25f * (curve[0] + curve[1] + curve[2] + curve[3]));
  const float dt = dot(center - ray_P, ray_D) / dot(ray_D, ray_D);
  const float3 ref = ray_P + ray_D * dt;
  const float4 ref4 = make_float4(ref.x, ref.y, ref.z, 0.0f);
  curve[0] -= ref4;
  curve[1] -= ref4;
  curve[2] -= ref4;
  curve[3] -= ref4;

  const bool use_backfacing = false;
  const float step_size = 1.0f / (float)(CURVE_NUM_BEZIER_STEPS);

  int depth = 0;

  /* todo: optimize stack for GPU somehow? Possibly some bitflags are enough, and
   * u0/u1 can be derived from the depth. */
  struct {
    float u0, u1;
    int i;
  } stack[CURVE_NUM_BEZIER_SUBDIVISIONS_UNSTABLE];

  bool found = false;

  float u0 = 0.0f;
  float u1 = 1.0f;
  int i = 0;

  while (1) {
    for (; i < CURVE_NUM_BEZIER_STEPS; i++) {
      const float step = i * step_size;

      /* Subdivide curve. */
      const float dscale = (u1 - u0) * (1.0f / 3.0f) * step_size;
      const float vu0 = mix(u0, u1, step);
      const float vu1 = mix(u0, u1, step + step_size);

      const float4 P0 = catmull_rom_basis_eval(curve, vu0);
      const float4 dP0du = dscale * catmull_rom_basis_derivative(curve, vu0);
      const float4 P3 = catmull_rom_basis_eval(curve, vu1);
      const float4 dP3du = dscale * catmull_rom_basis_derivative(curve, vu1);

      const float4 P1 = P0 + dP0du;
      const float4 P2 = P3 - dP3du;

      /* Calculate bounding cylinders. */
      const float rr1 = sqr_point_to_line_distance(float4_to_float3(dP0du),
                                                   float4_to_float3(P3 - P0));
      const float rr2 = sqr_point_to_line_distance(float4_to_float3(dP3du),
                                                   float4_to_float3(P3 - P0));
      const float maxr12 = sqrtf(max(rr1, rr2));
      const float one_plus_ulp = 1.0f + 2.0f * FLT_EPSILON;
      const float one_minus_ulp = 1.0f - 2.0f * FLT_EPSILON;
      float r_outer = max(max(P0.w, P1.w), max(P2.w, P3.w)) + maxr12;
      float r_inner = min(min(P0.w, P1.w), min(P2.w, P3.w)) - maxr12;
      r_outer = one_plus_ulp * r_outer;
      r_inner = max(0.0f, one_minus_ulp * r_inner);
      bool valid = true;

      /* Intersect with outer cylinder. */
      float2 tc_outer;
      float u_outer0, u_outer1;
      float3 Ng_outer0, Ng_outer1;
      valid = cylinder_intersect(float4_to_float3(P0),
                                 float4_to_float3(P3),
                                 r_outer,
                                 ray_D,
                                 &tc_outer,
                                 &u_outer0,
                                 &Ng_outer0,
                                 &u_outer1,
                                 &Ng_outer1);
      if (!valid) {
        continue;
      }

      /* Intersect with cap-planes. */
      float2 tp = make_float2(ray_tmin - dt, ray_tmax - dt);
      tp = make_float2(max(tp.x, tc_outer.x), min(tp.y, tc_outer.y));
      const float2 h0 = half_plane_intersect(float4_to_float3(P0), float4_to_float3(dP0du), ray_D);
      tp = make_float2(max(tp.x, h0.x), min(tp.y, h0.y));
      const float2 h1 = half_plane_intersect(
          float4_to_float3(P3), -float4_to_float3(dP3du), ray_D);
      tp = make_float2(max(tp.x, h1.x), min(tp.y, h1.y));
      valid = tp.x <= tp.y;
      if (!valid) {
        continue;
      }

      /* Clamp and correct u parameter. */
      u_outer0 = clamp(u_outer0, 0.0f, 1.0f);
      u_outer1 = clamp(u_outer1, 0.0f, 1.0f);
      u_outer0 = mix(u0, u1, (step + u_outer0) * (1.0f / (float)(CURVE_NUM_BEZIER_STEPS + 1)));
      u_outer1 = mix(u0, u1, (step + u_outer1) * (1.0f / (float)(CURVE_NUM_BEZIER_STEPS + 1)));

      /* Intersect with inner cylinder. */
      float2 tc_inner;
      float u_inner0, u_inner1;
      float3 Ng_inner0, Ng_inner1;
      const bool valid_inner = cylinder_intersect(float4_to_float3(P0),
                                                  float4_to_float3(P3),
                                                  r_inner,
                                                  ray_D,
                                                  &tc_inner,
                                                  &u_inner0,
                                                  &Ng_inner0,
                                                  &u_inner1,
                                                  &Ng_inner1);

      /* At the unstable area we subdivide deeper. */
#  if 0
      const bool unstable0 = (!valid_inner) |
                             (fabsf(dot(normalize(ray_D), normalize(Ng_inner0))) < 0.3f);
      const bool unstable1 = (!valid_inner) |
                             (fabsf(dot(normalize(ray_D), normalize(Ng_inner1))) < 0.3f);
#  else
      /* On the GPU appears to be a little faster if always enabled. */
      (void)valid_inner;

      const bool unstable0 = true;
      const bool unstable1 = true;
#  endif

      /* Subtract the inner interval from the current hit interval. */
      float2 tp0 = make_float2(tp.x, min(tp.y, tc_inner.x));
      float2 tp1 = make_float2(max(tp.x, tc_inner.y), tp.y);
      bool valid0 = valid && (tp0.x <= tp0.y);
      bool valid1 = valid && (tp1.x <= tp1.y);
      if (!(valid0 || valid1)) {
        continue;
      }

      /* Process one or two hits. */
      bool recurse = false;
      if (valid0) {
        const int termDepth = unstable0 ? CURVE_NUM_BEZIER_SUBDIVISIONS_UNSTABLE :
                                          CURVE_NUM_BEZIER_SUBDIVISIONS;
        if (depth >= termDepth) {
          found |= curve_intersect_iterative(
              ray_D, ray_tmin, &ray_tmax, dt, curve, u_outer0, tp0.x, use_backfacing, isect);
        }
        else {
          recurse = true;
        }
      }

      const float t1 = tp1.x + dt;
      if (valid1 && (t1 >= ray_tmin && t1 <= ray_tmax)) {
        const int termDepth = unstable1 ? CURVE_NUM_BEZIER_SUBDIVISIONS_UNSTABLE :
                                          CURVE_NUM_BEZIER_SUBDIVISIONS;
        if (depth >= termDepth) {
          found |= curve_intersect_iterative(
              ray_D, ray_tmin, &ray_tmax, dt, curve, u_outer1, tp1.y, use_backfacing, isect);
        }
        else {
          recurse = true;
        }
      }

      if (recurse) {
        stack[depth].u0 = u0;
        stack[depth].u1 = u1;
        stack[depth].i = i + 1;
        depth++;

        u0 = vu0;
        u1 = vu1;
        i = -1;
      }
    }

    if (depth > 0) {
      depth--;
      u0 = stack[depth].u0;
      u1 = stack[depth].u1;
      i = stack[depth].i;
    }
    else {
      break;
    }
  }

  return found;
}

/* Ribbons */

ccl_device_inline bool cylinder_culling_test(const float2 p1, const float2 p2, const float r)
{
  /* Performs culling against a cylinder. */
  const float2 dp = p2 - p1;
  const float num = dp.x * p1.y - dp.y * p1.x;
  const float den2 = dot(dp, dp);
  return num * num <= r * r * den2;
}

/**
 * Intersects a ray with a quad with back-face culling
 * enabled. The quad v0,v1,v2,v3 is split into two triangles
 * v0,v1,v3 and v2,v3,v1. The edge v1,v2 decides which of the two
 * triangles gets intersected.
 */
ccl_device_inline bool ribbon_intersect_quad(const float ray_tmin,
                                             const float ray_tmax,
                                             const float3 quad_v0,
                                             const float3 quad_v1,
                                             const float3 quad_v2,
                                             const float3 quad_v3,
                                             ccl_private float *u_o,
                                             ccl_private float *v_o,
                                             ccl_private float *t_o)
{
  /* Calculate vertices relative to ray origin? */
  const float3 O = make_float3(0.0f, 0.0f, 0.0f);
  const float3 D = make_float3(0.0f, 0.0f, 1.0f);
  const float3 va = quad_v0 - O;
  const float3 vb = quad_v1 - O;
  const float3 vc = quad_v2 - O;
  const float3 vd = quad_v3 - O;

  const float3 edb = vb - vd;
  const float WW = dot(cross(vd, edb), D);
  const float3 v0 = (WW <= 0.0f) ? va : vc;
  const float3 v1 = (WW <= 0.0f) ? vb : vd;
  const float3 v2 = (WW <= 0.0f) ? vd : vb;

  /* Calculate edges? */
  const float3 e0 = v2 - v0;
  const float3 e1 = v0 - v1;

  /* perform edge tests */
  const float U = dot(cross(v0, e0), D);
  const float V = dot(cross(v1, e1), D);
  if (!(max(U, V) <= 0.0f)) {
    return false;
  }

  /* Calculate geometry normal and denominator? */
  const float3 Ng = cross(e1, e0);
  const float den = dot(Ng, D);
  const float rcpDen = 1.0f / den;

  /* Perform depth test? */
  const float t = rcpDen * dot(v0, Ng);
  if (!(t >= ray_tmin && t <= ray_tmax)) {
    return false;
  }

  /* Avoid division by 0? */
  if (!(den != 0.0f)) {
    return false;
  }

  /* Update hit information? */
  *t_o = t;
  *u_o = U * rcpDen;
  *v_o = V * rcpDen;
  *u_o = (WW <= 0.0f) ? *u_o : 1.0f - *u_o;
  *v_o = (WW <= 0.0f) ? *v_o : 1.0f - *v_o;
  return true;
}

ccl_device_inline void ribbon_ray_space(const float3 ray_D,
                                        const float ray_D_invlen,
                                        float3 ray_space[3])
{
  const float3 D = ray_D * ray_D_invlen;
  const float3 dx0 = make_float3(0, D.z, -D.y);
  const float3 dx1 = make_float3(-D.z, 0, D.x);
  ray_space[0] = normalize(dot(dx0, dx0) > dot(dx1, dx1) ? dx0 : dx1);
  ray_space[1] = normalize(cross(D, ray_space[0]));
  ray_space[2] = D * ray_D_invlen;
}

ccl_device_inline float4 ribbon_to_ray_space(const float3 ray_space[3],
                                             const float3 ray_org,
                                             const float4 P4)
{
  float3 P = float4_to_float3(P4) - ray_org;
  return make_float4(dot(ray_space[0], P), dot(ray_space[1], P), dot(ray_space[2], P), P4.w);
}

ccl_device_inline bool ribbon_intersect(const float3 ray_org,
                                        const float3 ray_D,
                                        const float ray_tmin,
                                        float ray_tmax,
                                        const int N,
                                        float4 curve[4],
                                        ccl_private Intersection *isect)
{
  /* Transform control points into ray space. */
  const float ray_D_invlen = 1.0f / len(ray_D);
  float3 ray_space[3];
  ribbon_ray_space(ray_D, ray_D_invlen, ray_space);

  curve[0] = ribbon_to_ray_space(ray_space, ray_org, curve[0]);
  curve[1] = ribbon_to_ray_space(ray_space, ray_org, curve[1]);
  curve[2] = ribbon_to_ray_space(ray_space, ray_org, curve[2]);
  curve[3] = ribbon_to_ray_space(ray_space, ray_org, curve[3]);

  const float4 mx = max(max(fabs(curve[0]), fabs(curve[1])), max(fabs(curve[2]), fabs(curve[3])));
  const float eps = 4.0f * FLT_EPSILON * max(max(mx.x, mx.y), max(mx.z, mx.w));
  const float step_size = 1.0f / (float)N;

  /* Evaluate first point and radius scaled normal direction. */
  float4 p0 = catmull_rom_basis_eval(curve, 0.0f);
  float3 dp0dt = float4_to_float3(catmull_rom_basis_derivative(curve, 0.0f));
  if (reduce_max(fabs(dp0dt)) < eps) {
    const float4 p1 = catmull_rom_basis_eval(curve, step_size);
    dp0dt = float4_to_float3(p1 - p0);
  }
  float3 wn0 = normalize(make_float3(dp0dt.y, -dp0dt.x, 0.0f)) * p0.w;

  /* Evaluate the bezier curve. */
  for (int i = 0; i < N; i++) {
    const float u = i * step_size;
    const float4 p1 = catmull_rom_basis_eval(curve, u + step_size);
    const bool valid = cylinder_culling_test(
        make_float2(p0.x, p0.y), make_float2(p1.x, p1.y), max(p0.w, p1.w));

    /* Evaluate next point. */
    float3 dp1dt = float4_to_float3(catmull_rom_basis_derivative(curve, u + step_size));
    dp1dt = (reduce_max(fabs(dp1dt)) < eps) ? float4_to_float3(p1 - p0) : dp1dt;
    const float3 wn1 = normalize(make_float3(dp1dt.y, -dp1dt.x, 0.0f)) * p1.w;

    if (valid) {
      /* Construct quad coordinates. */
      const float3 lp0 = float4_to_float3(p0) + wn0;
      const float3 lp1 = float4_to_float3(p1) + wn1;
      const float3 up0 = float4_to_float3(p0) - wn0;
      const float3 up1 = float4_to_float3(p1) - wn1;

      /* Intersect quad. */
      float vu, vv, vt;
      bool valid0 = ribbon_intersect_quad(ray_tmin, ray_tmax, lp0, lp1, up1, up0, &vu, &vv, &vt);

      if (valid0) {
        /* ignore self intersections */
        const float avoidance_factor = 2.0f;
        if (avoidance_factor != 0.0f) {
          float r = mix(p0.w, p1.w, vu);
          valid0 = vt > avoidance_factor * r * ray_D_invlen;
        }

        if (valid0) {
          vv = 2.0f * vv - 1.0f;

          /* Record intersection. */
          ray_tmax = vt;
          isect->t = vt;
          isect->u = u + vu * step_size;
          isect->v = vv;
          return true;
        }
      }
    }

    /* Store point for next step. */
    p0 = p1;
    wn0 = wn1;
  }
  return false;
}

ccl_device_forceinline bool curve_intersect(KernelGlobals kg,
                                            ccl_private Intersection *isect,
                                            const float3 ray_P,
                                            const float3 ray_D,
                                            const float tmin,
                                            const float tmax,
                                            int object,
                                            int prim,
                                            float time,
                                            int type)
{
  const bool is_motion = (type & PRIMITIVE_MOTION);

  KernelCurve kcurve = kernel_data_fetch(curves, prim);

  int k0 = kcurve.first_key + PRIMITIVE_UNPACK_SEGMENT(type);
  int k1 = k0 + 1;
  int ka = max(k0 - 1, kcurve.first_key);
  int kb = min(k1 + 1, kcurve.first_key + kcurve.num_keys - 1);

  float4 curve[4];
  if (!is_motion) {
    curve[0] = kernel_data_fetch(curve_keys, ka);
    curve[1] = kernel_data_fetch(curve_keys, k0);
    curve[2] = kernel_data_fetch(curve_keys, k1);
    curve[3] = kernel_data_fetch(curve_keys, kb);
  }
  else {
    motion_curve_keys(kg, object, prim, time, ka, k0, k1, kb, curve);
  }

  if (type & PRIMITIVE_CURVE_RIBBON) {
    /* todo: adaptive number of subdivisions could help performance here. */
    const int subdivisions = kernel_data.bvh.curve_subdivisions;
    if (ribbon_intersect(ray_P, ray_D, tmin, tmax, subdivisions, curve, isect)) {
      isect->prim = prim;
      isect->object = object;
      isect->type = type;
      return true;
    }

    return false;
  }
  else {
    if (curve_intersect_recursive(ray_P, ray_D, tmin, tmax, curve, isect)) {
      isect->prim = prim;
      isect->object = object;
      isect->type = type;
      return true;
    }

    return false;
  }
}

ccl_device_inline void curve_shader_setup(KernelGlobals kg,
                                          ccl_private ShaderData *sd,
                                          float3 P,
                                          float3 D,
                                          float t,
                                          const int isect_object,
                                          const int isect_prim)
{
  if (!(sd->object_flag & SD_OBJECT_TRANSFORM_APPLIED)) {
    const Transform tfm = object_get_inverse_transform(kg, sd);

    P = transform_point(&tfm, P);
    D = transform_direction(&tfm, D * t);
    D = safe_normalize_len(D, &t);
  }

  KernelCurve kcurve = kernel_data_fetch(curves, isect_prim);

  int k0 = kcurve.first_key + PRIMITIVE_UNPACK_SEGMENT(sd->type);
  int k1 = k0 + 1;
  int ka = max(k0 - 1, kcurve.first_key);
  int kb = min(k1 + 1, kcurve.first_key + kcurve.num_keys - 1);

  float4 P_curve[4];

  if (!(sd->type & PRIMITIVE_MOTION)) {
    P_curve[0] = kernel_data_fetch(curve_keys, ka);
    P_curve[1] = kernel_data_fetch(curve_keys, k0);
    P_curve[2] = kernel_data_fetch(curve_keys, k1);
    P_curve[3] = kernel_data_fetch(curve_keys, kb);
  }
  else {
    motion_curve_keys(kg, sd->object, sd->prim, sd->time, ka, k0, k1, kb, P_curve);
  }

  P = P + D * t;

  const float4 dPdu4 = catmull_rom_basis_derivative(P_curve, sd->u);
  const float3 dPdu = float4_to_float3(dPdu4);

  if (sd->type & PRIMITIVE_CURVE_RIBBON) {
    /* Rounded smooth normals for ribbons, to approximate thick curve shape. */
    const float3 tangent = normalize(dPdu);
    const float3 bitangent = normalize(cross(tangent, -D));
    const float sine = sd->v;
    const float cosine = safe_sqrtf(1.0f - sine * sine);

    sd->N = normalize(sine * bitangent - cosine * normalize(cross(tangent, bitangent)));
#  if 0
    /* This approximates the position and geometric normal of a thick curve too,
     * but gives too many issues with wrong self intersections. */
    const float dPdu_radius = dPdu4.w;
    sd->Ng = sd->N;
    P += sd->N * dPdu_radius;
#  endif
  }
  else {
    /* Thick curves, compute normal using direction from inside the curve.
     * This could be optimized by recording the normal in the intersection,
     * however for Optix this would go beyond the size of the payload. */
    /* NOTE: It is possible that P will be the same as P_inside (precision issues, or very small
     * radius). In this case use the view direction to approximate the normal. */
    const float3 P_inside = float4_to_float3(catmull_rom_basis_eval(P_curve, sd->u));
    const float3 N = (!isequal(P, P_inside)) ? normalize(P - P_inside) : -sd->I;

    sd->N = N;
    sd->v = 0.0f;
  }

#  ifdef __DPDU__
  /* dPdu/dPdv */
  sd->dPdu = dPdu;
#  endif

  /* Convert to world space. */
  if (!(sd->object_flag & SD_OBJECT_TRANSFORM_APPLIED)) {
    object_position_transform_auto(kg, sd, &P);
    object_normal_transform_auto(kg, sd, &sd->N);
    object_dir_transform_auto(kg, sd, &sd->dPdu);
  }

  sd->P = P;
  sd->Ng = (sd->type & PRIMITIVE_CURVE_RIBBON) ? sd->I : sd->N;
  sd->dPdv = cross(sd->dPdu, sd->Ng);
  sd->shader = kernel_data_fetch(curves, sd->prim).shader_id;
}

#endif

CCL_NAMESPACE_END
