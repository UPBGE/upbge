in vec4 bgeOfsPos;
in vec2 bgeOfsUvs;

out vec4 texcovar;

void main()
{
	gl_Position = bgeOfsPos;
	texcovar = vec4(bgeOfsUvs, 0.0, 0.0);
}
