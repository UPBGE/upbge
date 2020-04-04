uniform sampler2D bgl_RenderedTexture;
uniform sampler2D bufC;

float exponent = 3.0;

in vec4 bgl_TexCoord;
out vec4 fragColor;

void main(void)
{
    vec4 color = 1.0 * texture(bgl_RenderedTexture, bgl_TexCoord.xy);
    float ao = texture(bufC, bgl_TexCoord.xy).r;
    
    ao = pow(ao, exponent);

    fragColor = vec4(color.rgb * ao, color.a);
}
