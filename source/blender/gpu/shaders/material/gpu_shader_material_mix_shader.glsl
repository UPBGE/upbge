void node_mix_shader(float fac, inout Closure shader1, inout Closure shader2, out Closure shader)
{
  shader = closure_mix(shader1, shader2, fac);
}
