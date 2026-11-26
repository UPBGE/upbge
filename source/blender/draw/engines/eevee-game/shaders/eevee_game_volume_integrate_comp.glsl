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
 * Output, per channel:
 *   out_integrated_img    (RGBA16F)  RGB = accumulated in-scattered radiance L(z)
 *   out_transmittance_img (R11G11B10) RGB = accumulated transmittance T(z)
 *
 * The resolve pass reads this with a single 3D texture lookup per screen pixel.
 *
 * Local group 8×8×1.
 */

#include "infos/eevee_game_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_volume_integrate)

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
  const int2 froxel_xy = int2(gl_GlobalInvocationID.xy);
  const int3 grid_size = imageSize(out_integrated_img);

  if (any(greaterThanEqual(froxel_xy, grid_size.xy))) {
    return;
  }

  /* Accumulation state: starts at z=0 (near plane). */
  float3  L_accum = float3(0.0);  /* Accumulated radiance L(z) */
  float3  T_accum = float3(1.0);  /* Accumulated transmittance T(z) per channel, starts at 1 */

  const float3 grid_size_f = float3(grid_size);

  for (int z = 0; z < grid_size.z; z++) {
    int3 froxel = int3(froxel_xy, z);

    /* Read the scatter + extinction for this slice. */
    float3  uvw      = (float3(froxel) + 0.5) / grid_size_f;
    float3  in_L     = texture(in_grid_tx, uvw).rgb;  /* sigma_s L_i + emission */
    float3  sigma_t  = texture(in_extinction_tx, uvw).rgb;

    /* Slice thickness in world units (exponential Z spacing). */
    float dz = slice_thickness(z);

    /* Beer-Lambert transmittance step, per channel: T' = T × exp(-σ_t × dz). */
    float3 T_step = exp(-sigma_t * dz);

    /* Single-scatter radiance integral for this slice:
     *   ΔL = T_before × ∫₀^dz L_in(z') × σ_s × exp(-σ_t × z') dz'
     *
     * Analytical result (homogeneous medium in slice):
     *   ΔL = T_before × (in_L / σ_t) × (1 - exp(-σ_t × dz))
     *
     * Guard: when σ_t × dz < 1e-4 (very thin or empty slice), the above
     * is numerically unstable (0/0 form).  L'Hopital limit: → in_L × dz. */
    float3 optical = sigma_t * dz;
    float3 segment = mix((1.0 - T_step) / max(sigma_t, float3(1e-30)),
                       float3(dz),
                       lessThan(optical, float3(1e-4)));
    float3 delta_L = T_accum * in_L * segment;

    L_accum += delta_L;
    T_accum *= T_step;

    /* Write accumulated state at this slice depth.
     * The resolve pass samples these to get L + T at any pixel's depth. */
    imageStore(out_integrated_img, froxel, float4(L_accum, 0.0));
    imageStore(out_transmittance_img, froxel, float4(T_accum, 0.0));
  }
}
