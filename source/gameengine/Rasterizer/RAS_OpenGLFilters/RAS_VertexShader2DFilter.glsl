in vec4 bgeOfsPos;
in vec2 bgeOfsUvs;

out vec4 bgl_TexCoord;

void main(void)
{
	gl_Position = bgeOfsPos;
	bgl_TexCoord = vec4(bgeOfsUvs, 0.0, 0.0);
}

