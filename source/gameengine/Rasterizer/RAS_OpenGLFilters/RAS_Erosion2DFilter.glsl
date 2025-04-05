void main(void)
{
  vec4 samples[9];
  vec4 minValue = vec4(1.0);

  for (int i = 0; i < 9; i++) {
    samples[i] = texture(bgl_RenderedTexture, bgl_TexCoord.xy + bgl_TextureCoordinateOffset[i]);
    minValue = min(samples[i], minValue);
  }

  fragColor = minValue;
}
