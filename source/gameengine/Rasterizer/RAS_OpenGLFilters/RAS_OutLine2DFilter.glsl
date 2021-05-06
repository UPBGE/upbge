uniform sampler2D bgl_RenderedTexture;
uniform sampler2D bgl_DepthTexture;
uniform sampler2D bgl_DataTextures[1];

uniform mat2 rot = mat2(vec2(0, -1), vec2(1, 0));

void main(){
  vec2 coord = gl_TexCoord[0].st;
  vec4 color = texture2D(bgl_RenderedTexture, coord);
  vec4 Attach = texture2D(bgl_DataTextures[0], coord);
  vec2 outl = vec2(Attach.a, 0);
  vec4 AttachOut;

  if (Attach.a < 1){
    for (int i = 0; i < 4; ++i){
      AttachOut = texture2D(bgl_DataTextures[0], coord + outl);
      if (AttachOut.a > .99){
                color = vec4(Attach.r, Attach.g, Attach.b, 1);
            break;
      }
        outl = rot * outl;
    }

    }
  gl_FragColor = color;
}
