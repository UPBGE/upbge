#if __VERSION__ < 130
  #define in varying
  #define out varying
#endif

flat in vec4 finalColor;

void main()
{
	gl_FragData[0] = finalColor;
}
