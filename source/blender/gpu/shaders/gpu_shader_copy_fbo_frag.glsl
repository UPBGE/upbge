#if defined(ANAGLYPH) || defined(STIPPLE)
uniform sampler2D lefteyetex;
uniform sampler2D righteyetex;
#else
uniform sampler2D colortex;
uniform sampler2D depthtex;
#endif

#ifdef STIPPLE
#define STIPPLE_COLUMN 0
#define STIPPLE_ROW 1

uniform int stippleid;
#endif

void main()
{
#ifdef STIPPLE
	if (stippleid == STIPPLE_ROW) {
		int result = int(mod(gl_FragCoord.y, 2));
		if (result != 0) {
			gl_FragColor = texture2D(lefteyetex, gl_TexCoord[0].xy);
		}
		else {
			gl_FragColor = texture2D(righteyetex, gl_TexCoord[0].xy);
		}
	}
	else if (stippleid == STIPPLE_COLUMN) {
		int result = int(mod(gl_FragCoord.x, 2));
		if (result == 0) {
			gl_FragColor = texture2D(lefteyetex, gl_TexCoord[0].xy);
		}
		else {
			gl_FragColor = texture2D(righteyetex, gl_TexCoord[0].xy);
		}
	}
#elif defined(ANAGLYPH)
	gl_FragColor = vec4(texture2D(lefteyetex, gl_TexCoord[0].xy).r, texture2D(righteyetex, gl_TexCoord[0].xy).gb, 1.0);
#else
	gl_FragColor = texture2D(colortex, gl_TexCoord[0].xy);
	gl_FragDepth = texture2D(depthtex, gl_TexCoord[0].xy).x;
#endif
}
