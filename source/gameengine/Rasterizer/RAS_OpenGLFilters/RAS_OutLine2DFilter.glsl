uniform sampler2D bgl_RenderedTexture;
uniform sampler2D bgl_DepthTexture;
uniform sampler2D bgl_DataTextures[1];

uniform float outlinerSize;
uniform float outlinerR;
uniform float outlinerG;
uniform float outlinerB;

uniform mat2 rot = mat2(vec2(0, -1), vec2(1, 0));

void main(){
	vec2 coord = gl_TexCoord[0].st;
	vec4 color = texture2D(bgl_RenderedTexture, coord);
	vec4 Attach = texture2D(bgl_DataTextures[0], coord);
	vec2 outl = vec2(outlinerSize+0.0015, 0);
	vec4 AttachOut;

	if (Attach.r == 1){ 
		for (int i = 0; i < 4; ++i){
			AttachOut = texture2D(bgl_DataTextures[0], coord + outl);
			if (AttachOut.r < 1){
				color = vec4(outlinerR,outlinerG,outlinerB,1); //vec4(1,0.5,0,1);
			     break;
			}
        outl = rot * outl;
		} 
  
    }
	gl_FragColor = color;
}