#if __VERSION__ < 130
  #define in varying
  #define out varying
#endif

in vec2 pos;
in vec4 trans;
in vec4 color;

flat out vec4 finalColor;

void main()
{
	gl_Position = gl_ProjectionMatrix * gl_ModelViewMatrix * vec4(pos * trans.zw + trans.xy, 0.0, 1.0);
	finalColor = color;
}
