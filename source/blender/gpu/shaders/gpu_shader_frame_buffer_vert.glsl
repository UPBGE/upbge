in vec4 pos;
in vec2 uvs;

out vec4 texcovar;

void main()
{
	gl_Position = pos;
	texcovar = vec4(uvs, 0.0, 0.0);
}
