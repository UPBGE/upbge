uniform sampler2D bgl_RenderedTexture;
uniform vec2 bgl_TextureCoordinateOffset[9];

void main(void)
{
	vec4 samples[9];
	vec4 minValue = vec4(1.0);

	for (int i = 0; i < 9; i++)
	{
		samples[i] = texture2D(bgl_RenderedTexture,
		                      gl_TexCoord[0].st + bgl_TextureCoordinateOffset[i]);
		minValue = min(samples[i], minValue);
	}

	gl_FragColor = minValue;
}
