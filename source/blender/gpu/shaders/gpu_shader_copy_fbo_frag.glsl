uniform sampler2D colortex;
uniform sampler2D depthtex;

void main()
{
	ivec2 size = textureSize(colortex, 0);
	ivec2 co = ivec2(size * gl_TexCoord[0].xy);
	gl_FragColor = texelFetch(colortex, co, 0);
	gl_FragDepth = texelFetch(depthtex, co, 0).x;
}
