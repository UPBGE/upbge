uniform sampler2D bgl_RenderedTexture;
in vec4 bgl_TexCoord;
out vec4 fragColor;

//source: https://learnopengl.com/#!Advanced-Lighting/HDR
const float exposure = 1.0;

void main()
{
    const float gamma = 2.2;
    vec3 hdrColor = texture(bgl_RenderedTexture, bgl_TexCoord.xy).rgb;
  
    // Exposure tone mapping
    vec3 mapped = vec3(1.0) - exp(-hdrColor * exposure);
    // Gamma correction 
    mapped = pow(mapped, vec3(1.0 / gamma));
  
    fragColor = vec4(mapped, 1.0);
}