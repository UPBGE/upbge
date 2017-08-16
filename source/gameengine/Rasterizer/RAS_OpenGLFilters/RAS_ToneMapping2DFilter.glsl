#define DITHER 20.0
uniform sampler2D bgl_RenderedTexture;
in vec4 bgl_TexCoord;
out vec4 fragColor;

//http://slideshare.net/ozlael/hable-john-uncharted2-hdr-lighting
//http://filmicgames.com/archives/75
//http://filmicgames.com/archives/183
//http://filmicgames.com/archives/190
//http://imdoingitwrong.wordpress.com/2010/08/19/why-reinhard-desaturates-my-blacks-3/
//http://mynameismjp.wordpress.com/2010/04/30/a-closer-look-at-tone-mapping/
//http://renderwonk.com/publications/s2010-color-course/

float gamma = 2.2;

vec3 linearToneMapping(vec3 color)
{
	float exposure = 1.;
	color = clamp(exposure * color, 0., 1.);
	color = pow(color, vec3(1. / gamma));
	return color;
}

vec3 simpleReinhardToneMapping(vec3 color)
{
	float exposure = 1.5;
	color *= exposure/(1. + color / exposure);
	color = pow(color, vec3(1. / gamma));
	return color;
}

vec3 lumaBasedReinhardToneMapping(vec3 color)
{
	float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
	float toneMappedLuma = luma / (1. + luma);
	color *= toneMappedLuma / luma;
	color = pow(color, vec3(1. / gamma));
	return color;
}

vec3 whitePreservingLumaBasedReinhardToneMapping(vec3 color)
{
	float white = 2.;
	float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
	float toneMappedLuma = luma * (1. + luma / (white*white)) / (1. + luma);
	color *= toneMappedLuma / luma;
	color = pow(color, vec3(1. / gamma));
	return color;
}

vec3 RomBinDaHouseToneMapping(vec3 color)
{
    color = exp( -1.0 / ( 2.72*color + 0.15 ) );
	color = pow(color, vec3(1. / gamma));
	return color;
}

vec3 filmicToneMapping(vec3 color)
{
	color = max(vec3(0.), color - vec3(0.004));
	color = (color * (6.2 * color + .5)) / (color * (6.2 * color + 1.7) + 0.06);
	return color;
}

vec3 Uncharted2ToneMapping(vec3 color)
{
	float A = 0.15;
	float B = 0.50;
	float C = 0.10;
	float D = 0.20;
	float E = 0.02;
	float F = 0.30;
	float W = 11.2;
	float exposure = 2.;
	color *= exposure;
	color = ((color * (A * color + C * B) + D * E) / (color * (A * color + B) + D * F)) - E / F;
	float white = ((W * (A * W + C * B) + D * E) / (W * (A * W + B) + D * F)) - E / F;
	color /= white;
	color = pow(color, vec3(1. / gamma));
	return color;
}

float mod_dither3(vec2 u) {
	return mod(u.x + u.y + mod(208. + u.x * 3.58, 13. + mod(u.y * 22.9, 9.)),7.) * .143 * 2. -1.;
}

void main()
{
	vec2 uv = bgl_TexCoord.xy;
	vec3 color = texture(bgl_RenderedTexture, bgl_TexCoord.xy).rgb;
	float n = mod_dither3(gl_FragCoord.xy);
	float lum = floor(DITHER + n) / DITHER;
	color *= lum;

	color = linearToneMapping(color);

	//color = simpleReinhardToneMapping(color);
	//color = lumaBasedReinhardToneMapping(color);
	//color = whitePreservingLumaBasedReinhardToneMapping(color);
	//color = RomBinDaHouseToneMapping(color);
	//color = filmicToneMapping(color);
	//color = Uncharted2ToneMapping(color);

	fragColor = vec4(color, 1.0);
}