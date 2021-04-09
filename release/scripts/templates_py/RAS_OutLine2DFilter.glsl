uniform sampler2D bgl_RenderedTexture;
uniform sampler2D bgl_DepthTexture;
uniform sampler2D bgl_DataTextures[1];

uniform float outilineSize = 0.003;
uniform vec4 outline_color = vec4(1,0,0,1);
uniform mat2 rot = mat2(vec2(0, -1), vec2(1, 0));

void main(){
	vec2 coord = gl_TexCoord[0].st;
	vec4 color = texture2D(bgl_RenderedTexture, coord);
	vec4 depth = texture2D(bgl_DataTextures[0], coord);
	
	vec2 offset = vec2(outilineSize, 0);
	vec4 new_depth;
	

	if (depth.r == 1){ 

		for (int i = 0; i < 4; ++i){
			new_depth = texture2D(bgl_DataTextures[0], coord + offset);
			if (new_depth.r < 1){
				color = outline_color;
			     break;
			}
        offset = rot * offset;
		} 
  
    }
	gl_FragColor = color;
}