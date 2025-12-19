/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * GPU-accelerated Displace modifier implementation.
 */

#include "draw_displace.hh"

#include "BLI_hash.h"
#include "BLI_map.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_vector.hh"

#include "BKE_deform.hh"
#include "BKE_image.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"
#include "BKE_object.hh"
#include "BKE_action.hh" /* BKE_pose_channel_find_name for MAP_OBJECT/bone */

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

#include "../modifiers/intern/MOD_util.hh"

#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_storage_buffer.hh"
#include "GPU_texture.hh"

#include "../gpu/intern/gpu_shader_create_info.hh"

#include "DRW_render.hh"
#include "draw_cache_impl.hh"

#include "../blenkernel/intern/mesh_gpu_cache.hh"

#include "DEG_depsgraph_query.hh"

#include "MEM_guardedalloc.h"

using namespace blender::draw;

/* -------------------------------------------------------------------- */
/** \name Internal Implementation Data
 * \{ */

struct blender::draw::DisplaceManager::Impl {
  /* Composite key: (Mesh*, modifier UID) to support multiple Displace modifiers per mesh */
  struct MeshModifierKey {
    Mesh *mesh;
    uint32_t modifier_uid;

    uint64_t hash() const
    {
      return (uint64_t(reinterpret_cast<uintptr_t>(mesh)) << 32) | uint64_t(modifier_uid);
    }

    bool operator==(const MeshModifierKey &other) const
    {
      return mesh == other.mesh && modifier_uid == other.modifier_uid;
    }
  };

  struct MeshStaticData {
    std::vector<float> vgroup_weights; /* per-vertex weight (0.0-1.0) */
    std::vector<blender::float3> tex_coords; /* per-vertex texture coordinates */
    int verts_num = 0;

    Object *deformed = nullptr;

    bool pending_gpu_setup = false;
    int gpu_setup_attempts = 0;
    uint32_t last_verified_hash = 0;
  };

  Map<MeshModifierKey, MeshStaticData> static_map;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Displace Compute Shader (GPU port of MOD_displace.cc)
 * \{ */

/* GPU Displace Compute Shader - Split into several parts to avoid 16380 char limit */

/* Part 1: Defines and helper functions */
static std::string get_displace_shader_part1() {
  return R"GLSL(
/* GPU Displace Modifier Compute Shader v2.0 */
/* Displace direction modes (matching DisplaceModifierDirection enum) */
#define MOD_DISP_DIR_X 0
#define MOD_DISP_DIR_Y 1
#define MOD_DISP_DIR_Z 2
#define MOD_DISP_DIR_NOR 3
#define MOD_DISP_DIR_RGB_XYZ 4
#define MOD_DISP_DIR_CLNOR 5

/* Displace space modes (matching DisplaceModifierSpace enum) */
#define MOD_DISP_SPACE_LOCAL 0
#define MOD_DISP_SPACE_GLOBAL 1

/* Texture extend modes (matching DNA_texture_types.h line 280-286)
 * CRITICAL: Values start at 1 due to backward compatibility! */
#define TEX_EXTEND 1
#define TEX_CLIP 2
#define TEX_REPEAT 3
#define TEX_CLIPCUBE 4
#define TEX_CHECKER 5

/* Box sampling helpers - GPU port of boxsampleclip() and boxsample() from texture_image.cc
 * Simplified: computes texel coverage weights per-pixel within the box region and
 * accumulates texel values using texelFetch. Handles REPEAT and EXTEND wrapping.
 */
void boxsample_gpu(
    sampler2D displacement_texture,
    ivec2 tex_size,
    float min_tex_x, float min_tex_y,
    float max_tex_x, float max_tex_y,
    out vec4 result,
    bool talpha,
    bool imaprepeat,
    bool imapextend,
    bool tex_is_byte_buffer)
{
  result = vec4(0.0);
  float tot = 0.0;

  int startx = int(floor(min_tex_x));
  int endx = int(floor(max_tex_x));
  int starty = int(floor(min_tex_y));
  int endy = int(floor(max_tex_y));

  if (imapextend) {
    startx = max(startx, 0);
    starty = max(starty, 0);
    endx = min(endx, tex_size.x - 1);
    endy = min(endy, tex_size.y - 1);
  }

  for (int y = starty; y <= endy; ++y) {
    // compute vertical overlap
    float y0 = max(min_tex_y, float(y));
    float y1 = min(max_tex_y, float(y + 1));
    float h = y1 - y0;
    if (h <= 0.0) {
      continue;
    }

    for (int x = startx; x <= endx; ++x) {
      // compute horizontal overlap
      float x0 = max(min_tex_x, float(x));
      float x1 = min(max_tex_x, float(x + 1));
      float w = x1 - x0;
      if (w <= 0.0) {
        continue;
      }

      float area = w * h;

      int sx = x;
      int sy = y;

      if (imaprepeat) {
        sx %= tex_size.x;
        sx += (sx < 0) ? tex_size.x : 0;
        sy %= tex_size.y;
        sy += (sy < 0) ? tex_size.y : 0;
      }
      else if (imapextend) {
        sx = clamp(sx, 0, tex_size.x - 1);
        sy = clamp(sy, 0, tex_size.y - 1);
      }
      else {
        // In clip mode coordinates outside are already handled earlier, but clamp to be safe
        if (sx < 0 || sx >= tex_size.x || sy < 0 || sy >= tex_size.y) {
          continue;
        }
      }

      ivec2 texel = ivec2(sx, sy);
      vec4 col = texelFetch(displacement_texture, texel, 0);

      /* If the texture was uploaded from a byte buffer the CPU path
       * premultiplies RGB by alpha before filtering. Reproduce that
       * behaviour here so box filtering matches exactly. */
      if (tex_is_byte_buffer) {
        col.rgb *= col.a;
      }

      result += col * area;
      tot += area;
    }
  }

  if (tot > 0.0) {
    result /= tot;
  }
  else {
    result = vec4(0.0);
  }

  /* Leave alpha post-processing to outer shader path to avoid duplication. */
}
)GLSL";
}

static std::string get_vertex_normals() {
  return R"GLSL(
vec3 face_normal_object(int f) {
  int beg = face_offsets(f);
  int end = face_offsets(f + 1);
  int count = end - beg;

  /* Handle common polygon sizes explicitly to better match CPU behavior. */
  if (count == 3) {
    vec3 a = input_positions[corner_verts(beg + 0)].xyz;
    vec3 b = input_positions[corner_verts(beg + 1)].xyz;
    vec3 c = input_positions[corner_verts(beg + 2)].xyz;
    vec3 n = cross(b - a, c - a);
    float len = length(n);
    if (len <= 1e-20) {
      return vec3(0.0, 0.0, 1.0);
    }
    return n / len;
  }
  else if (count == 4) {
    vec3 v1 = input_positions[corner_verts(beg + 0)].xyz;
    vec3 v2 = input_positions[corner_verts(beg + 1)].xyz;
    vec3 v3 = input_positions[corner_verts(beg + 2)].xyz;
    vec3 v4 = input_positions[corner_verts(beg + 3)].xyz;
    /* Use diagonal cross-product method to match CPU `normal_quad_v3`. */
    vec3 d1 = v1 - v3;
    vec3 d2 = v2 - v4;
    vec3 n = cross(d1, d2);
    float len = length(n);
    if (len <= 1e-20) {
      return vec3(0.0, 0.0, 1.0);
    }
    return n / len;
  }

  /* Fallback: Newell's method for ngons */
  vec3 n = vec3(0.0);
  int v_prev_idx = corner_verts(end - 1);
  vec3 v_prev = input_positions[v_prev_idx].xyz;
  for (int i = beg; i < end; ++i) {
    int v_curr_idx = corner_verts(i);
    vec3 v_curr = input_positions[v_curr_idx].xyz;
    n += cross(v_prev, v_curr);
    v_prev = v_curr;
  }
  float len = length(n);
  if (len <= 1e-20) {
    return vec3(0.0, 0.0, 1.0);
  }
  return n / len;
}

vec3 compute_vertex_normal(uint v) {
  vec3 n_mesh;
  int beg = vert_to_face_offsets(int(v));
  int end = vert_to_face_offsets(int(v) + 1);
  vec3 n_accum = vec3(0.0);
  for (int i = beg; i < end; ++i) {
    int f = vert_to_face(i);
    n_accum += face_normal_object(f);
  }
  n_mesh = n_accum;

  n_mesh = normalize(n_mesh);
  return n_mesh;
}
)GLSL";
}

/* Part 2: Main function body (texture sampling + displacement logic) */
static std::string get_displace_shader_part2() {
  return R"GLSL(

void main() {
  uint v = gl_GlobalInvocationID.x;
  if (v >= deformed_positions.length()) {
    return;
  }

  vec4 co_in = input_positions[v];
  vec3 co = co_in.xyz;

  /* Get vertex group weight */
  float vgroup_weight = 1.0;
  if (vgroup_weights.length() > 0 && v < vgroup_weights.length()) {
    vgroup_weight = vgroup_weights[v];
  }

  /* Early exit if weight is zero (match CPU behavior) */
  if (vgroup_weight == 0.0) {
    deformed_positions[v] = co_in;
    return;
  }

  /* Compute delta (displacement amount) */
  float delta;
  
#ifdef HAS_TEXTURE
/* GPU port of Blender's texture sampling pipeline (texture_procedural.cc + texture_image.cc)
 * Flow: MOD_get_texture_coords() → do_2d_mapping() → imagewrap() → BRICONTRGB
 * This replicates the EXACT CPU path for pixel-perfect GPU/CPU match. */

struct TexResult {
  vec4 trgba;  /* RGBA color */
  float tin;   /* Intensity */
  bool talpha; /* Use alpha channel */
};

/* Sample texture using MOD_get_texture_coords() or input_positions when requested */
vec3 tex_coord = texture_coords[v].xyz;

if (mapping_use_input_positions) {
  vec3 in_pos = input_positions[v].xyz;
  if (tex_mapping == 0) { //MOD_DISP_MAP_LOCAL
    tex_coord = in_pos;
  } else if (tex_mapping == 1) { //MOD_DISP_MAP_GLOBAL
    vec4 w = object_to_world_mat * vec4(in_pos, 1.0);
    tex_coord = w.xyz;
  } else if (tex_mapping == 2) { //MOD_DISP_MAP_OBJECT
    vec4 w = object_to_world_mat * vec4(in_pos, 1.0);
    vec4 o = mapref_imat * w;
    tex_coord = o.xyz;
  } else {
    /* Fallback to precomputed coords (covers UV case and others) */
    tex_coord = texture_coords[v].xyz;
  }
}
else {
  tex_coord = texture_coords[v].xyz;
}

/* Sample texture (CPU uses boxsample for interpolation) */
TexResult texres;
texres.trgba = vec4(0.0);
texres.talpha = use_talpha;  /* From CPU line 211-213 */
bool should_displace = true;

/* Step 1: FLAT mapping (normalize [-1,1] → [0,1]) */
float fx = (tex_coord.x + 1.0) / 2.0;
float fy = (tex_coord.y + 1.0) / 2.0;
  
/* Get texture size for pixel-space calculations */
ivec2 tex_size = textureSize(displacement_texture, 0);
  
/* Step 2: do_2d_mapping() - REPEAT scaling + MIRROR (matching CPU line 501-527) */
if (tex_extend == TEX_REPEAT) {
  float origf_x = fx;
  float origf_y = fy;
  
  /* Repeat X */
  if (tex_repeat.x > 1.0) {
    fx *= tex_repeat.x;
    if (fx > 1.0) {
      fx -= float(int(fx));
    }
    else if (fx < 0.0) {
      fx += 1.0 - float(int(fx));
    }
    
    /* Mirror X if needed */
    if (tex_xmir) {
      int orig = int(floor(origf_x * tex_repeat.x));
      if ((orig & 1) != 0) {
        fx = 1.0 - fx;
      }
    }
  }
  
  /* Repeat Y */
  if (tex_repeat.y > 1.0) {
    fy *= tex_repeat.y;
    if (fy > 1.0) {
      fy -= float(int(fy));
    }
    else if (fy < 0.0) {
      fy += 1.0 - float(int(fy));
    }
    
    /* Mirror Y if needed */
    if (tex_ymir) {
      int orig = int(floor(origf_y * tex_repeat.y));
      if ((orig & 1) != 0) {
        fy = 1.0 - fy;
      }
    }
  }
}

/* Step 3: do_2d_mapping() - CROP (matching CPU line 528-537) */
if (tex_crop.x != 0.0 || tex_crop.z != 1.0) {
  float fac1 = tex_crop.z - tex_crop.x;
  fx = tex_crop.x + fx * fac1;
}
if (tex_crop.y != 0.0 || tex_crop.w != 1.0) {
  float fac1 = tex_crop.w - tex_crop.y;
  fy = tex_crop.y + fy * fac1;
}

/* Step 4: imagewrap() - TEX_IMAROT (swap X/Y) AFTER crop (matching CPU line 120-122)
 * CRITICAL: This MUST happen AFTER crop and BEFORE TEX_CHECKER! */
if (tex_flip_axis) {
  float temp = fx;
  fx = fy;
  fy = temp;
}

/* Step 5: imagewrap() - TEX_CHECKER filtering (matching CPU line 124-153)
 * Applied AFTER repeat/crop/swap to ensure correct tile detection */
  if (tex_extend == TEX_CHECKER) {
    /* Calculate tile coordinates from normalized UV coordinates (after repeat/crop)
     * xs = int(floor(fx)), ys = int(floor(fy)) */
    int xs = int(floor(fx));
    int ys = int(floor(fy));
    int tile_parity = (xs + ys) & 1;  /* 1 = odd tile, 0 = even tile */
    
    /* Apply checker odd/even filter (CPU texture_image.cc line 98-111)
     * NOTE: CPU logic uses inverted flags!
     * tex_checker_odd = true means "TEX_CHECKER_ODD flag is NOT SET"
     *                              → hide EVEN tiles
     * tex_checker_even = true means "TEX_CHECKER_EVEN flag is NOT SET"  
     *                               → hide ODD tiles */
    bool show_tile = true;
    
    if (tex_checker_odd && (tile_parity == 0)) {
      show_tile = false;  /* Hide EVEN tiles when ODD flag not set */
    }
    if (tex_checker_even && (tile_parity == 1)) {
      show_tile = false;  /* Hide ODD tiles when EVEN flag not set */
    }
    
    if (!show_tile) {
      texres.trgba = vec4(0.0);
      should_displace = false;
    }
    
    /* Normalize to fractional part within the tile */
    fx -= float(xs);
    fy -= float(ys);
    
    /* Scale checker pattern if needed (CPU line 113-117)
     * scale around center, (0.5, 0.5) */
    if (tex_checkerdist < 1.0) {
      fx = (fx - 0.5) / (1.0 - tex_checkerdist) + 0.5;
      fy = (fy - 0.5) / (1.0 - tex_checkerdist) + 0.5;
    }
  }
  
  /* Compute integer pixel coordinates (CPU line 157-158)
   * x = xi = int(floorf(fx * ibuf->x)); */
  int x = int(floor(fx * float(tex_size.x)));
  int y = int(floor(fy * float(tex_size.y)));
  int xi = x;  /* Save original for interpolation fix later */
  int yi = y;
  
  /* EARLY RETURN for CLIP/CLIPCUBE (CPU line 160-175) */
  if (tex_extend == TEX_CLIP) {
    if (x < 0 || y < 0 || x >= tex_size.x || y >= tex_size.y) {
      /* Early exit: no displacement */
      deformed_positions[v] = co_in;
      return;
    }
  }
  else if (tex_extend == TEX_CLIPCUBE) {
    if (x < 0 || y < 0 || x >= tex_size.x || y >= tex_size.y ||
        tex_coord.z < -1.0 || tex_coord.z > 1.0) {
      /* Early exit: no displacement */
      deformed_positions[v] = co_in;
      return;
    }
  }
  else if (tex_extend == TEX_CHECKER) {
    if (x < 0 || y < 0 || x >= tex_size.x || y >= tex_size.y) {
      texres.trgba = vec4(0.0);
      should_displace = false;
    }
  }
  else {
    /* EXTEND or REPEAT mode: wrap/clamp coordinates (CPU line 176-202) */
    if (tex_extend == TEX_EXTEND) {
      x = (x >= tex_size.x) ? (tex_size.x - 1) : ((x < 0) ? 0 : x);
    }
    else {
      /* REPEAT */
      x = x % tex_size.x;
      if (x < 0) x += tex_size.x;
    }
    
    if (tex_extend == TEX_EXTEND) {
      y = (y >= tex_size.y) ? (tex_size.y - 1) : ((y < 0) ? 0 : y);
    }
    else {
      /* REPEAT */
      y = y % tex_size.y;
      if (y < 0) y += tex_size.y;
    }
  }
  
  /* Now sample texture (CPU line 215-241: interpolate/no filtering)
   * Normalize pixel coords back to [0,1] for texture() sampling */
  
  /* Remap coordinates for interpolation (CPU line 220-223):
   * "Important that this value is wrapped #27782" */
  fx -= float(xi - x) / float(tex_size.x);
  fy -= float(yi - y) / float(tex_size.y);
  
  /* Normalize UVs to [0,1] for texture sampling */
  vec2 uv_normalized = vec2(fx, fy);
  
  if (tex_interpol) {
    /* Interpolated sampling (boxsample) - use GPU boxsample implementation */
    float filterx = (0.5 * tex_filtersize) / float(tex_size.x);
    float filtery = (0.5 * tex_filtersize) / float(tex_size.y);

    /* fx,fy already adjusted above (remap for interpolation) */
    float min_tex_x = (fx - filterx) * float(tex_size.x);
    float min_tex_y = (fy - filtery) * float(tex_size.y);
    float max_tex_x = (fx + filterx) * float(tex_size.x);
    float max_tex_y = (fy + filtery) * float(tex_size.y);

    boxsample_gpu(displacement_texture,
                  tex_size,
                  min_tex_x,
                  min_tex_y,
                  max_tex_x,
                  max_tex_y,
                  texres.trgba,
                  texres.talpha,
                  (tex_extend == TEX_REPEAT),
                  (tex_extend == TEX_EXTEND),
                  tex_is_byte_buffer);
  } else {
    /* No filtering (CPU line 242: ibuf_get_color) */
    ivec2 px_coord = ivec2(x, y);
    px_coord = clamp(px_coord, ivec2(0), tex_size - 1);
    /* Exact texel fetch to match CPU ibuf_get_color (no filtering). */
    texres.trgba = texelFetch(displacement_texture, px_coord, 0);
    /* If texture was uploaded from byte buffer, the CPU path premultiplies bytes
     * (rgb *= alpha). Reproduce that here. */
    if (tex_is_byte_buffer) {
      texres.trgba.rgb *= texres.trgba.a;
    }
  }

  if (!should_displace) {
    texres.trgba = vec4(0.0);
  }
  
  /* Compute intensity (CPU line 244-253) */
  if (texres.talpha) {
    texres.tin = texres.trgba.a;
  }
  else if (tex_calcalpha) {
    texres.tin = max(max(texres.trgba.r, texres.trgba.g), texres.trgba.b);
    texres.trgba.a = texres.tin;
  }
  else {
    texres.tin = 1.0;
    texres.trgba.a = 1.0;
  }
  
  if (tex_negalpha) {
    texres.trgba.a = 1.0 - texres.trgba.a;
  }
  
  /* De-pre-multiply (CPU line 260-264) */
  if (texres.trgba.a != 1.0 && texres.trgba.a > 1e-4 && !tex_calcalpha) {
    float inv_alpha = 1.0 / texres.trgba.a;
    texres.trgba.rgb *= inv_alpha;
  }
  
  /* BRICONTRGB macro (texture_common.h) - CPU line 270 */
  vec3 rgb = texres.trgba.rgb;
  rgb.r = tex_rfac * ((rgb.r - 0.5) * tex_contrast + tex_bright - 0.5);
  rgb.g = tex_gfac * ((rgb.g - 0.5) * tex_contrast + tex_bright - 0.5);
  rgb.b = tex_bfac * ((rgb.b - 0.5) * tex_contrast + tex_bright - 0.5);
  
  if (!tex_no_clamp) {
    rgb = max(rgb, vec3(0.0));
  }
  
  /* Apply saturation */
  if (tex_saturation != 1.0) {
    float cmax = max(max(rgb.r, rgb.g), rgb.b);
    float cmin = min(min(rgb.r, rgb.g), rgb.b);
    float delta_hsv = cmax - cmin;
    
    float h = 0.0, s = 0.0, v = cmax;
    
    if (delta_hsv > 1e-20) {
      s = delta_hsv / (cmax + 1e-20);
      
      if (rgb.r >= cmax) {
        h = (rgb.g - rgb.b) / delta_hsv;
      } else if (rgb.g >= cmax) {
        h = 2.0 + (rgb.b - rgb.r) / delta_hsv;
      } else {
        h = 4.0 + (rgb.r - rgb.g) / delta_hsv;
      }
      
      h /= 6.0;
      if (h < 0.0) h += 1.0;
    }
    
    s *= tex_saturation;
    
    float nr = abs(h * 6.0 - 3.0) - 1.0;
    float ng = 2.0 - abs(h * 6.0 - 2.0);
    float nb = 2.0 - abs(h * 6.0 - 4.0);
    
    nr = clamp(nr, 0.0, 1.0);
    ng = clamp(ng, 0.0, 1.0);
    nb = clamp(nb, 0.0, 1.0);
    
    rgb.r = ((nr - 1.0) * s + 1.0) * v;
    rgb.g = ((ng - 1.0) * s + 1.0) * v;
    rgb.b = ((nb - 1.0) * s + 1.0) * v;
    
    if (tex_saturation > 1.0 && !tex_no_clamp) {
      rgb = max(rgb, vec3(0.0));
    }
  }
  
  /* Linear → sRGB conversion (for intensity calculation)
   * CRITICAL: GPU textures are ALWAYS loaded as LINEAR!
   * If source image was sRGB, GPU auto-converted to linear.
   * We only apply linear→sRGB if image was ORIGINALLY linear. */
  vec3 srgb_rgb;
  if (tex_skip_srgb_conversion) { // For movies, choose working space colorspace to have same displacement as CPU
    /* Image was sRGB or ?, use as-is */
    srgb_rgb = rgb;
  }
  else {
    /* Image was linear, apply linear→sRGB conversion.
     * Clamp to >=0 before pow to avoid NaNs from tiny negative values and
     * ensure consistent behavior with CPU code that clamps prior to conversion. */
    vec3 rgb_clamped = max(rgb, vec3(0.0));

    srgb_rgb.r = linearrgb_to_srgb(rgb_clamped.r);
    srgb_rgb.g = linearrgb_to_srgb(rgb_clamped.g);
    srgb_rgb.b = linearrgb_to_srgb(rgb_clamped.b);
  }
  
  float tex_value = (srgb_rgb.r + srgb_rgb.g + srgb_rgb.b) * (1.0 / 3.0);
  
  if (tex_flipblend) {
    tex_value = 1.0 - tex_value;
  }

  float s = strength * vgroup_weight;
  vec3 rgb_displacement = (srgb_rgb - vec3(midlevel)) * s;
  delta = (tex_value - midlevel) * s;
#else
  /* Fixed delta (no texture) */
  delta = (1.0 - midlevel) * strength * vgroup_weight;
  vec3 rgb_displacement = vec3(0.0);  /* Not used without texture */
#endif
  
  /* Clamp delta to prevent extreme deformations */
  delta = clamp(delta, -10000.0, 10000.0);

  /* Apply displacement based on direction */
  if (direction == MOD_DISP_DIR_X) {
    if (use_global) {
      /* Global X axis */
      co += delta * vec3(local_mat[0][0], local_mat[1][0], local_mat[2][0]);
    } else {
      /* Local X axis */
      co.x += delta;
    }
  }
  else if (direction == MOD_DISP_DIR_Y) {
    if (use_global) {
      /* Global Y axis */
      co += delta * vec3(local_mat[0][1], local_mat[1][1], local_mat[2][1]);
    } else {
      /* Local Y axis */
      co.y += delta;
    }
  }
  else if (direction == MOD_DISP_DIR_Z) {
    if (use_global) {
      /* Global Z axis */
      co += delta * vec3(local_mat[0][2], local_mat[1][2], local_mat[2][2]);
    } else {
      /* Local Z axis */
      co.z += delta;
    }
  }
  else if (direction == MOD_DISP_DIR_NOR) {
    vec3 n_mesh = compute_vertex_normal(v);
    /* Displacement along vertex normal
     * This matches CPU behavior and is acceptable for most use cases. */
    co += delta * normalize(n_mesh);
  }
  else if (direction == MOD_DISP_DIR_CLNOR) {
    /* Displacement along custom loop normals (Simplification -> same than DISP_DIR_NOR) */
    vec3 n_mesh = compute_vertex_normal(v);
    co += delta * normalize(n_mesh);
  }
  else if (direction == MOD_DISP_DIR_RGB_XYZ) {
    /* Displacement using RGB as (X, Y, Z) vector
     * Each RGB component controls displacement along its respective axis
     * R → X displacement, G → Y displacement, B → Z displacement */
#ifdef HAS_TEXTURE
    if (use_global) {
      /* Transform local displacement vector to global space */
      vec3 global_disp = vec3(
        dot(vec3(local_mat[0][0], local_mat[0][1], local_mat[0][2]), rgb_displacement),
        dot(vec3(local_mat[1][0], local_mat[1][1], local_mat[1][2]), rgb_displacement),
        dot(vec3(local_mat[2][0], local_mat[2][1], local_mat[2][2]), rgb_displacement)
      );
      co += global_disp;
    } else {
      /* Local space: directly apply RGB as (X, Y, Z) */
      co += rgb_displacement;
    }
#else
    /* No texture: cannot use RGB_XYZ mode, fallback to no displacement */
    /* (This matches CPU behavior: RGB_XYZ requires texture) */
#endif
  }

  deformed_positions[v] = vec4(co, 1.0);
}
)GLSL";
}

/* Final assembly function - concatenates both parts */
static std::string get_displace_compute_src() {
  return get_displace_shader_part1() + get_vertex_normals() + get_displace_shader_part2();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name DisplaceManager Public API
 * \{ */

DisplaceManager &DisplaceManager::instance()
{
  static DisplaceManager manager;
  return manager;
}

DisplaceManager::DisplaceManager() : impl_(new Impl()) {}
DisplaceManager::~DisplaceManager() {}

uint32_t DisplaceManager::compute_displace_hash(const Mesh *mesh_orig,
                                                const DisplaceModifierData *dmd)
{
  if (!mesh_orig || !dmd) {
    return 0;
  }

  uint32_t hash = 0;

  /* Hash vertex count */
  hash = BLI_hash_int_2d(hash, mesh_orig->verts_num);

  /* Hash direction mode */
  hash = BLI_hash_int_2d(hash, int(dmd->direction));

  /* Hash space mode */
  hash = BLI_hash_int_2d(hash, int(dmd->space));

  /* Hash vertex group name */
  if (dmd->defgrp_name[0] != '\0') {
    hash = BLI_hash_string(dmd->defgrp_name);
  }

  /* Hash invert flag */
  hash = BLI_hash_int_2d(hash, int(dmd->flag & MOD_DISP_INVERT_VGROUP));

  /* Hash texture mapping mode */
  hash = BLI_hash_int_2d(hash, int(dmd->texmapping));

  const bool has_texture = (dmd->texture && dmd->texture->type == TEX_IMAGE &&
                            dmd->texture->ima);
  hash = BLI_hash_int_2d(hash, has_texture ? 1 : 0);

  if (has_texture) {
    hash = BLI_hash_int_2d(hash, int(reinterpret_cast<uintptr_t>(dmd->texture->ima)));
    hash = BLI_hash_int_2d(hash, int(dmd->texture->ima->source));
    hash = BLI_hash_int_2d(hash, int(reinterpret_cast<uintptr_t>(dmd->texture)));
    hash = BLI_hash_int_2d(hash, int(reinterpret_cast<uintptr_t>(&dmd->texture->iuser)));
  }

  /* Hash deform_verts pointer (detects vertex group changes) */
  blender::Span<MDeformVert> dverts = mesh_orig->deform_verts();
  hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(dverts.data())));

  /* Note: strength and midlevel are runtime uniforms, not hashed */

  return hash;
}

void DisplaceManager::ensure_static_resources(const DisplaceModifierData *dmd,
                                             Object *deform_ob,
                                             Mesh *orig_mesh,
                                             uint32_t pipeline_hash)
{
  if (!orig_mesh || !dmd) {
    return;
  }

  /* Use composite key (mesh, modifier_uid) to support multiple Displace modifiers per mesh */
  Impl::MeshModifierKey key{orig_mesh, uint32_t(dmd->modifier.persistent_uid)};
  Impl::MeshStaticData &msd = impl_->static_map.lookup_or_add_default(key);

  const bool first_time = (msd.last_verified_hash == 0);
  const bool hash_changed = (pipeline_hash != msd.last_verified_hash);
  const bool gpu_invalidated = msd.pending_gpu_setup;

  if (!first_time && !hash_changed && !gpu_invalidated) {
    return;
  }

  msd.last_verified_hash = pipeline_hash;
  msd.verts_num = orig_mesh->verts_num;
  msd.deformed = deform_ob;

  if (first_time || hash_changed) {
    msd.pending_gpu_setup = true;
    msd.gpu_setup_attempts = 0;
  }

  /* Extract vertex group weights */
  msd.vgroup_weights.clear();
  if (dmd->defgrp_name[0] != '\0') {
    const int defgrp_index = BKE_id_defgroup_name_index(&orig_mesh->id, dmd->defgrp_name);
    if (defgrp_index != -1) {
      blender::Span<MDeformVert> dverts = orig_mesh->deform_verts();

      /* Check if dverts is empty to prevent crash
       * When ALL vertex groups are deleted, dverts.data() == nullptr.
       * Accessing dverts[v] would crash with Access Violation. */
      if (!dverts.is_empty()) {
        msd.vgroup_weights.resize(orig_mesh->verts_num, 0.0f);
        const bool invert_vgroup = (dmd->flag & MOD_DISP_INVERT_VGROUP) != 0;

        for (int v = 0; v < orig_mesh->verts_num; ++v) {
          const MDeformVert &dvert = dverts[v];
          float weight = BKE_defvert_find_weight(&dvert, defgrp_index);
          msd.vgroup_weights[v] = invert_vgroup ? 1.0f - weight : weight;
        }
      }
    }
  }

  /* Extract texture coordinates (if texture is present) */
  msd.tex_coords.clear();
  if (dmd->texture && dmd->texture->type == TEX_IMAGE) {
    /* Use the same MOD_get_texture_coords() function as the CPU modifier
     * to guarantee identical behavior for all mapping modes (LOCAL/GLOBAL/OBJECT/UV) */
    const int verts_num = orig_mesh->verts_num;
    float (*tex_co)[3] = MEM_malloc_arrayN<float[3]>(verts_num, "displace_tex_coords");

    MOD_get_texture_coords(
        reinterpret_cast<MappingInfoModifierData *>(const_cast<DisplaceModifierData *>(dmd)),
        nullptr,  // ctx (not needed for coordinate calculation)
        deform_ob,
        orig_mesh,
        nullptr,  // cos (use original positions)
        tex_co);
    
    /* Copy to msd.tex_coords vector */
    msd.tex_coords.resize(verts_num);
    for (int v = 0; v < verts_num; ++v) {
      msd.tex_coords[v] = blender::float3(tex_co[v]);
    }
    
    MEM_freeN(tex_co);
  }
}

blender::gpu::StorageBuf *DisplaceManager::dispatch_deform(const DisplaceModifierData *dmd,
                                                           Depsgraph *depsgraph,
                                                           Object *deformed_eval,
                                                           MeshBatchCache *cache,
                                                           blender::gpu::StorageBuf *ssbo_in)
{
  if (!dmd) {
    return nullptr;
  }

  using namespace blender::draw;

  Mesh *mesh_owner = (cache && cache->mesh_owner) ? cache->mesh_owner : nullptr;
  if (!mesh_owner) {
    return nullptr;
  }

  /* Use composite key (mesh, modifier_uid) to support multiple Displace modifiers per mesh */
  Impl::MeshModifierKey key{mesh_owner, uint32_t(dmd->modifier.persistent_uid)};
  Impl::MeshStaticData *msd_ptr = impl_->static_map.lookup_ptr(key);
  if (!msd_ptr) {
    return nullptr;
  }
  Impl::MeshStaticData &msd = *msd_ptr;

  /* GPU setup retry logic */
  const int MAX_ATTEMPTS = 3;
  if (msd.pending_gpu_setup) {
    if (msd.gpu_setup_attempts == 0) {
      msd.gpu_setup_attempts = 1;
      return nullptr;
    }
    if (msd.gpu_setup_attempts >= MAX_ATTEMPTS) {
      msd.pending_gpu_setup = false;
      msd.gpu_setup_attempts = 0;
      return nullptr;
    }
    msd.gpu_setup_attempts++;
  }

  MeshGpuInternalResources *ires = BKE_mesh_gpu_internal_resources_ensure(mesh_owner);
  if (!ires) {
    return nullptr;
  }

  /* GPU setup successful! Clear pending flag. */
  if (msd.pending_gpu_setup) {
    msd.pending_gpu_setup = false;
    msd.gpu_setup_attempts = 0;
  }

  /* Create unique buffer keys per modifier instance using composite key hash */
  const std::string key_prefix = "displace_" + std::to_string(key.hash()) + "_";
  const std::string key_vgroup = key_prefix + "vgroup_weights";
  const std::string key_out = key_prefix + "output";

  /* Upload vertex group weights SSBO */
  blender::gpu::StorageBuf *ssbo_vgroup = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_vgroup);

  if (!msd.vgroup_weights.empty()) {
    if (!ssbo_vgroup) {
      const size_t size_vgroup = msd.vgroup_weights.size() * sizeof(float);
      ssbo_vgroup = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_vgroup, size_vgroup);
      if (ssbo_vgroup) {
        GPU_storagebuf_update(ssbo_vgroup, msd.vgroup_weights.data());
      }
    }
  }
  else {
    /* No vertex group: create dummy buffer (length=0 triggers default weight=1.0 in shader) */
    if (!ssbo_vgroup) {
      ssbo_vgroup = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_vgroup, sizeof(float));
      if (ssbo_vgroup) {
        float dummy = 1.0f;
        GPU_storagebuf_update(ssbo_vgroup, &dummy);
      }
    }
  }

  /* Upload texture coordinates SSBO and prepare texture binding */
  const std::string key_texcoords = key_prefix + "tex_coords";
  blender::gpu::StorageBuf *ssbo_texcoords = nullptr;
  blender::gpu::Texture *gpu_texture = nullptr;
  bool has_texture = false;

  if (dmd->texture && dmd->texture->type == TEX_IMAGE && dmd->texture->ima) {
    Image *ima = dmd->texture->ima;
    Tex *tex = dmd->texture;
    
    /* Setup ImageUser with correct frame for ImageSequence/Movies
     * CRITICAL: ImageUser.framenr must be updated from scene frame for animation!
     * The CPU path (MOD_init_texture) calls BKE_texture_fetch_images_for_pool() which
     * updates iuser.framenr. We must replicate this for GPU. */
    if (ima && ima->runtime && tex) {
      ImageUser iuser = tex->iuser;  /* Start with texture's ImageUser */
      
      /* For animated textures, update frame number from current scene
       * This is CRITICAL for ImageSequence/Movie playback! */
      if (ELEM(ima->source, IMA_SRC_SEQUENCE, IMA_SRC_MOVIE)) {
        /* Get scene from depsgraph (same as CPU modifier evaluator) and compute
         * the correct image user frame using the shared utility which handles
         * offsets, cycling and ranges. */
        Scene *scene = DEG_get_evaluated_scene(depsgraph);
        if (scene) {
          BKE_image_user_frame_calc(ima, &iuser, int(scene->r.cfra));
        }
      }
      
      /* Ensure GPU texture is loaded for this frame */
      gpu_texture = BKE_image_get_gpu_texture(ima, &iuser);
      
      if (gpu_texture && !msd.tex_coords.empty()) {
        has_texture = true;
        
        /* Upload texture coordinates SSBO */
        ssbo_texcoords = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_texcoords);
        
        if (!ssbo_texcoords) {
          const size_t size_texcoords = msd.tex_coords.size() * sizeof(blender::float4);
          ssbo_texcoords = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_texcoords, size_texcoords);
          if (ssbo_texcoords) {
            /* Pad float3 to float4 for GPU alignment */
            std::vector<blender::float4> padded_texcoords(msd.tex_coords.size());
            for (size_t i = 0; i < msd.tex_coords.size(); ++i) {
              padded_texcoords[i] = blender::float4(
                  msd.tex_coords[i].x, msd.tex_coords[i].y, msd.tex_coords[i].z, 1.0f);
            }
            GPU_storagebuf_update(ssbo_texcoords, padded_texcoords.data());
          }
        }
      }
    }
  }

  /* Create output SSBO */
  const size_t size_out = msd.verts_num * sizeof(float) * 4;
  blender::gpu::StorageBuf *ssbo_out = BKE_mesh_gpu_internal_ssbo_ensure(
      mesh_owner, key_out, size_out);
  if (!ssbo_out || !ssbo_in) {
    return nullptr;
  }

  /* Compute transformation matrix (for global space) */
  float local_mat[4][4];
  const bool use_global = (dmd->space == MOD_DISP_SPACE_GLOBAL);
  if (use_global) {
    copy_m4_m4(local_mat, deformed_eval->object_to_world().ptr());
  }
  else {
    unit_m4(local_mat);
  }

  /* Create shader */
  using namespace blender::gpu::shader;
  ShaderCreateInfo info("pyGPU_Shader");
  info.local_group_size(256, 1, 1);
  
  /* Build shader source with conditional texture support */
  std::string shader_src;
  
  if (has_texture) {
    shader_src += "#define HAS_TEXTURE\n";
  }
  shader_src += get_displace_compute_src();

  using namespace blender::bke;
  auto &mesh_data = MeshGPUCacheManager::get().mesh_cache()[mesh_owner];
  if (!mesh_data.topology.ssbo) {
    if (!BKE_mesh_gpu_topology_create(mesh_owner, mesh_data.topology) ||
        !BKE_mesh_gpu_topology_upload(mesh_data.topology))
    {
      return nullptr;
    }
  }
  std::string glsl_accessors = BKE_mesh_gpu_topology_glsl_accessors_string(mesh_data.topology);
  
  info.compute_source_generated = glsl_accessors + shader_src;

  /* Bindings */
  info.storage_buf(0, Qualifier::write, "vec4", "deformed_positions[]");
  info.storage_buf(1, Qualifier::read, "vec4", "input_positions[]");
  info.storage_buf(2, Qualifier::read, "float", "vgroup_weights[]");
  if (has_texture) {
    info.storage_buf(3, Qualifier::read, "vec4", "texture_coords[]");
    info.sampler(0, ImageType::Float2D, "displacement_texture");
  }
  info.storage_buf(4, Qualifier::read, "int", "topo[]");

  /* Push constants */
  info.push_constant(Type::float4x4_t, "local_mat");
  info.push_constant(Type::float_t, "strength");
  info.push_constant(Type::float_t, "midlevel");
  info.push_constant(Type::int_t, "direction");
  info.push_constant(Type::bool_t, "use_global");
  
  /* Texture processing parameters (for BRICONTRGB and de-premultiply) */
  if (has_texture) {
    info.push_constant(Type::bool_t, "use_talpha");      /* Enable de-premultiply */
    info.push_constant(Type::bool_t, "tex_calcalpha");   /* TEX_CALCALPHA */
    info.push_constant(Type::bool_t, "tex_negalpha");    /* TEX_NEGALPHA */
    info.push_constant(Type::float_t, "tex_bright");     /* Tex->bright */
    info.push_constant(Type::float_t, "tex_contrast");   /* Tex->contrast */
    info.push_constant(Type::float_t, "tex_saturation"); /* Tex->saturation */
    info.push_constant(Type::float_t, "tex_rfac");       /* Tex->rfac */
    info.push_constant(Type::float_t, "tex_gfac");       /* Tex->gfac */
    info.push_constant(Type::float_t, "tex_bfac");       /* Tex->bfac */
    info.push_constant(Type::bool_t, "tex_no_clamp");    /* Tex->flag & TEX_NO_CLAMP */
    info.push_constant(Type::int_t, "tex_extend");       /* Tex->extend (wrap mode) */
    info.push_constant(Type::float4_t, "tex_crop");      /* (cropxmin, cropymin, cropxmax, cropymax) */
    info.push_constant(Type::float2_t, "tex_repeat");    /* (xrepeat, yrepeat) */
    info.push_constant(Type::bool_t, "tex_xmir");        /* TEX_REPEAT_XMIR */
    info.push_constant(Type::bool_t, "tex_ymir");        /* TEX_REPEAT_YMIR */
    info.push_constant(Type::bool_t, "tex_interpol");    /* TEX_INTERPOL */
    info.push_constant(Type::float_t, "tex_filtersize"); /* Tex->filtersize for boxsample */
    info.push_constant(Type::bool_t, "tex_checker_odd"); /* TEX_CHECKER_ODD */
    info.push_constant(Type::bool_t, "tex_checker_even");/* TEX_CHECKER_EVEN */
    info.push_constant(Type::float_t, "tex_checkerdist");/* Tex->checkerdist */
    info.push_constant(Type::bool_t, "tex_flipblend");   /* TEX_FLIPBLEND */
    info.push_constant(Type::bool_t, "tex_flip_axis");   /* TEX_IMAROT (flip X/Y) */
    info.push_constant(Type::bool_t, "tex_skip_srgb_conversion"); /* Skip linear→sRGB if image already sRGB */
    /* Mapping controls (when mapping_use_input_positions==true shader will
     * compute texture coords from input_positions[] instead of using
     * precomputed texture_coords[]). UV mapping remains CPU-side. */
    info.push_constant(Type::int_t, "tex_mapping");
    info.push_constant(Type::bool_t, "mapping_use_input_positions");
    info.push_constant(Type::float4x4_t, "object_to_world_mat");
    info.push_constant(Type::float4x4_t, "mapref_imat");
    info.push_constant(Type::bool_t, "tex_is_byte_buffer"); /* Image data originally bytes (needs premultiply) */
  }
  BKE_mesh_gpu_topology_add_specialization_constants(info, mesh_data.topology);

  blender::gpu::Shader *shader = BKE_mesh_gpu_internal_shader_ensure(
      mesh_owner, "displace_compute_v2", info);
  if (!shader) {
    return nullptr;
  }

  /* Bind and dispatch */
  const blender::gpu::shader::SpecializationConstants *constants =
      &GPU_shader_get_default_constant_state(shader);
  GPU_shader_bind(shader, constants);

  GPU_storagebuf_bind(ssbo_out, 0);
  GPU_storagebuf_bind(ssbo_in, 1);
  if (ssbo_vgroup) {
    GPU_storagebuf_bind(ssbo_vgroup, 2);
  }
  
  /* Note: vertex normals SSBO removed — shader computes vertex normal from topology. */
  
  /* Bind texture coordinates and texture (if present) */
  if (has_texture) {
    if (ssbo_texcoords) {
      GPU_storagebuf_bind(ssbo_texcoords, 3);
    }
    if (gpu_texture) {
      GPU_texture_bind(gpu_texture, 0);
    }
  }

  GPU_storagebuf_bind(mesh_data.topology.ssbo, 4);

  /* Set uniforms (runtime parameters) */
  GPU_shader_uniform_mat4(shader, "local_mat", (const float(*)[4])local_mat);
  GPU_shader_uniform_1f(shader, "strength", dmd->strength);
  GPU_shader_uniform_1f(shader, "midlevel", dmd->midlevel);
  GPU_shader_uniform_1i(shader, "direction", int(dmd->direction));
  GPU_shader_uniform_1b(shader, "use_global", use_global);
  
  /* Set texture processing parameters (if texture is present) */
  if (has_texture) {
    Tex *tex = dmd->texture;
    Image *ima = tex->ima;
    
    /* Determine if we should use de-premultiply (talpha flag logic from imagewrap)
     * talpha is set when: TEX_USEALPHA && alpha_mode != IGNORE && !TEX_CALCALPHA */
    bool use_talpha = false;
    if ((tex->imaflag & TEX_USEALPHA) && ima && (ima->alpha_mode != IMA_ALPHA_IGNORE)) {
      if ((tex->imaflag & TEX_CALCALPHA) == 0) {
        use_talpha = true;
      }
    }
    
    GPU_shader_uniform_1b(shader, "use_talpha", use_talpha);
    GPU_shader_uniform_1b(shader, "tex_calcalpha", (tex->imaflag & TEX_CALCALPHA) != 0);
    GPU_shader_uniform_1b(shader, "tex_negalpha", (tex->flag & TEX_NEGALPHA) != 0);
    GPU_shader_uniform_1f(shader, "tex_bright", tex->bright);
    GPU_shader_uniform_1f(shader, "tex_contrast", tex->contrast);
    GPU_shader_uniform_1f(shader, "tex_saturation", tex->saturation);
    GPU_shader_uniform_1f(shader, "tex_rfac", tex->rfac);
    GPU_shader_uniform_1f(shader, "tex_gfac", tex->gfac);
    GPU_shader_uniform_1f(shader, "tex_bfac", tex->bfac);
    GPU_shader_uniform_1b(shader, "tex_no_clamp", (tex->flag & TEX_NO_CLAMP) != 0);
    GPU_shader_uniform_1i(shader, "tex_extend", int(tex->extend));
    
    /* Upload crop parameters (xmin, ymin, xmax, ymax) */
    float crop[4] = {tex->cropxmin, tex->cropymin, tex->cropxmax, tex->cropymax};
    GPU_shader_uniform_4f(shader, "tex_crop", crop[0], crop[1], crop[2], crop[3]);
    
    /* Upload repeat/mirror flags */
    GPU_shader_uniform_2f(shader, "tex_repeat", float(tex->xrepeat), float(tex->yrepeat));
    GPU_shader_uniform_1b(shader, "tex_xmir", (tex->flag & TEX_REPEAT_XMIR) != 0);
    GPU_shader_uniform_1b(shader, "tex_ymir", (tex->flag & TEX_REPEAT_YMIR) != 0);
    GPU_shader_uniform_1b(shader, "tex_interpol", (tex->imaflag & TEX_INTERPOL) != 0);
    GPU_shader_uniform_1b(shader, "tex_checker_odd", (tex->flag & TEX_CHECKER_ODD) == 0);
    GPU_shader_uniform_1b(shader, "tex_checker_even", (tex->flag & TEX_CHECKER_EVEN) == 0);
    GPU_shader_uniform_1b(shader, "tex_flipblend", (tex->flag & TEX_FLIPBLEND) != 0);
    GPU_shader_uniform_1b(shader, "tex_flip_axis", (tex->imaflag & TEX_IMAROT) != 0);
    GPU_shader_uniform_1f(shader, "tex_filtersize", tex->filtersize);

    /* Determine if GPU texture was uploaded from byte buffer data. If so, we need to
     * premultiply RGB by alpha to match CPU ibuf_get_color behavior for byte images. */
    /* Determine if texture was originally uploaded from a byte buffer.
     * Use Image flags rather than inspecting ImBuf to avoid extra cost. */
    bool tex_is_byte = false;
    if (ima) {
      ImageTile *tile = BKE_image_get_tile(ima, 0);
      /* If image is NOT high bitdepth and NOT a generated float image, assume byte buffer. */
      if (!(tile->gen_flag & IMA_GEN_FLOAT)) {
        tex_is_byte = true;
      }
    }
    
    /* Simple heuristic: skip linear→sRGB conversion on the GPU for movies
     * and image sequences (they are uploaded as linear by the GPU). For
     * other images we do not skip the conversion by default. */
    bool skip_srgb_conversion = false;
    if (ima) {
      if (ELEM(ima->source, IMA_SRC_SEQUENCE, IMA_SRC_MOVIE) || !tex_is_byte) {
        skip_srgb_conversion = true;
      }
    }
    GPU_shader_uniform_1b(shader, "tex_skip_srgb_conversion", skip_srgb_conversion);
    
    /* Checker pattern scaling parameter */
    GPU_shader_uniform_1f(shader, "tex_checkerdist", tex->checkerdist);
    GPU_shader_uniform_1b(shader, "tex_is_byte_buffer", tex_is_byte);
    /* Mapping controls: if UV mapping, keep false; otherwise allow using input_positions. */
    int tex_mapping = int(dmd->texmapping);
    bool mapping_use_input_positions = (tex_mapping != MOD_DISP_MAP_UV);
    GPU_shader_uniform_1i(shader, "tex_mapping", tex_mapping);
    GPU_shader_uniform_1b(shader, "mapping_use_input_positions", mapping_use_input_positions);

    /* Pass object->world matrix (fast copy) */
    float obj2w[4][4];
    memcpy(obj2w, deformed_eval->object_to_world().ptr(), sizeof(obj2w));
    GPU_shader_uniform_mat4(shader, "object_to_world_mat", obj2w);

    /* mapref_imat: compute inverse map reference for MOD_DISP_MAP_OBJECT when possible.
     * Falls back to identity when no map_object is set. This mirrors logic from MOD_get_texture_coords(). */
    float mapref_imat[4][4];
    if (dmd->texmapping == MOD_DISP_MAP_OBJECT && dmd->map_object != nullptr) {
      Object *map_object = dmd->map_object;
      if (dmd->map_bone[0] != '\0') {
        bPoseChannel *pchan = BKE_pose_channel_find_name(map_object->pose, dmd->map_bone);
        if (pchan) {
          float mat_bone_world[4][4];
          mul_m4_m4m4(mat_bone_world, map_object->object_to_world().ptr(), pchan->pose_mat);
          invert_m4_m4(mapref_imat, mat_bone_world);
        }
        else {
          invert_m4_m4(mapref_imat, map_object->object_to_world().ptr());
        }
      }
      else {
        invert_m4_m4(mapref_imat, map_object->object_to_world().ptr());
      }
    }
    else {
      unit_m4(mapref_imat);
    }
    GPU_shader_uniform_mat4(shader, "mapref_imat", mapref_imat);
  }


  const int group_size = 256;
  const int num_groups = (msd.verts_num + group_size - 1) / group_size;
  GPU_compute_dispatch(shader, num_groups, 1, 1, constants);

  /* Unbind texture */
  if (gpu_texture) {
    GPU_texture_unbind(gpu_texture);
  }

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  msd.pending_gpu_setup = false;
  msd.gpu_setup_attempts = 0;

  return ssbo_out;
}

void DisplaceManager::free_resources_for_mesh(Mesh *mesh)
{
  if (!mesh) {
    return;
  }

  /* Remove all entries for this mesh (may be multiple Displace modifiers) */
  Vector<Impl::MeshModifierKey> keys_to_remove;
  for (const auto &item : impl_->static_map.items()) {
    if (item.key.mesh == mesh) {
      keys_to_remove.append(item.key);
    }
  }

  for (const Impl::MeshModifierKey &key : keys_to_remove) {
    impl_->static_map.remove(key);
  }
}

void DisplaceManager::invalidate_all(Mesh *mesh)
{
  if (!mesh) {
    return;
  }

  BKE_mesh_gpu_internal_resources_free_for_mesh(mesh);

  /* Invalidate all Displace modifiers for this mesh */
  for (auto item : impl_->static_map.items()) {
    if (item.key.mesh == mesh) {
      /* Lookup again to get mutable reference */
      Impl::MeshStaticData *msd = impl_->static_map.lookup_ptr(item.key);
      if (msd) {
        msd->pending_gpu_setup = true;
        msd->gpu_setup_attempts = 0;
      }
    }
  }
}

void DisplaceManager::free_all()
{
  impl_->static_map.clear();
}

/** \} */
