in vec2 pos;
in vec2 texCoord;

out vec4 bgl_TexCoord;

void main(void)
{
  gl_Position = vec4(pos, 1.0, 1.0);
  bgl_TexCoord = vec4(texCoord, 0.0, 0.0);
}
