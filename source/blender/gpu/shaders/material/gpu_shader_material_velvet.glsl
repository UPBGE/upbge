
void node_bsdf_velvet(vec4 color, float roughness, vec3 N, float weight, out Closure result)
{
  N = safe_normalize(N);

  /* Fallback to diffuse. */
  ClosureDiffuse diffuse_data;
  diffuse_data.weight = weight;
  diffuse_data.color = color.rgb;
  diffuse_data.N = N;
  diffuse_data.sss_id = 0u;

  result = closure_eval(diffuse_data);
}
