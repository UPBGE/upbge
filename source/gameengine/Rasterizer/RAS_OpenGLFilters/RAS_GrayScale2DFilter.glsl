uniform sampler2D bgl_RenderedTexture;
in vec4 bgl_TexCoord;
out vec4 fragColor;

void main(void)
{
  vec4 texcolor = texture(bgl_RenderedTexture, bgl_TexCoord.xy);
  float gray = dot(texcolor.rgb, vec3(0.299, 0.587, 0.114));
  fragColor = vec4(gray, gray, gray, texcolor.a);
}
