in vec4 pos;
in vec2 texCoord;

out vec4 texcovar;

void main()
{
  gl_Position = pos;
  texcovar = vec4(texCoord, 0.0, 0.0);
}
