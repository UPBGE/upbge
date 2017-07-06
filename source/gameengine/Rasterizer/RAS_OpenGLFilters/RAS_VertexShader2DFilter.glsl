in vec4 bgl_InPositon;
in vec2 bgl_InTexCoord;

out vec4 bgl_TexCoord;

void main(void)
{
	gl_Position = bgl_InPositon;
	bgl_TexCoord = vec4(bgl_InTexCoord, 0.0, 0.0);
}

