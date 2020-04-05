uniform sampler2D bgl_RenderedTexture;
uniform sampler2D bufC;

float exponent = 3.0;

in vec4 uvcoordsvar;
out vec4 fragColor;

void main(void)
{
    vec4 color = 1.0 * texture(bgl_RenderedTexture, uvcoordsvar.xy);
    float ao = texture(bufC, uvcoordsvar.xy).r;
    
    ao = pow(ao, exponent);

    fragColor = vec4(color.rgb * ao, color.a);
}
