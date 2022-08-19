
#pragma BLENDER_REQUIRE(common_math_geom_lib.glsl)

/**
 * Geometric shape structures.
 * Some constructors might seems redundant but are here to make the API cleaner and
 * allow for more than one constructor per type.
 */

/* ---------------------------------------------------------------------- */
/** \name Circle
 * \{ */

struct Circle {
  vec2 center;
  float radius;
};

Circle shape_circle(vec2 center, float radius)
{
  return Circle(center, radius);
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Sphere
 * \{ */

struct Sphere {
  vec3 center;
  float radius;
};

Sphere shape_sphere(vec3 center, float radius)
{
  return Sphere(center, radius);
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Box
 * \{ */

struct Box {
  vec3 corners[8];
};

/* Construct box from 4 basis points. */
Box shape_box(vec3 v000, vec3 v100, vec3 v010, vec3 v001)
{
  v100 -= v000;
  v010 -= v000;
  v001 -= v000;
  Box box;
  box.corners[0] = v000;
  box.corners[1] = v000 + v100;
  box.corners[2] = v000 + v010 + v100;
  box.corners[3] = v000 + v010;
  box.corners[4] = box.corners[0] + v001;
  box.corners[5] = box.corners[1] + v001;
  box.corners[6] = box.corners[2] + v001;
  box.corners[7] = box.corners[3] + v001;
  return box;
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Square Pyramid
 * \{ */

struct Pyramid {
  /* Apex is the first. Base vertices are in clockwise order from front view. */
  vec3 corners[5];
};

/**
 * Regular Square Pyramid (can be oblique).
 * Use this corner order.
 * (Top-Down View of the pyramid)
 * <pre>
 *
 * Y
 * |
 * |
 * .-----X
 *
 *  4-----------3
 *  | \       / |
 *  |   \   /   |
 *  |     0     |
 *  |   /   \   |
 *  | /       \ |
 *  1-----------2
 * </pre>
 * base_corner_00 is vertex 1
 * base_corner_01 is vertex 2
 * base_corner_10 is vertex 4
 */
Pyramid shape_pyramid(vec3 apex, vec3 base_corner_00, vec3 base_corner_01, vec3 base_corner_10)
{
  Pyramid pyramid;
  pyramid.corners[0] = apex;
  pyramid.corners[1] = base_corner_00;
  pyramid.corners[2] = base_corner_01;
  pyramid.corners[3] = base_corner_10 + (base_corner_01 - base_corner_00);
  pyramid.corners[4] = base_corner_10;
  return pyramid;
}

/**
 * Regular Square Pyramid.
 * <pre>
 *
 * Y
 * |
 * |
 * .-----X
 *
 *  4-----Y-----3
 *  | \   |   / |
 *  |   \ | /   |
 *  |     0-----X
 *  |   /   \   |
 *  | /       \ |
 *  1-----------2
 * </pre>
 * base_center_pos_x is vector from base center to X
 * base_center_pos_y is vector from base center to Y
 */
Pyramid shape_pyramid_non_oblique(vec3 apex,
                                  vec3 base_center,
                                  vec3 base_center_pos_x,
                                  vec3 base_center_pos_y)
{
  Pyramid pyramid;
  pyramid.corners[0] = apex;
  pyramid.corners[1] = base_center - base_center_pos_x - base_center_pos_y;
  pyramid.corners[2] = base_center + base_center_pos_x - base_center_pos_y;
  pyramid.corners[3] = base_center + base_center_pos_x + base_center_pos_y;
  pyramid.corners[4] = base_center - base_center_pos_x + base_center_pos_y;
  return pyramid;
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Frustum
 * \{ */

struct Frustum {
  vec3 corners[8];
};

/**
 * Use this corner order.
 * <pre>
 *
 * Z  Y
 * | /
 * |/
 * .-----X
 *     2----------6
 *    /|         /|
 *   / |        / |
 *  1----------5  |
 *  |  |       |  |
 *  |  3-------|--7
 *  | /        | /
 *  |/         |/
 *  0----------4
 * </pre>
 */
Frustum shape_frustum(vec3 corners[8])
{
  Frustum frustum;
  for (int i = 0; i < 8; i++) {
    frustum.corners[i] = corners[i];
  }
  return frustum;
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Cone
 * \{ */

/* Cone at orign with no height. */
struct Cone {
  vec3 direction;
  float angle_cos;
};

Cone shape_cone(vec3 direction, float angle_cosine)
{
  return Cone(direction, angle_cosine);
}

/** \} */
