/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Temporal anti-aliasing and upscaling resolve. One dispatch at output resolution, after the
 * store pass wrote this frame into the sample ring.
 *
 * Two estimators, chosen per pixel by its motion:
 *
 * Recursive history (still pixels). Converges past any window length and costs a few taps.
 * 1. Reconstruct the current frame at the pixel center from the 3x3 jittered render samples.
 *    Each sample sits at its texel center minus the jitter; its weight is a Gaussian of its
 *    distance in output pixels. The largest weight says how well this frame covers the pixel:
 *    it is the frame's share in the blend.
 * 2. Reproject with exact geometry: depth -> world position -> object motion chain (resource id
 *    from the prepass) -> previous view projection. Deformed meshes use the velocity prepass.
 *    The sample used is the closest one of the 3x3, so silhouettes follow the foreground.
 * 3. Validate the history against the previous ring layer, in the 2x2 footprint of the
 *    previous samples: same object key and expected depth inside the depth span of those
 *    samples. A static surface in front of all of them is valid too: it could not have been
 *    hidden, it was just missed by the samples (cables, fences, stochastic alpha).
 * 4. Locks. A thin feature (a luma ridge that no 2x2 quad around it explains) is undersampled:
 *    most frames miss it, so the 3x3 box would erase what the history accumulated. A lock skips
 *    the clip while the neighborhood median luma stays within LOCK_KILL_RATIO of its value at
 *    creation, so a moving shadow still kills it. No locks on geometric edges.
 * 5. Clip the history to the variance box of the 3x3, toward the center so the hue is kept;
 *    clipped history loses confidence. Blend with alpha = w / (n + w), n being the confidence
 *    accumulated in the history alpha.
 *
 * Sample window (moving pixels). The recursive history is resampled every frame the pixel
 * moves, and each resampling blurs it: on the CPU reference this was the largest error left
 * in motion. The window instead reconstructs the pixel from the raw samples of the last N
 * frames, each one moved exactly to this frame, so nothing is resampled twice.
 * 6. Per past frame k: where the pixel's surface was (its motion chain for frame k; deformed
 *    meshes: this frame's velocity k times, over fewer frames), the 2x2 nearest samples there.
 *    Samples of the pixel's own moving object follow its motion; any other sample is moved
 *    exactly with its own depth and both cameras (static surfaces).
 *    A sample counts if the current sample at its new position is the same object and is
 *    static, or is the pixel's own object: occlusion and disocclusion come out of this test,
 *    with no color clamp. Clamping each sample to this frame's 3x3 erased thin features on the
 *    CPU reference. A thin foreground (thin_front) also keeps its static samples that are in
 *    front of the current one: this frame's samples miss most of a thin cable. Volatile
 *    pixels also drop the frames whose 3x3 median luma moved, and so do pixels and samples
 *    under transparent surfaces (reactive mask), which have no motion of their own.
 *    Weight: the same Gaussian as step 1, at the sample's new position; past frames count
 *    (1 - reactive) as much under transparent surfaces. Behind a moving object, the surfaces
 *    it uncovered get the weight its past frames give them, with this frame's color.
 * The output is the window where the pixel moves more than WINDOW_MOTION_PX, the history
 * where it stands still, a mix in between, and it is written back as the history. Pixels that
 * use the window alone skip the history sampling, clip and blend (validation and locks still
 * run, so the locks survive the motion).
 *
 * Fresh pixels get no special filter: an edge-directed filter for them measured no gain once
 * the window covered the moving pixels, cost a third of the registers, and blurred still
 * pixels whose clipped confidence fell below one (-0.4 to -0.7 dB).
 *
 * Registers: 64 (4 waves per SIMD on GCN), checked with the AMD compiler. Keep it there.
 *
 * Cost, as measured with the AMD compiler (ACO statistics on GCN):
 * - every load that does not depend on another one is issued before the first use, so their
 *   latencies overlap (tile, reactive mask, closest depth with its id);
 * - shared memory: constant row stride, the 3x3 read at non negative constant offsets from one
 *   address; tile coordinates in float (an integer division by a uniform is 30 instructions);
 * - window sample: the CPU folds the texel to NDC scale, the depth conversion and the output
 *   pixel mapping into one matrix (window_matrix()), so a sample is three multiply-adds, one
 *   division, one shared memory read and the color decode;
 * - static surfaces reproject with one matrix (prev_reproject), world positions only for the
 *   motion chains.
 *
 * Color spaces. Statistics, clip and locks run on c / (1 + max(c)), where one very bright sample
 * cannot dominate. Averages (reconstruction, blend, window) run in linear space, which is the
 * space of the ground truth: tone mapped averages darken every partially covered pixel. Moving
 * pixels blend toward the tone mapped space (fully at HYBRID_MOTION_PX), where fireflies would
 * smear. The history is stored linear, so it is also the output of the pass.
 */

#include "infos/eevee_game_temporal_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_temporal_resolve)

#include "eevee_game_temporal_lib.glsl"

#define RECONSTRUCTION_SHARPNESS TEMPORAL_RECONSTRUCTION_SHARPNESS
/* Window units: output pixels times sqrt(k log2(e)), the Gaussian weight of a sample at g is
 * exp2(-|g|^2). Must match WINDOW_UNIT (TemporalModule), which folds it into the matrices. */
#define WINDOW_UNIT_SQUARED (RECONSTRUCTION_SHARPNESS * 1.4426950408889634)
#define WINDOW_UNIT sqrt(WINDOW_UNIT_SQUARED)
/* Variance box size in standard deviations. 1.25 kept moving shadows trailing on static
 * geometry 20 % longer on the CPU reference, for no measurable gain on thin features. */
#define VARIANCE_GAMMA 1.0
/* Frequency split of the clip (frequency_split): the history's low band, 4 bilinear taps on the
 * diagonals SPLIT_RADIUS * (1 / scale - 1) output pixels away, is clipped; the rest, limited to
 * SPLIT_HIGH_LIMIT box half widths, is kept. Static scene at half resolution: +0.59 dB, less
 * flicker. At 2/3: +0.19 dB but +10 % flicker; outside idle (8 to 32 frames of history, not
 * converged) up to -0.16 dB: hence idle and render scale <= 0.6 only. Unlimited: same PSNR,
 * more flicker; a limit of 1 or less, or scaling it with the clip, loses most of the gain. */
#define SPLIT_RADIUS 0.5
#define SPLIT_HIGH_LIMIT 2.0
/* Volatile objects: -18 % moving shadow trail and better error on the CPU reference. */
#define VARIANCE_GAMMA_VOLATILE 0.75
/* Relative depth slack around the previous depth span. */
#define DEPTH_TOLERANCE 0.02
/* Window: largest 3x3 coverage of a thin foreground (thin_front). */
#define THIN_FRONT_TEXELS 2
/* History resampling blurs a little every frame the pixel moves: cap the confidence. */
#define MOTION_CONFIDENCE_FALLOFF 0.5
/* Motion (output pixels per frame) at which the blend is fully tone mapped. */
#define HYBRID_MOTION_PX 1.0
/* Neighbors within this luma ratio of the center are the same feature. */
#define LOCK_SIMILAR_RATIO 1.05
/* Median luma change that kills a lock, and drops a past frame from a volatile pixel's window.
 * The median ignores a thin feature (at most 3 of 9 samples) and follows any shading change
 * that covers the neighborhood. */
#define MEDIAN_KILL_RATIO 1.25
/* A 3x3 whose linear depths span more than this ratio holds a geometric edge. */
#define LOCK_EDGE_DEPTH_RATIO 1.02
/* Motion (output pixels per frame) at which the output is the window alone. Below, the window
 * and the history mix linearly; WINDOW_REST_PX and below is the history alone. */
#define WINDOW_MOTION_PX 0.25
/* A still pixel does not reproject to exactly zero motion in float (uv to NDC and back: up to
 * 3e-5 pixels on a third of the pixels at 1080p), and any window_mix above zero runs the whole
 * window: measured on a GTX 1660, the static scene paid 0.12 ms per window frame at 1080p. */
#define WINDOW_REST_PX 0.01
/* Frames of the window for deformed meshes. Their past positions are extrapolated from this
 * frame's velocity, which drifts from the true path as the motion changes. Wobbling disk:
 * disk error -38 % (native) to -50 % (render scale 0.67) at 6 frames; at 4 less gain, from 8
 * on the drift costs more than the samples bring. */
#define DEFORM_WINDOW_FRAMES 6
/* Gaussian of the colors filling the surfaces a moving object uncovered (end of step 6), per
 * squared output pixel: 1 measured slightly better than the reconstruction's 4. */
#define MISSING_COLOR_SHARPNESS 1.0

/* Per render texel of the tile: tone mapped YCoCg and 3x3 median luma (statistics), linear RGB
 * (averages), ring info (key, depth, stability). Row stride TILE_STRIDE at every render scale:
 * every neighbor offset is a constant, folded into the shared memory instruction. */
#define TILE_STRIDE TEMPORAL_TILE_MAX
/* Literal sizes: shared arrays are declared before the defines. TEMPORAL_TILE_MAX^2, group size^2. */
shared float4 tile_work[100];
shared float3 tile_linear[100];
shared uint tile_info[100];
/* Per thread, from step 1 to the end of step 6: see `other` in step 1. */
shared float4 tile_other[64];

float4 age_data(int age, int row)
{
  return window_data.rows[age * TEMPORAL_AGE_STRIDE + row];
}

float4x4 age_matrix(int age, int first_row)
{
  return float4x4(age_data(age, first_row),
                  age_data(age, first_row + 1),
                  age_data(age, first_row + 2),
                  age_data(age, first_row + 3));
}

/* World position of this frame to the object's world position `age` frames ago.
 * Rows of object_to_world[t - age] * inverse(object_to_world[t]). */
/* First row of the motion chain of slot (1 + chain index) for frame t - age, age >= 1. */
int chain_row(uint slot, int age)
{
  return int(slot - 1u) * chain_stride + (age - 1) * 3;
}

float3 chain_apply(int row, float4 world)
{
  return float3(
      dot(chain_buf[row], world), dot(chain_buf[row + 1], world), dot(chain_buf[row + 2], world));
}

/* Catmull-Rom in 5 bilinear taps, corners dropped and weights renormalized.
 * Keeps the history sharp under sub-pixel reprojection; bilinear would blur it every frame.
 * (Lanczos-2 with 12 or 16 taps measured only +0.1 dB on the CPU reference.) */
float4 sample_history_catmull_rom(float2 uv, float2 size)
{
  float2 sample_pos = uv * size;
  float2 texel_1 = floor(sample_pos - 0.5) + 0.5;
  float2 f = sample_pos - texel_1;
  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
  float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
  float2 w3 = f * f * (-0.5 + 0.5 * f);
  float2 w12 = w1 + w2;
  float2 uv_0 = (texel_1 - 1.0) / size;
  float2 uv_3 = (texel_1 + 2.0) / size;
  float2 uv_12 = (texel_1 + w2 / w12) / size;

  float4 result = textureLod(history_tx, float2(uv_12.x, uv_0.y), 0.0) * (w12.x * w0.y);
  result += textureLod(history_tx, float2(uv_0.x, uv_12.y), 0.0) * (w0.x * w12.y);
  result += textureLod(history_tx, uv_12, 0.0) * (w12.x * w12.y);
  result += textureLod(history_tx, float2(uv_3.x, uv_12.y), 0.0) * (w3.x * w12.y);
  result += textureLod(history_tx, float2(uv_12.x, uv_3.y), 0.0) * (w12.x * w3.y);
  float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
  return result * (1.0 / weight);
}

/* One past sample of the window (step 6 in main()). A function called 4 times so the calls
 * are unrolled with constant texels: as a loop, the compiler kept the loop (its `continue`s)
 * and indexed the fetched words dynamically. */
/* One past sample of the window, at g in window units relative to the pixel center. True
 * when it counts. */
bool window_sample(uint info,
                   uint color,
                   float4 surface,
                   float2 own_g,
                   bool own_chain,
                   uint key_closest,
                   float4 depth_axis,
                   float far_distance,
                   float2 g_now,
                   float2 tile_base,
                   float2 tile_per_g,
                   bool thin_front,
                   float3 &window_sum,
                   float &window_weight)
{
  /* The pixel's own object (never the background, key 0): moved with it. */
  const bool own = own_chain && info_key(info) == key_closest;
  float2 g = own_g;
  const float inverse = info_inverse_depth(info);
  if (!own) {
    /* Static surface: exact, from its own depth and both cameras; the matrix is affine in z.
     * Perspective: inverse depth, the background (0) only follows the camera rotation.
     * Orthographic: distance, the background on the far plane. */
    float z = inverse;
    if (window_ortho) {
      z = min(1.0 / max(z, 1e-30), far_distance);
    }
    const float4 clip = surface + depth_axis * z;
    if (clip.w <= 0.0) {
      return false;
    }
    g = clip.xy / clip.w - g_now;
  }
  /* Beyond one output pixel per axis the weight is below exp(-4); inside, the current sample at
   * the new position is in this pixel's 3x3, already in shared memory. Tested on the squares,
   * which the weight needs anyway. (A disc instead of the square: +0.05 dB native, -0.04 dB at
   * half resolution, up to 0.26 on one pixel.) */
  const float2 g_squared = g * g;
  if (max(g_squared.x, g_squared.y) >= WINDOW_UNIT_SQUARED) {
    return false;
  }
  /* Tile texel floor(p * scale + jitter) - tile origin, p the new position: positive within one
   * output pixel of the center, so truncation is the floor, and the row offset is added before
   * the conversion (exact: small integers). */
  const float2 tile_pos = tile_base + g * tile_per_g;
  const uint info_now = tile_info[uint(trunc(tile_pos.y) * float(TILE_STRIDE) + tile_pos.x)];
  /* Visible now: same object (or background) at the new position, which must be static unless
   * it is the pixel's own object (whose motion moved the sample). Or a static sample of a thin
   * foreground (see thin_front) in front of the current surface there: nothing hides it, the
   * current samples just missed it. */
  const bool same = info_key(info_now) == info_key(info) && (own || !info_is_unstable(info_now));
  const bool in_front = thin_front && !own && info_key(info) == key_closest &&
                        !info_is_unstable(info) &&
                        inverse > info_inverse_depth(info_now) * (1.0 + DEPTH_TOLERANCE);
  if (!(same || in_front)) {
    return false;
  }
  const float weight = exp2(-(g_squared.x + g_squared.y));
  const float3 squares = ring_color_squares(color);
  window_sum += squares * (weight * ring_color_inverse_scale(squares));
  window_weight += weight;
  return true;
}

/* A sample next to the pixel's closest surface back then (another surface or the background) is
 * next to its edge now, at own_g: the surface keeps its shape. Its weight there when it does not
 * count now (hidden then or elsewhere now: see the end of step 6). */
float missing_sample(uint info, float2 own_g, uint key_closest, bool counted)
{
  const float2 own_squared = own_g * own_g;
  const bool missing = !counted && info_key(info) != key_closest &&
                       max(own_squared.x, own_squared.y) < WINDOW_UNIT_SQUARED;
  return missing ? exp2(-(own_squared.x + own_squared.y)) : 0.0;
}

/* Tile texel i: (i mod tile_dim, i / tile_dim), the quotient in float: (i + 0.5) / tile_dim stays
 * 0.5 / tile_dim away from an integer, far above the rounding error for i < 100. An integer
 * division by a uniform costs 30 instructions on GCN. */
int2 tile_local(int i, float tile_dim_inv)
{
  const float row = floor((float(i) + 0.5) * tile_dim_inv);
  return int2(float(i) - row * float(tile_dim), row);
}

void tile_store(int2 local, float3 color, uint2 ring)
{
  const int index = local.y * TILE_STRIDE + local.x;
  color = sanitize_color(color);
  tile_linear[index] = color;
  /* color_to_space(color, 1.0) without its clamp: sanitized colors are not negative. */
  tile_work[index] = float4(rgb_to_ycocg(color * (1.0 / (1.0 + max3(color)))),
                            ring_median_decode(unpackUnorm4x8(ring.y)));
  tile_info[index] = ring.x;
}

void main()
{
  /* Jittered render texel nearest to an output pixel center: floor(r + jitter), r being the
   * center in unjittered render pixels. The tile holds this texel's 3x3 for the whole group. */
  const int2 group_origin = int2(gl_WorkGroupID.xy) * TEMPORAL_GROUP_SIZE;
  const int2 tile_origin = int2(floor((float2(group_origin) + 0.5) * render_scale + jitter_px)) -
                           1;
  /* Texel i of the tile_dim x tile_dim footprint, in two rounds of 64 (tile_dim <= 10). Every
   * load is issued before the first use, so their latencies overlap. */
  const float tile_dim_inv = 1.0 / float(tile_dim);
  const int tile_len = tile_dim * tile_dim;
  const int i_0 = int(gl_LocalInvocationIndex);
  const int i_1 = i_0 + TEMPORAL_GROUP_SIZE * TEMPORAL_GROUP_SIZE;
  const int2 local_0 = tile_local(min(i_0, tile_len - 1), tile_dim_inv);
  const int2 local_1 = tile_local(min(i_1, tile_len - 1), tile_dim_inv);
  const int3 texel_0 = int3(clamp(tile_origin + local_0, int2(0), render_size - 1), ring_layer);
  const int3 texel_1 = int3(clamp(tile_origin + local_1, int2(0), render_size - 1), ring_layer);
  /* Full precision: the ring color is only for the window's past frames. */
  const float3 color_0 = texelFetch(color_tx, texel_0.xy, 0).rgb;
  const uint2 ring_0 = texelFetch(ring_tx, texel_0, 0).xy;
  const float3 color_1 = texelFetch(color_tx, texel_1.xy, 0).rgb;
  const uint2 ring_1 = texelFetch(ring_tx, texel_1, 0).xy;

  const int2 out_px = int2(gl_GlobalInvocationID.xy);
  const float2 display_f = float2(display_size);
  const float2 uv = (float2(out_px) + 0.5) / display_f;
  /* Transparent surfaces drawn over the opaque scene have no motion of their own. Issued with
   * the tile: nothing depends on it before step 3. */
  const float reactive = textureLod(reactive_tx, uv, 0.0).r;

  if (i_0 < tile_len) {
    tile_store(local_0, color_0, ring_0);
  }
  if (i_1 < tile_len) {
    tile_store(local_1, color_1, ring_1);
  }
  barrier();

  if (any(greaterThanEqual(out_px, display_size))) {
    return;
  }

  const float2 inv_scale = 1.0 / render_scale;
  const float2 render_f = float2(render_size);
  const float2 center = (float2(out_px) + 0.5) * render_scale;
  const int2 base = int2(floor(center + jitter_px));
  const int2 base_local = base - tile_origin;
  const int base_index = base_local.y * TILE_STRIDE + base_local.x;
  /* Top left of the 3x3: its 9 texels are at non negative constant offsets from one address
   * (shared memory instruction offsets are unsigned). */
  const int corner_index = base_index - TILE_STRIDE - 1;

  /* ---- 1. Current frame reconstruction, closest sample. ---- */
  float3 color_sum = float3(0.0);
  float weight_sum = 0.0;
  float weight_max = 0.0;
  /* Inverse depths: the closest is the largest, background is 0. The center is the first
   * candidate and the others must be strictly closer: the center wins ties. */
  float closest_w = info_inverse_depth(tile_info[base_index]);
  int closest_tap = 4;
  float farthest_w = 1e30;
  /* Offsets in output pixels of the samples of the texels base - 1, base, base + 1 per axis.
   * (The compiler fuses the products of exponentials below: 9 of them either way.) */
  const float2 offset_center = (float2(base) + 0.5 - jitter_px - center) * inv_scale;
  float3 weight_x;
  float3 weight_y;
  for (int i = 0; i < 3; i++) {
    const float2 d = offset_center + float(i - 1) * inv_scale;
    weight_x[i] = exp(-RECONSTRUCTION_SHARPNESS * d.x * d.x);
    weight_y[i] = exp(-RECONSTRUCTION_SHARPNESS * d.y * d.y);
  }
  for (int i = 0; i < 9; i++) {
    const int index = corner_index + (i / 3) * TILE_STRIDE + i % 3;
    const float w = info_inverse_depth(tile_info[index]);
    const float weight = weight_x[i % 3] * weight_y[i / 3];
    color_sum += tile_linear[index] * weight;
    weight_sum += weight;
    weight_max = max(weight_max, weight);
    farthest_w = min(farthest_w, w);
    if (i != 4 && w > closest_w) {
      closest_w = w;
      closest_tap = i;
    }
  }
  /* Tap to offset: tap / 3 = (tap * 11) >> 5 for taps below 9. */
  const int closest_row = (closest_tap * 11) >> 5;
  const int2 closest = int2(closest_tap - closest_row * 3, closest_row) - 1;
  const int closest_index = base_index + closest.y * TILE_STRIDE + closest.x;
  const uint closest_info = tile_info[closest_index];
  /* Thin foreground: the closest object covers at most THIN_FRONT_TEXELS of the 3x3 (wires,
   * cables, not the edge of a surface). In the window its samples count where they are in front
   * of the current surface: wires -2 to -5 % error in motion. For any foreground (an edge) that
   * dilates it by up to a pixel: horizon +15 to +21 % error. Counted here, on the infos the loop
   * above just read (counted in the window they stayed live: 9 more registers). */
  /* Also, on edges, the color of the other surfaces (normalized, w: 1 when there are any), for
   * the ones a moving object uncovered (end of step 6). Wide weights: at render scale 1 / 2 the
   * nearest one can be two pixels away. Kept in shared memory until then: in registers, or
   * computed there, it took the window from 64 to 71 to 84 registers. */
  bool thin_front = false;
  float4 other = float4(0.0);
  /* Geometric edge: the 3x3 spans more than LOCK_EDGE_DEPTH_RATIO in depth, or holds the
   * background (0) and a surface. The background alone is not one: its thin features (stars)
   * need the locks (fastfloor sky, star error -61 %). */
  const bool geometric_edge = closest_w > farthest_w * LOCK_EDGE_DEPTH_RATIO;
  if (geometric_edge) {
    /* Read again, not shared with the loop above: that kept its 9 colors live to here. */
    memoryBarrierShared();
    int front_count = 0;
    for (int i = 0; i < 9; i++) {
      const int index = corner_index + (i / 3) * TILE_STRIDE + i % 3;
      if (info_key(tile_info[index]) == info_key(closest_info)) {
        front_count++;
      }
      else {
        const float2 d = offset_center + float2(i % 3 - 1, i / 3 - 1) * inv_scale;
        const float weight = exp(-MISSING_COLOR_SHARPNESS * dot(d, d)) + 1e-6;
        other += float4(tile_linear[index], 1.0) * weight;
      }
    }
    thin_front = front_count <= THIN_FRONT_TEXELS;
    other = (other.w > 0.0) ? float4(other.rgb * (1.0 / other.w), 1.0) : float4(0.0);
  }
  tile_other[gl_LocalInvocationIndex] = other;

  /* ---- 2. Reprojection to the previous frame. ---- */
  const int2 closest_texel = clamp(base + closest, int2(0), render_size - 1);
  const bool is_background = info_is_background(closest_info);
  const bool is_perspective = (curr_depth_params.z != 0.0);
  const float4 prev_depth_params = age_data(1, TEMPORAL_AGE_DEPTH_PARAMS);
  const float2 ndc = uv * 2.0 - 1.0;

  uint entry = 0u;
  /* NDC depth of the closest sample at the pixel center. Background: infinity (perspective,
   * -A: the camera rotation only) or the far plane (orthographic). */
  float ndc_z = is_perspective ? -curr_depth_params.x : 1.0;
  /* World position of the closest sample, for its motion chain. */
  float3 world = float3(0.0);
  float2 uv_prev;
  float expected_w = 0.0;
  bool check_depth = false;
  if (is_background) {
    const float4 clip = prev_reproject * float4(ndc, ndc_z, 1.0);
    uv_prev = clip.xy / clip.w * 0.5 + 0.5;
  }
  else {
    const uint resource_id = texelFetch(object_id_tx, closest_texel, 0).r;
    /* Issued with the id: the entry that says whether it is needed comes one fetch later. */
    const float depth = texelFetch(depth_tx, closest_texel, 0).r;
    entry = motion_buf[min(resource_id, uint(motion_len - 1))];
    if ((entry & TEMPORAL_DEFORM_BIT) != 0u) {
      /* EEVEE velocity: previous minus current UV, packed * 0.01 (velocity_pack). */
      uv_prev = uv + texelFetch(vector_tx, closest_texel, 0).xy * 100.0;
    }
    else {
      /* Full precision depth: the ring holds a half float. */
      ndc_z = (1.0 - depth) * 2.0 - 1.0;
      const uint chain_slot = (entry >> TEMPORAL_SLOT_SHIFT) & TEMPORAL_SLOT_MASK;
      float4 clip;
      if (chain_slot != 0u) {
        /* Moving object: its motion chain applies to world positions. */
        const float4 p = curr_viewprojinv * float4(ndc, ndc_z, 1.0);
        world = p.xyz / p.w;
        clip = age_matrix(1, TEMPORAL_AGE_VIEWPROJ) *
               float4(chain_apply(chain_row(chain_slot, 1), float4(world, 1.0)), 1.0);
      }
      else {
        clip = prev_reproject * float4(ndc, ndc_z, 1.0);
      }
      uv_prev = clip.xy / clip.w * 0.5 + 0.5;
      expected_w = inverse_depth(clip.z / clip.w, prev_depth_params);
      check_depth = true;
    }
  }
  const uint slot = (entry >> TEMPORAL_SLOT_SHIFT) & TEMPORAL_SLOT_MASK;
  const bool is_deformed = (entry & TEMPORAL_DEFORM_BIT) != 0u;
  const bool is_moving = (entry & (TEMPORAL_DEFORM_BIT | TEMPORAL_MOVED_BIT)) != 0u;
  const bool is_volatile = (entry & TEMPORAL_VOLATILE_BIT) != 0u;
  const float motion_px = length((uv_prev - uv) * display_f);
  /* Share of the sample window in the output (step 6); the history alone below. */
  float window_mix = 0.0;
  if (window_frames > 1) {
    window_mix = clamp((motion_px - WINDOW_REST_PX) * (1.0 / (WINDOW_MOTION_PX - WINDOW_REST_PX)),
                       0.0,
                       1.0);
    /* Volatile shading: the recursive history trails behind moving shadows, the window drops
     * the frames whose median changed. Only measured as a gain down to render scale 0.6. */
    if (is_volatile && window_volatile) {
      window_mix = 1.0;
    }
  }
  const bool use_history = window_mix < 1.0;

  const float confidence_cap = max(max_confidence / (1.0 + motion_px * MOTION_CONFIDENCE_FALLOFF),
                                   1.0);
  /* Pixels moving more than WINDOW_MOTION_PX use the window alone: the history is neither
   * sampled nor blended. Validation and locks still run: the locks must survive the motion for
   * when the pixel stops (measured: -0.3 to -0.5 dB in the frames after a stop otherwise). */
  float confidence_new = weight_max;
  float3 result = color_sum / weight_sum;
  /* Median of the 3x3 tone mapped luma around the base texel, from the store pass. */
  const float neighborhood_median = tile_work[base_index].w;
  {
    /* ---- 3. History validation against the previous ring layer. ---- */
    bool valid = history_valid && all(greaterThanEqual(uv_prev, float2(0.0))) &&
                 all(lessThanEqual(uv_prev, float2(1.0)));
    if (valid) {
      /* The 2x2 previous samples nearest to uv_prev: their texels are offset by their jitter. */
      const float4 prev_jitter = age_data(1, TEMPORAL_AGE_JITTER);
      const uint4 info_prev = textureGather(
          ring_tx, float3(uv_prev + prev_jitter.xy / render_f, prev_jitter.z));
      if (is_background) {
        valid = info_is_background(info_prev.x) || info_is_background(info_prev.y) ||
                info_is_background(info_prev.z) || info_is_background(info_prev.w);
      }
      else {
        const uint key = info_key(closest_info);
        bool same_object = false;
        /* Inverse depths: the nearest previous sample is the largest. */
        float span_min = 1e30;
        float span_max = 0.0;
        float nearest_prev = 0.0;
        for (int k = 0; k < 4; k++) {
          if (info_is_background(info_prev[k])) {
            continue;
          }
          float z = info_inverse_depth(info_prev[k]);
          nearest_prev = max(nearest_prev, z);
          if (info_key(info_prev[k]) == key) {
            same_object = true;
            span_min = min(span_min, z);
            span_max = max(span_max, z);
          }
        }
        /* The span covers sloped surfaces; the slack covers precision and jitter. */
        bool same_surface = same_object &&
                            (!check_depth || (expected_w * (1.0 + DEPTH_TOLERANCE) >= span_min &&
                                              expected_w * (1.0 - DEPTH_TOLERANCE) <= span_max));
        bool in_front = check_depth && !is_moving &&
                        expected_w * (1.0 - DEPTH_TOLERANCE) > nearest_prev;
        valid = same_surface || in_front;
      }
    }

    float confidence = 0.0;
    float3 history_rgb = float3(0.0);
    float3 history_low_rgb = float3(0.0);
    const bool split = frequency_split && !is_volatile;
    float2 lock = float2(0.0);
    if (valid) {
      if (use_history) {
        float4 h = sample_history_catmull_rom(uv_prev, display_f);
        history_rgb = max(h.rgb, float3(0.0));
        confidence = max(h.a, 0.0);
        if (split) {
          /* Issued with the Catmull-Rom taps, same texels. */
          const float2 o = (SPLIT_RADIUS * (1.0 / render_scale - 1.0)) / display_f;
          float3 low = textureLod(history_tx, uv_prev + float2(-o.x, -o.y), 0.0).rgb;
          low += textureLod(history_tx, uv_prev + float2(o.x, -o.y), 0.0).rgb;
          low += textureLod(history_tx, uv_prev + float2(-o.x, o.y), 0.0).rgb;
          low += textureLod(history_tx, uv_prev + float2(o.x, o.y), 0.0).rgb;
          history_low_rgb = max(low * 0.25, float3(0.0));
        }
      }
      int2 lock_px = clamp(int2(uv_prev * display_f), int2(0), display_size - 1);
      lock = texelFetch(lock_tx, lock_px, 0).xy;
    }
    confidence *= 1.0 - reactive;

    /* ---- 4. Locks: reproject (nearest), kill, age, create. ---- */
    float lock_life = lock.x * (1.0 - reactive);
    float lock_luma = median_decode(lock.y);
    if (lock_life > 0.0) {
      if (!luma_within(neighborhood_median + 0.01, lock_luma + 0.01, MEDIAN_KILL_RATIO)) {
        lock_life = 0.0;
      }
      lock_life = max(lock_life - lock_decay, 0.0);
    }
    if (geometric_edge) {
      lock_life = 0.0;
    }
    /* Decided before creation: a new lock has no history of its own yet. */
    const bool locked = lock_life > 0.0;
    if (!geometric_edge) {
      /* Ridge: the center is outside the range of the neighbors that differ from it. Thin: no
       * 2x2 quad containing the center is made of similar samples only.
       * luma_within(luma, center, LOCK_SIMILAR_RATIO) split by side: a neighbor differs when it
       * is above `above` or below `below`, so the ridge test is that the differing neighbors
       * are all on one side. Taps: 0 1 2 / 3 4 5 / 6 7 8. */
      const float center_luma = tile_work[base_index].x;
      const float above = LOCK_SIMILAR_RATIO * max(center_luma, 1e-4);
      const float below_ratio = center_luma * (1.0 / LOCK_SIMILAR_RATIO);
      const float below = (below_ratio > 1e-4) ? below_ratio : 0.0;
      bool similar[9];
      bool any_above = false;
      bool any_below = false;
      for (int i = 0; i < 9; i++) {
        /* Read again from shared memory rather than kept in registers since step 1. */
        const float luma = tile_work[corner_index + (i / 3) * TILE_STRIDE + i % 3].x;
        const bool is_above = i != 4 && luma > above;
        const bool is_below = i != 4 && luma < below;
        similar[i] = !(is_above || is_below);
        any_above = any_above || is_above;
        any_below = any_below || is_below;
      }
      const bool ridge = !any_above || !any_below;
      const bool thin = ridge && !(similar[0] && similar[1] && similar[3]) &&
                        !(similar[1] && similar[2] && similar[5]) &&
                        !(similar[3] && similar[6] && similar[7]) &&
                        !(similar[5] && similar[7] && similar[8]);
      if (thin) {
        if (lock_life <= 0.0) {
          lock_luma = neighborhood_median;
        }
        lock_life = 1.0;
      }
    }

    /* Final: stored now, so it does not stay live through the window. */
    imageStore(lock_out_img, out_px, float4(lock_life, median_encode(lock_luma), 0.0, 0.0));

    /* ---- 5. Clip (tone mapped), blend. ---- */
    if (use_history && !locked) {
      /* Variance box of the 3x3, tone mapped YCoCg. */
      float3 moment_1 = float3(0.0);
      float3 moment_2 = float3(0.0);
      for (int i = 0; i < 9; i++) {
        const float3 work = tile_work[corner_index + (i / 3) * TILE_STRIDE + i % 3].xyz;
        moment_1 += work;
        moment_2 += work * work;
      }
      const float3 mean = moment_1 * (1.0 / 9.0);
      /* 1 / max(sigma * gamma, 1e-5): one reciprocal square root instead of a square root and a
       * reciprocal. The variance floor is far below 1e-10 (sigma * gamma = 1e-5). */
      const float3 variance = max(moment_2 * (1.0 / 9.0) - mean * mean, float3(1e-20));
      const float inv_gamma = is_volatile ? 1.0 / VARIANCE_GAMMA_VOLATILE : 1.0 / VARIANCE_GAMMA;
      const float3 inv_box = min(inversesqrt(variance) * inv_gamma, float3(1e5));
      const float3 history_work = color_to_space(history_rgb, 1.0);
      /* Without the split the low band is the whole history. The box of the render resolution
       * 3x3 cannot validate frequencies above the render Nyquist: the split keeps them. */
      float3 low_work = history_work;
      float3 high = float3(0.0);
      if (split) {
        low_work = color_to_space(history_low_rgb, 1.0);
        const float3 limit = SPLIT_HIGH_LIMIT / inv_box;
        high = clamp(history_work - low_work, -limit, limit);
      }
      const float3 delta = low_work - mean;
      const float clip_ratio = max3(abs(delta) * inv_box);
      if (split && clip_ratio <= 1.0) {
        history_rgb = space_to_color(low_work + high, 1.0);
      }
      if (clip_ratio > 1.0) {
        history_rgb = space_to_color(mean + delta / clip_ratio + high, 1.0);
        /* Squared: measured on the CPU reference against moving shadows (trail) and thin
         * features (flicker); the square removes most of the trail for a small flicker cost. */
        confidence /= clip_ratio * clip_ratio;
      }
    }
    if (use_history) {
      confidence_new = min(confidence + weight_max, confidence_cap);
      const float alpha = clamp(weight_max / confidence_new, 0.0, 1.0);
      const float s = clamp(motion_px / HYBRID_MOTION_PX, 0.0, 1.0);
      const float3 blended = mix(color_to_space(history_rgb, s), color_to_space(result, s), alpha);
      result = space_to_color(blended, s);
    }
  }

  /* ---- 6. Sample window. ---- */
  if (window_mix > 0.0) {
    const uint key_closest = info_key(closest_info);
    const float2 p_now = float2(out_px) + 0.5;
    /* Deformed meshes follow their own samples like the motion chains. */
    const bool own_chain = !is_background && (slot != 0u || is_deformed);
    /* The current frame, all of its 3x3 samples. Transparent surfaces over the pixel have no
     * motion of their own: the past frames count (1 - reactive) as much, the current frame
     * being counted 1 / (1 - reactive) as much. */
    const float keep = max(1.0 - reactive, 1.0 / 256.0);
    float3 window_sum = color_sum * (1.0 / keep);
    float window_weight = weight_sum * (1.0 / keep);
    /* For the confidence without history below: one register instead of three in the loop. */
    const float confidence_scale = keep * (weight_max / weight_sum);
    const bool reactive_now = reactive > TEMPORAL_REACTIVE_THRESHOLD;
    /* Window units (WINDOW_UNIT): the pixel center, one render texel. */
    const float2 g_now = p_now * WINDOW_UNIT;
    const float2 g_texel = inv_scale * WINDOW_UNIT;
    /* Tile position of the current sample nearest to the pixel center moved by g:
     * tile_base + g * tile_per_g. */
    const float2 tile_base = p_now * render_scale + jitter_px - float2(tile_origin);
    const float2 tile_per_g = render_scale * (1.0 / WINDOW_UNIT);
    /* NDC to render texels, minus the half texel of the 2x2 footprint below. */
    const float2 render_half = render_f * 0.5;
    const float2 ring_offset = render_half - 0.5;
    const int chain_base = (slot != 0u) ? chain_row(slot, 1) - 3 : 0;
    /* One float4 per kind of pixel, the same registers. Static surfaces and the background:
     * homogeneous world position of the closest sample (a point at infinity for the perspective
     * background), projected to each past frame. Motion chain: its world position, moved by the
     * chain. Deformed: NDC position and its step back per frame from this frame's velocity. */
    const float4 world_h = is_deformed  ? float4(ndc, (uv_prev - uv) * 2.0) :
                           (slot != 0u) ? float4(world, 1.0) :
                                          curr_viewprojinv * float4(ndc, ndc_z, 1.0);
    float missing_weight = 0.0;
    for (int age = 1; age < window_frames; age++) {
      /* Where the pixel's surface was `age` frames ago. */
      const float4x4 viewproj_k = age_matrix(age, TEMPORAL_AGE_VIEWPROJ);
      const float4 clip_k = viewproj_k *
                            ((slot != 0u) ?
                                 float4(chain_apply(chain_base + age * 3, world_h), 1.0) :
                                 world_h);
      if (clip_k.w <= 0.0 && !is_deformed) {
        continue;
      }
      /* Deformed: selected rather than branched, and off screen past DEFORM_WINDOW_FRAMES (the
       * range test below skips the frame): a per pixel loop count or branch here took 20 more
       * registers. */
      const float2 ndc_k = !is_deformed ? clip_k.xy / clip_k.w :
                                          ((age < DEFORM_WINDOW_FRAMES) ?
                                               world_h.xy + world_h.zw * float(age) :
                                               float2(4.0));
      /* Its position in that frame's render texels (sample of texel i at i + 0.5 - jitter),
       * minus one half: the 2x2 samples nearest to it start at its floor. */
      const float4 jitter_k = age_data(age, TEMPORAL_AGE_JITTER);
      const float2 ring_lo = ndc_k * render_half + (ring_offset + jitter_k.xy);
      const int2 t0 = int2(floor(ring_lo));
      if (any(lessThan(t0, int2(-1))) || any(greaterThanEqual(t0, render_size))) {
        continue;
      }
      /* -1 <= t0 < render_size. */
      const int2 lo = max(t0, int2(0));
      const int2 hi = min(t0 + 1, render_size - 1);
      const int layer = int(jitter_k.z);
      /* Every load of this frame at once: one memory round trip per frame, not two dependent
       * ones. Order: (lo.x, hi.y), (hi.x, hi.y), (hi.x, lo.y), (lo.x, lo.y). */
      const uint2 ring_0 = texelFetch(ring_tx, int3(lo.x, hi.y, layer), 0).xy;
      const uint2 ring_1 = texelFetch(ring_tx, int3(hi.x, hi.y, layer), 0).xy;
      const uint2 ring_2 = texelFetch(ring_tx, int3(hi.x, lo.y, layer), 0).xy;
      const uint2 ring_3 = texelFetch(ring_tx, int3(lo.x, lo.y, layer), 0).xy;
      /* Moving shadows, and transparent surfaces over the pixel now or over the samples then:
       * the shading moved, not with the surface. */
      const bool transparent = ((ring_0.x | ring_1.x | ring_2.x | ring_3.x) &
                                TEMPORAL_INFO_TRANSPARENT_BIT) != 0u;
      if (is_volatile || reactive_now || transparent) {
        /* Median around the sample nearest to the position, texel floor(ring_lo + 0.5). */
        const bool2 nearest_hi = greaterThan(int2(floor(ring_lo + 0.5)), t0);
        const uint nearest = nearest_hi.y ? (nearest_hi.x ? ring_1.y : ring_0.y) :
                                            (nearest_hi.x ? ring_2.y : ring_3.y);
        if (!luma_within(ring_median_decode(unpackUnorm4x8(nearest)) + 0.01,
                         neighborhood_median + 0.01,
                         MEDIAN_KILL_RATIO))
        {
          continue;
        }
      }
      /* Static samples: (x, y, -, w) = M * (texel, z, 1) is affine in the texel, so the four
       * samples share the lo texel's terms (columns 0, 1 and 3) and differ by a step. */
      const float4x4 window_k = age_matrix(age, TEMPORAL_AGE_REPROJECT);
      const float2 lo_f = float2(lo);
      const float2 step = float2(hi - lo);
      const float4 surface_00 = window_k[3] + window_k[0] * lo_f.x + window_k[1] * lo_f.y;
      const float4 surface_01 = surface_00 + window_k[1] * step.y;
      const float4 surface_10 = surface_00 + window_k[0] * step.x;
      const float4 surface_11 = surface_01 + window_k[0] * step.x;
      /* Own object samples move with the pixel, from the position to the pixel center: their
       * offset from the center is their offset from the position back then. */
      const float2 own_00 = (lo_f - ring_lo) * g_texel;
      const float2 own_step = g_texel * step;
      const float2 own_01 = own_00 + float2(0.0, own_step.y);
      const float2 own_10 = own_00 + float2(own_step.x, 0.0);
      const float2 own_11 = own_00 + own_step;
      const float far_distance = age_data(age, TEMPORAL_AGE_DEPTH_PARAMS).w;
      const bool counted_0 = window_sample(ring_0.x,
                                           ring_0.y,
                                           surface_01,
                                           own_01,
                                           own_chain,
                                           key_closest,
                                           window_k[2],
                                           far_distance,
                                           g_now,
                                           tile_base,
                                           tile_per_g,
                                           thin_front,
                                           window_sum,
                                           window_weight);
      const bool counted_1 = window_sample(ring_1.x,
                                           ring_1.y,
                                           surface_11,
                                           own_11,
                                           own_chain,
                                           key_closest,
                                           window_k[2],
                                           far_distance,
                                           g_now,
                                           tile_base,
                                           tile_per_g,
                                           thin_front,
                                           window_sum,
                                           window_weight);
      const bool counted_2 = window_sample(ring_2.x,
                                           ring_2.y,
                                           surface_10,
                                           own_10,
                                           own_chain,
                                           key_closest,
                                           window_k[2],
                                           far_distance,
                                           g_now,
                                           tile_base,
                                           tile_per_g,
                                           thin_front,
                                           window_sum,
                                           window_weight);
      const bool counted_3 = window_sample(ring_3.x,
                                           ring_3.y,
                                           surface_00,
                                           own_00,
                                           own_chain,
                                           key_closest,
                                           window_k[2],
                                           far_distance,
                                           g_now,
                                           tile_base,
                                           tile_per_g,
                                           thin_front,
                                           window_sum,
                                           window_weight);
      /* Only frames that saw none of the other surfaces where they are now: one that saw some
       * already weighs them (counting both pulled the wall's bottom edge toward the sky in a
       * slow pan, -0.6 to -1.0 dB). */
      const bool other_seen = (counted_0 && info_key(ring_0.x) != key_closest) ||
                              (counted_1 && info_key(ring_1.x) != key_closest) ||
                              (counted_2 && info_key(ring_2.x) != key_closest) ||
                              (counted_3 && info_key(ring_3.x) != key_closest);
      if ((own_chain || geometric_edge) && !other_seen) {
        missing_weight += missing_sample(ring_0.x, own_01, key_closest, counted_0) +
                          missing_sample(ring_1.x, own_11, key_closest, counted_1) +
                          missing_sample(ring_2.x, own_10, key_closest, counted_2) +
                          missing_sample(ring_3.x, own_00, key_closest, counted_3);
      }
    }
    /* The window reads each past frame where the pixel's closest surface was. At its edges the
     * other surfaces moved differently: uncovered behind a moving object (own_chain), or left
     * behind by a surface the camera moves past them (parallax, the background). Their samples
     * of the past frames are elsewhere, the closest surface's are all there, and the edge was
     * pulled toward it (ghost). The weight the others would have had comes from the closest
     * surface's frames (missing_weight), their color from this frame's samples of them around
     * the pixel (step 1). Behind a moving object, frame after the uncovering: error -49 %
     * (native) to -50 % (1 / 2), ghost -20 %; sky under a wall in a fast pan: error -64 %. */
    if (missing_weight > 0.0) {
      const float4 other = tile_other[gl_LocalInvocationIndex];
      window_sum += other.rgb * missing_weight;
      window_weight += missing_weight * other.w;
    }
    if (use_history) {
      result = mix(result, window_sum / window_weight, window_mix);
    }
    else {
      /* The history was not computed: the window is the output, and its sample weights, in
       * frames of this one, are the confidence the history starts from when it stops. */
      result = window_sum / window_weight;
      confidence_new = min(window_weight * confidence_scale, confidence_cap);
    }
  }

  imageStore(history_out_img, out_px, float4(result, confidence_new));
  /* Alpha: EEVEE's transmittance, nothing behind the opaque output. */
  imageStore(out_color_img, out_px, float4(result, 0.0));
}
