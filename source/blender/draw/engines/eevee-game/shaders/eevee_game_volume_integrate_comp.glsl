/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Volumetric Integration — Front-to-Back Z Accumulation
 *
 * Pass 2 of the two-pass volumetric pipeline.
 *
 * Each thread processes one (X, Y) froxel column from near to far, accumulating
 * the in-scattered radiance and transmittance using the analytical Beer-Lambert
 * exponential integration:
 *
 *   Transmittance(z) = exp(-∫₀ᶻ σ_t(z') dz')
 *
 * For a homogeneous medium in each slice (constant σ_t per froxel), this
 * simplifies to:
 *
 *   T_i = T_{i-1} × exp(-σ_t_i × dz_i)
 *   L_i = L_{i-1} + (in_scatter_i / σ_t_i) × (1 - exp(-σ_t_i × dz_i)) × T_{i-1}
 *
 * The "(1 - exp(-σ_t × dz)) / σ_t" factor is the analytical single-scatter
 * transmittance integral — it avoids numerical instability for small σ_t × dz
 * (which would require L'Hopital: approaches dz as σ_t → 0).
 *
 * Dispatch: ceil(grid_x/8) × ceil(grid_y/8) × 1.
 * The Z dimension is an inner loop, not a grid dimension.  This keeps all Z
 * slices for a given (X, Y) column in a single thread, ensuring sequential
 * reads from volume_grid_tx along Z are cache-friendly (consecutive memory
 * in a 3D texture with Z as the innermost axis).
 *
 * Output: RGBA16F 3D texture.
 *   RGB = accumulated in-scattered radiance at depth slice z (L(z))
 *   A   = accumulated transmittance at depth slice z (T(z))
 *
 * The resolve pass reads this with a single 3D texture lookup per screen pixel.
 *
 * Local group 8×8×1.
 */

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

/* Input from scatter pass: RGB = in-scatter × σ_s, A = σ_t. */
uniform sampler3D in_grid_tx;

/* Output: integrated transmittance + accumulated radiance. */
layout(rgba16f) uniform writeonly image3D out_integrated_img;

/* Froxel geometry for computing slice depth extents. */
uniform float z_near;
uniform float z_far;
uniform int   samples_z;

/* ------------------------------------------------------------------ */
/* Slice thickness in view depth for slice index i.
 *
 * Exponential Z spacing:
 *   z_center(i) = z_near * (z_far / z_near)^((i + 0.5) / samples_z)
 *   dz(i) = z_center(i + 1) - z_center(i)
 *
 * Approximation: dz ≈ z_center(i) × ln(z_far/z_near) / samples_z
 * Exact enough for Beer-Lambert integration — error is < 1% for typical
 * z_near/z_far ratios and slice counts. */
float slice_thickness(int z)
{
  float log_ratio = log(z_far / z_near);
  float z_centre  = z_near * exp(log_ratio * (float(z) + 0.5) / float(samples_z));
  /* Derivative of z(t) = z_near * e^(t * log_ratio) w.r.t. t,
   * times the slice width dt = 1/samples_z. */
  return z_centre * log_ratio / float(samples_z);
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

void main()
{
  const ivec2 froxel_xy = ivec2(gl_GlobalInvocationID.xy);
  const ivec3 grid_size = imageSize(out_integrated_img);

  if (any(greaterThanEqual(froxel_xy, grid_size.xy))) {
    return;
  }

  /* Accumulation state: starts at z=0 (near plane). */
  vec3  L_accum = vec3(0.0);  /* Accumulated radiance L(z) */
  float T_accum = 1.0;        /* Accumulated transmittance T(z), starts at 1 (fully lit) */

  const vec3 grid_size_f = vec3(grid_size);

  for (int z = 0; z < grid_size.z; z++) {
    ivec3 froxel = ivec3(froxel_xy, z);

    /* Read the scatter + extinction for this slice. */
    vec3  uvw      = (vec3(froxel) + 0.5) / grid_size_f;
    vec4  voxel    = texture(in_grid_tx, uvw);
    vec3  in_L     = voxel.rgb;   /* In-scattered radiance (σ_s × L_i from scatter pass) */
    float sigma_t  = voxel.a;     /* Extinction coefficient */

    /* Slice thickness in world units (exponential Z spacing). */
    float dz = slice_thickness(z);

    /* Beer-Lambert transmittance step: T' = T × exp(-σ_t × dz). */
    float T_step = exp(-sigma_t * dz);

    /* Single-scatter radiance integral for this slice:
     *   ΔL = T_before × ∫₀^dz L_in(z') × σ_s × exp(-σ_t × z') dz'
     *
     * Analytical result (homogeneous medium in slice):
     *   ΔL = T_before × (in_L / σ_t) × (1 - exp(-σ_t × dz))
     *
     * Guard: when σ_t × dz < 1e-4 (very thin or empty slice), the above
     * is numerically unstable (0/0 form).  L'Hopital limit: → in_L × dz. */
    vec3 delta_L;
    if (sigma_t * dz > 1e-4) {
      delta_L = T_accum * (in_L / sigma_t) * (1.0 - T_step);
    }
    else {
      delta_L = T_accum * in_L * dz;
    }

    L_accum += delta_L;
    T_accum *= T_step;

    /* Write accumulated state at this slice depth.
     * The resolve pass samples this texture to get L + T at any pixel's depth. */
    imageStore(out_integrated_img, froxel, vec4(L_accum, T_accum));
  }
}
