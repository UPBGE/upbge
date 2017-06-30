uniform sampler2D bgl_RenderedTexture;
in vec4 bgl_TexCoord;
out vec4 fragColor;

//source: https://learnopengl.com/#!Advanced-Lighting/HDR
const float exposure = 1.0;

vec3 brightnessContrast(vec3 value, float brightness, float contrast)
{
    return (value - 0.5) * contrast + 0.5 + brightness;
}

void main()
{
    const float gamma = 2.2;
    vec3 hdrColor = texture(bgl_RenderedTexture, bgl_TexCoord.xy).rgb;
  
    // Exposure tone mapping
    vec3 mapped = vec3(1.0) - exp(-hdrColor * exposure);
    // Gamma correction 
    mapped = brightnessContrast(pow(mapped, vec3(1.0 / gamma)), 0.05, 1.3);
  
    fragColor = vec4(mapped, 1.0);
}