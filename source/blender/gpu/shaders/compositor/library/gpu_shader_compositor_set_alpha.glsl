void node_composite_set_alpha_apply(vec4 color, float alpha, out vec4 result)
{
  result = color * alpha;
}

void node_composite_set_alpha_replace(vec4 color, float alpha, out vec4 result)
{
  result = vec4(color.rgb, alpha);
}
