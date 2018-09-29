uniform sampler2D bgl_RenderedTexture;
uniform vec2 bgl_TextureCoordinateOffset[9];

void main(void)
{
	vec4 samples[9];
	vec4 maxValue = vec4(0.0);

	for (int i = 0; i < 9; i++)
	{
		samples[i] = texture2D(bgl_RenderedTexture,
		                      gl_TexCoord[0].st + bgl_TextureCoordinateOffset[i]);
		maxValue = max(samples[i], maxValue);
	}

	gl_FragColor = maxValue;
}

