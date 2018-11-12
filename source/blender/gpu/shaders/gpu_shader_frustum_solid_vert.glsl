#if __VERSION__ < 130
  #define in varying
  #define out varying
#endif

in vec3 pos;
in mat4 mat;
in vec4 insideColor;
in vec4 outsideColor;

flat out vec4 insideFinalColor;
flat out vec4 outsideFinalColor;

void main()
{
	gl_Position = gl_ModelViewProjectionMatrix * mat * vec4(pos, 1.0);
	insideFinalColor = insideColor;
	outsideFinalColor = outsideColor;
}
