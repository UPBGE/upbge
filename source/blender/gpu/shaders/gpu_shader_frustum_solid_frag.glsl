flat in vec4 insideFinalColor;
flat in vec4 outsideFinalColor;
out vec4 fragColor;

void main()
{
	gl_FragData[0] = gl_FrontFacing ? insideFinalColor : outsideFinalColor;
}
