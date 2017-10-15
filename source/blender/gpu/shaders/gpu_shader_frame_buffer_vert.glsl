in vec4 bgl_InPosition;
in vec2 bgl_InTexCoord;

out vec4 texcovar;

void main()
{
	gl_Position = bgl_InPosition;
	texcovar = vec4(bgl_InTexCoord, 0.0, 0.0);
}
