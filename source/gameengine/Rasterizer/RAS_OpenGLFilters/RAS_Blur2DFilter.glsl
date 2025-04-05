void main(void)
{
  vec4 samples[9];

  for (int i = 0; i < 9; i++) {
    samples[i] = texture(bgl_RenderedTexture, bgl_TexCoord.xy + bgl_TextureCoordinateOffset[i]);
  }

  fragColor = (samples[0] + (2.0 * samples[1]) + samples[2] + (2.0 * samples[3]) + samples[4] +
               (2.0 * samples[5]) + samples[6] + (2.0 * samples[7]) + samples[8]) /
              13.0;
}
