#if defined(ANAGLYPH) || defined(STIPPLE)
uniform sampler2D lefteyetex;
uniform sampler2D righteyetex;
#else
uniform sampler2D colortex;
#  ifdef DEPTH
uniform sampler2D depthtex;
#  endif
#endif

#ifdef STIPPLE
#define STIPPLE_COLUMN 0
#define STIPPLE_ROW 1

uniform int stippleid;
#endif

float linearrgb_to_srgb(float c)
{
	if (c < 0.0031308)
		return (c < 0.0) ? 0.0 : c * 12.92;
	else
		return 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

void linearrgb_to_srgb(vec4 col_from, out vec4 col_to)
{
	col_to.r = linearrgb_to_srgb(col_from.r);
	col_to.g = linearrgb_to_srgb(col_from.g);
	col_to.b = linearrgb_to_srgb(col_from.b);
	col_to.a = col_from.a;
}

void main()
{
	vec2 co = gl_TexCoord[0].xy;
#ifdef STIPPLE
	if (stippleid == STIPPLE_ROW) {
		int result = int(mod(gl_FragCoord.y, 2));
		if (result != 0) {
			gl_FragColor = texture2D(lefteyetex, co);
		}
		else {
			gl_FragColor = texture2D(righteyetex, co);
		}
	}
	else if (stippleid == STIPPLE_COLUMN) {
		int result = int(mod(gl_FragCoord.x, 2));
		if (result == 0) {
			gl_FragColor = texture2D(lefteyetex, co);
		}
		else {
			gl_FragColor = texture2D(righteyetex, co);
		}
	}
#elif defined(ANAGLYPH)
	gl_FragColor = vec4(texture2D(lefteyetex, co).r, texture2D(righteyetex, co).gb, 1.0);
#else
	gl_FragColor = texture2D(colortex, co);
#  ifdef DEPTH
	gl_FragDepth = texture2D(depthtex, co).x;
#  endif
#endif

#ifdef COLOR_MANAGEMENT
	linearrgb_to_srgb(gl_FragColor, gl_FragColor);
#endif
}
