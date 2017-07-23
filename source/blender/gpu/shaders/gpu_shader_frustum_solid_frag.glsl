flat in vec4 insideFinalColor;
flat in vec4 outsideFinalColor;
out vec4 fragColor;

void main()
{
	gl_FragColor = gl_FrontFacing ? insideFinalColor : outsideFinalColor;
}
