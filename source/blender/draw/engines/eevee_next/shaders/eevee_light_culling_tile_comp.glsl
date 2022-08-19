
/**
 * 2D Culling pass for lights.
 * We iterate over all items and check if they intersect with the tile frustum.
 * Dispatch one thread per word.
 */

#pragma BLENDER_REQUIRE(common_view_lib.glsl)
#pragma BLENDER_REQUIRE(common_intersect_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_light_iter_lib.glsl)

/* ---------------------------------------------------------------------- */
/** \name Culling shapes extraction
 * \{ */

struct CullingTile {
  IsectFrustum frustum;
  vec4 bounds;
};

/* Corners are expected to be in viewspace so that the cone is starting from the origin.
 * Corner order does not matter. */
vec4 tile_bound_cone(vec3 v00, vec3 v01, vec3 v10, vec3 v11)
{
  v00 = normalize(v00);
  v01 = normalize(v01);
  v10 = normalize(v10);
  v11 = normalize(v11);
  vec3 center = normalize(v00 + v01 + v10 + v11);
  float angle_cosine = dot(center, v00);
  angle_cosine = max(angle_cosine, dot(center, v01));
  angle_cosine = max(angle_cosine, dot(center, v10));
  angle_cosine = max(angle_cosine, dot(center, v11));
  return vec4(center, angle_cosine);
}

/* Corners are expected to be in viewspace. Returns Z-aligned bounding cylinder.
 * Corner order does not matter. */
vec4 tile_bound_cylinder(vec3 v00, vec3 v01, vec3 v10, vec3 v11)
{
  vec3 center = (v00 + v01 + v10 + v11) * 0.25;
  vec4 corners_dist;
  float dist_sqr = distance_squared(center, v00);
  dist_sqr = max(dist_sqr, distance_squared(center, v01));
  dist_sqr = max(dist_sqr, distance_squared(center, v10));
  dist_sqr = max(dist_sqr, distance_squared(center, v11));
  /* Return a cone. Later converted to cylinder. */
  return vec4(center, sqrt(dist_sqr));
}

vec2 tile_to_ndc(vec2 tile_co, vec2 offset)
{
  /* Add a margin to prevent culling too much if the frustum becomes too much unstable. */
  const float margin = 0.02;
  tile_co += margin * (offset * 2.0 - 1.0);

  tile_co += offset;
  return tile_co * light_cull_buf.tile_to_uv_fac * 2.0 - 1.0;
}

CullingTile tile_culling_get(uvec2 tile_co)
{
  vec2 ftile = vec2(tile_co);
  /* Culling frustum corners for this tile. */
  vec3 corners[8];
  /* Follow same corners order as view frustum. */
  corners[1].xy = corners[0].xy = tile_to_ndc(ftile, vec2(0, 0));
  corners[5].xy = corners[4].xy = tile_to_ndc(ftile, vec2(1, 0));
  corners[6].xy = corners[7].xy = tile_to_ndc(ftile, vec2(1, 1));
  corners[2].xy = corners[3].xy = tile_to_ndc(ftile, vec2(0, 1));
  corners[1].z = corners[5].z = corners[6].z = corners[2].z = -1.0;
  corners[0].z = corners[4].z = corners[7].z = corners[3].z = 1.0;

  for (int i = 0; i < 8; i++) {
    /* Culling in view space for precision. */
    corners[i] = project_point(ProjectionMatrixInverse, corners[i]);
  }

  bool is_persp = ProjectionMatrix[3][3] == 0.0;
  CullingTile tile;
  tile.bounds = (is_persp) ? tile_bound_cone(corners[0], corners[4], corners[7], corners[3]) :
                             tile_bound_cylinder(corners[0], corners[4], corners[7], corners[3]);

  tile.frustum = isect_data_setup(shape_frustum(corners));
  return tile;
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Intersection Tests
 * \{ */

bool intersect(CullingTile tile, Sphere sphere)
{
  bool isect = true;
  /* Test tile intersection using bounding cone or bounding cylinder.
   * This has less false positive cases when the sphere is large. */
  if (ProjectionMatrix[3][3] == 0.0) {
    isect = intersect(shape_cone(tile.bounds.xyz, tile.bounds.w), sphere);
  }
  else {
    /* Simplify to a 2D circle test on the view Z axis plane. */
    isect = intersect(shape_circle(tile.bounds.xy, tile.bounds.w),
                      shape_circle(sphere.center.xy, sphere.radius));
  }
  /* Refine using frustum test. If the sphere is small it avoids intersection
   * with a neighbor tile. */
  if (isect) {
    isect = intersect(tile.frustum, sphere);
  }
  return isect;
}

bool intersect(CullingTile tile, Box bbox)
{
  return intersect(tile.frustum, bbox);
}

bool intersect(CullingTile tile, Pyramid pyramid)
{
  return intersect(tile.frustum, pyramid);
}

/** \} */

void main()
{
  uint word_idx = gl_GlobalInvocationID.x % light_cull_buf.tile_word_len;
  uint tile_idx = gl_GlobalInvocationID.x / light_cull_buf.tile_word_len;
  uvec2 tile_co = uvec2(tile_idx % light_cull_buf.tile_x_len,
                        tile_idx / light_cull_buf.tile_x_len);

  if (tile_co.y >= light_cull_buf.tile_y_len) {
    return;
  }

  /* TODO(fclem): We could stop the tile at the HiZ depth. */
  CullingTile tile = tile_culling_get(tile_co);

  uint l_idx = word_idx * 32u;
  uint l_end = min(l_idx + 32u, light_cull_buf.visible_count);
  uint word = 0u;
  for (; l_idx < l_end; l_idx++) {
    LightData light = light_buf[l_idx];

    /* Culling in view space for precision and simplicity. */
    vec3 vP = transform_point(ViewMatrix, light._position);
    vec3 v_right = transform_direction(ViewMatrix, light._right);
    vec3 v_up = transform_direction(ViewMatrix, light._up);
    vec3 v_back = transform_direction(ViewMatrix, light._back);
    float radius = light.influence_radius_max;

    Sphere sphere = shape_sphere(vP, radius);
    bool intersect_tile = intersect(tile, sphere);

    switch (light.type) {
      case LIGHT_SPOT:
        /* Only for < ~170° Cone due to plane extraction precision. */
        if (light.spot_tan < 10.0) {
          Pyramid pyramid = shape_pyramid_non_oblique(
              vP,
              vP - v_back * radius,
              v_right * radius * light.spot_tan / light.spot_size_inv.x,
              v_up * radius * light.spot_tan / light.spot_size_inv.y);
          intersect_tile = intersect_tile && intersect(tile, pyramid);
          break;
        }
        /* Fallthrough to the hemispheric case. */
      case LIGHT_RECT:
      case LIGHT_ELLIPSE:
        vec3 v000 = vP - v_right * radius - v_up * radius;
        vec3 v100 = v000 + v_right * (radius * 2.0);
        vec3 v010 = v000 + v_up * (radius * 2.0);
        vec3 v001 = v000 - v_back * radius;
        Box bbox = shape_box(v000, v100, v010, v001);
        intersect_tile = intersect_tile && intersect(tile, bbox);
      default:
        break;
    }

    if (intersect_tile) {
      word |= 1u << (l_idx % 32u);
    }
  }

  out_light_tile_buf[gl_GlobalInvocationID.x] = word;
}
