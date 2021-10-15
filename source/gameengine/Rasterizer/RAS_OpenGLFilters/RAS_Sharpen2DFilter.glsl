uniform sampler2D bgl_RenderedTexture;
uniform vec2 bgl_TextureCoordinateOffset[9];
in vec4 bgl_TexCoord;
out vec4 fragColor;

void main(void)
{
  vec4 samples[9];

  for (int i = 0; i < 9; i++) {
    samples[i] = texture(bgl_RenderedTexture, bgl_TexCoord.xy + bgl_TextureCoordinateOffset[i]);
  }
  fragColor = (samples[4] * 9.0) - (samples[0] + samples[1] + samples[2] + samples[3] +
                                    samples[5] + samples[6] + samples[7] + samples[8]);
}
