#if __VERSION__ < 130
  #define flat
  #define in varying
#endif

flat in vec4 finalColor;

void main()
{
	gl_FragData[0] = finalColor;
}
