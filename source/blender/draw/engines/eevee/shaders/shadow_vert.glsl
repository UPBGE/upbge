
#pragma BLENDER_REQUIRE(common_view_lib.glsl)
#pragma BLENDER_REQUIRE(common_math_lib.glsl)
#pragma BLENDER_REQUIRE(common_hair_lib.glsl)
#pragma BLENDER_REQUIRE(surface_lib.glsl)

in vec3 pos;
in vec3 nor;

void main()
{
  GPU_INTEL_VERTEX_SHADER_WORKAROUND

#ifdef HAIR_SHADER
  hairStrandID = hair_get_strand_id();
  hairBary = hair_get_barycentric();
  vec3 pos, binor;
  hair_get_pos_tan_binor_time((ProjectionMatrix[3][3] == 0.0),
                              ModelMatrixInverse,
                              ViewMatrixInverse[3].xyz,
                              ViewMatrixInverse[2].xyz,
                              pos,
                              hairTangent,
                              binor,
                              hairTime,
                              hairThickness,
                              hairThickTime);

  worldNormal = cross(hairTangent, binor);
  vec3 world_pos = pos;
#elif defined(POINTCLOUD_SHADER)
  pointcloud_get_pos_and_radius(pointPosition, pointRadius);
  pointID = gl_VertexID;
#else
  vec3 world_pos = point_object_to_world(pos);
#endif

  gl_Position = point_world_to_ndc(world_pos);
#ifdef MESH_SHADER
  worldPosition = world_pos;
  viewPosition = point_world_to_view(worldPosition);

#  ifndef HAIR_SHADER
  worldNormal = normalize(normal_object_to_world(nor));
#  endif

  /* No need to normalize since this is just a rotation. */
  viewNormal = normal_world_to_view(worldNormal);
#  ifdef USE_ATTR
#    ifdef HAIR_SHADER
  pos = hair_get_strand_pos();
#    endif
  pass_attr(pos, NormalMatrix, ModelMatrixInverse);
#  endif
#endif
}

#ifdef HAIR_SHADER
#  ifdef OBINFO_LIB
vec3 attr_load_orco(samplerBuffer cd_buf)
{
  vec3 P = hair_get_strand_pos();
  vec3 lP = transform_point(ModelMatrixInverse, P);
  return OrcoTexCoFactors[0].xyz + lP * OrcoTexCoFactors[1].xyz;
}
#  endif

/* Per attribute scope follows loading order. */
int g_curves_attr_id = 0;

/* Return the index to use for looking up the attribute value in the sampler
 * based on the attribute scope (point or spline). */
int curves_attribute_element_id()
{
  int id = hairStrandID;
  if (drw_curves.is_point_attribute[g_curves_attr_id][0] != 0) {
    id = hair_get_base_id();
  }

  g_curves_attr_id += 1;
  return id;
}

vec4 attr_load_tangent(samplerBuffer cd_buf)
{
  /* Not supported. */
  return vec4(0.0, 0.0, 0.0, 1.0);
}

vec4 attr_load_vec4(samplerBuffer cd_buf)
{
  return texelFetch(cd_buf, curves_attribute_element_id()).rgba;
}

vec3 attr_load_vec3(samplerBuffer cd_buf)
{
  return texelFetch(cd_buf, curves_attribute_element_id()).rgb;
}

vec2 attr_load_vec2(samplerBuffer cd_buf)
{
  return texelFetch(cd_buf, curves_attribute_element_id()).rg;
}

float attr_load_float(samplerBuffer cd_buf)
{
  return texelFetch(cd_buf, curves_attribute_element_id()).r;
}

#else

#  ifdef OBINFO_LIB
vec3 attr_load_orco(samplerBuffer cd_buf)
{
  vec3 P = hair_get_strand_pos();
  vec3 lP = transform_point(ModelMatrixInverse, P);
  return OrcoTexCoFactors[0].xyz + lP * OrcoTexCoFactors[1].xyz;
}
#  endif

vec4 attr_load_tangent(vec4 tangent)
{
  tangent.xyz = safe_normalize(normal_object_to_world(tangent.xyz));
  return tangent;
}

/* Simple passthrough. */
vec4 attr_load_vec4(vec4 attr)
{
  return attr;
}
vec3 attr_load_vec3(vec3 attr)
{
  return attr;
}
vec2 attr_load_vec2(vec2 attr)
{
  return attr;
}
vec2 attr_load_float(vec2 attr)
{
  return attr;
}
#endif

/* Passthrough. */
float attr_load_temperature_post(float attr)
{
  return attr;
}
vec4 attr_load_color_post(vec4 attr)
{
  return attr;
}
