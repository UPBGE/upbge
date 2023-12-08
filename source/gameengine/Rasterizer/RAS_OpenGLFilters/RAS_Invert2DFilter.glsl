void main(void)
{
  vec4 texcolor = texture(bgl_RenderedTexture, bgl_TexCoord.xy);
  fragColor.rgb = 1.0 - texcolor.rgb;
  fragColor.a = texcolor.a;
}
