
#pragma BLENDER_REQUIRE(common_view_lib.glsl)
#pragma BLENDER_REQUIRE(eevee_film_lib.glsl)

void main()
{
  ivec2 texel_film = ivec2(gl_FragCoord.xy) - film_buf.offset;
  float out_depth;

  if (film_buf.display_only) {
    out_depth = imageLoad(depth_img, texel_film).r;

    if (film_buf.display_id == -1) {
      out_color = texelFetch(in_combined_tx, texel_film, 0);
    }
    else if (film_buf.display_is_value) {
      out_color.rgb = imageLoad(value_accum_img, ivec3(texel_film, film_buf.display_id)).rrr;
      out_color.a = 1.0;
    }
    else {
      out_color = imageLoad(color_accum_img, ivec3(texel_film, film_buf.display_id));
    }
  }
  else {
    film_process_data(texel_film, out_color, out_depth);
  }

  gl_FragDepth = get_depth_from_view_z(-out_depth);

  gl_FragDepth = film_display_depth_ammend(texel_film, gl_FragDepth);
}
