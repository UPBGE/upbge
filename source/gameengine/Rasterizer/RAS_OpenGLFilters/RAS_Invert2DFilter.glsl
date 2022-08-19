uniform sampler2D bgl_RenderedTexture;
in vec4 bgl_TexCoord;
out vec4 fragColor;

void main(void)
{
  vec4 texcolor = texture(bgl_RenderedTexture, bgl_TexCoord.xy);
  fragColor.rgb = 1.0 - texcolor.rgb;
  fragColor.a = texcolor.a;
}
