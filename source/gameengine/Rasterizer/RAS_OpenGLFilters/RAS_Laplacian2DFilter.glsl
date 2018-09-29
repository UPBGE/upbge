uniform sampler2D bgl_RenderedTexture;
uniform vec2 bgl_TextureCoordinateOffset[9];

void main(void)
{
	vec4 samples[9];

	for (int i = 0; i < 9; i++)
	{
		samples[i] = texture2D(bgl_RenderedTexture,
		                      gl_TexCoord[0].st + bgl_TextureCoordinateOffset[i]);
	}

	gl_FragColor = (samples[4] * 8.0) -
	        (samples[0] + samples[1] + samples[2] +
	         samples[3] + samples[5] +
	         samples[6] + samples[7] + samples[8]);
	gl_FragColor = vec4(gl_FragColor.rgb, 1.0);
}

