#if __VERSION__ < 130
  #define in varying
  #define out varying
#endif

in vec3 pos;
in mat4 mat;
in vec4 color;

flat out vec4 finalColor;

void main()
{
	gl_Position = gl_ModelViewProjectionMatrix * mat * vec4(pos, 1.0);
	finalColor = color;
}
