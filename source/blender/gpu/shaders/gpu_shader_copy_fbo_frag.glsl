uniform sampler2D colortex;
uniform sampler2D depthtex;

void main()
{
	gl_FragColor = texture2D(colortex, gl_TexCoord[0].xy);
	gl_FragDepth = texture2D(depthtex, gl_TexCoord[0].xy).x;
}
