
void main()
{
  vec4 pos_4d = vec4(pos, 1.0);
  gl_Position = pos_4d;

#ifdef USE_WORLD_CLIP_PLANES
  world_clip_planes_calc_clip_distance((ModelMatrix * pos_4d).xyz);
#endif
}
