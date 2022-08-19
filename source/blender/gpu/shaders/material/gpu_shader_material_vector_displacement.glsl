void node_vector_displacement_tangent(
    vec4 vector, float midlevel, float scale, vec4 T, out vec3 result)
{
  vec3 oN = normalize(normal_world_to_object(g_data.N));
  vec3 oT = normalize(normal_world_to_object(T.xyz));
  vec3 oB = T.w * normalize(cross(oN, oT));

  result = (vector.xyz - midlevel) * scale;
  result = result.x * oT + result.y * oN + result.z * oB;
  result = transform_point(ModelMatrix, result);
}

void node_vector_displacement_object(vec4 vector, float midlevel, float scale, out vec3 result)
{
  result = (vector.xyz - midlevel) * scale;
  result = transform_point(ModelMatrix, result);
}

void node_vector_displacement_world(vec4 vector, float midlevel, float scale, out vec3 result)
{
  result = (vector.xyz - midlevel) * scale;
}
