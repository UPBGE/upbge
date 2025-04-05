void main(void)
{
  vec4 samples[9];
  vec4 maxValue = vec4(0.0);

  for (int i = 0; i < 9; i++) {
    samples[i] = texture(bgl_RenderedTexture, bgl_TexCoord.xy + bgl_TextureCoordinateOffset[i]);
    maxValue = max(samples[i], maxValue);
  }

  fragColor = maxValue;
}
