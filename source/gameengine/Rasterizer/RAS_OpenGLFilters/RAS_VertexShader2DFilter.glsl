in vec4 pos;
in vec2 texCoord;

out vec4 bgl_TexCoord;

void main(void)
{
  gl_Position = pos;
  bgl_TexCoord = vec4(texCoord, 0.0, 0.0);
}
