void main(void)
{
  vec4 samples[9];

  for (int i = 0; i < 9; i++) {
    samples[i] = texture(bgl_RenderedTexture, bgl_TexCoord.xy + g_data.coo_offset[i].xy);
  }

  fragColor = (samples[4] * 8.0) - (samples[0] + samples[1] + samples[2] + samples[3] +
                                    samples[5] + samples[6] + samples[7] + samples[8]);
  fragColor = vec4(fragColor.rgb, 1.0);
}
