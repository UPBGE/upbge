void remainder(float frame, float x, float y, out float remainder_temp, out float quotient_floored)
{
  float L = x * y;
  quotient_floored = floor(frame / L);
  remainder_temp = (frame - (quotient_floored * L) + 0.000001) * 0.999999;
}

void node_sprites_animation(float frame,
                            float columns,
                            float rows,
                            float columns_offset,
                            float rows_offset,
                            out vec3 outLocation,
                            out vec3 outScale)
{
  float remainder_temp1, remainder_temp2, quotient_floored1, quotient_floored2;

  remainder(floor(frame), columns, rows, remainder_temp1, quotient_floored1);
  remainder((remainder_temp1 - 0.00001), floor(columns), 1.0, remainder_temp2, quotient_floored2);

  outLocation = vec3(((1 - remainder_temp2) - floor(columns_offset)),
                     (quotient_floored2 + (floor(rows_offset) + 1.0)),
                     0.0);
  outScale = vec3(floor(columns), floor(rows), 1.0);
}
