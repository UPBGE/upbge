
void node_bsdf_glossy(
    vec4 color, float roughness, vec3 N, float weight, float do_multiscatter, out Closure result)
{
  N = safe_normalize(N);
  vec3 V = cameraVec(g_data.P);
  float NV = dot(N, V);

  vec2 split_sum = brdf_lut(NV, roughness);

  ClosureReflection reflection_data;
  reflection_data.weight = weight;
  reflection_data.color = (do_multiscatter != 0.0) ?
                              F_brdf_multi_scatter(color.rgb, color.rgb, split_sum) :
                              F_brdf_single_scatter(color.rgb, color.rgb, split_sum);
  reflection_data.N = N;
  reflection_data.roughness = roughness;

  result = closure_eval(reflection_data);
}
