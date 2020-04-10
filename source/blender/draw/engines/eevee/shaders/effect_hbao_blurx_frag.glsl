uniform sampler2D bufA;
uniform float bgl_RenderedTextureWidth;
uniform float bgl_RenderedTextureHeight;

vec2 FullRes = vec2(bgl_RenderedTextureWidth, bgl_RenderedTextureHeight);
vec2 InvFullRes = 1.0 / FullRes;

#define KERNEL_RADIUS 8.0

in vec4 uvcoordsvar;
out vec4 fragColor;

vec2 SampleAOZ(vec2 uv)
{
    return texture(bufA, uvcoordsvar.xy + uv * InvFullRes).rg;
}

vec2 PointSampleAOZ(vec2 uv)
{
    ivec2 coord = ivec2(round(gl_FragCoord.xy + uv));
    return texelFetch(bufA, coord, 0).rg;
}

float CrossBilateralWeight(float r, float z, float z0)
{
    const float BlurSigma = (KERNEL_RADIUS+1.0f) * 0.5f;
    const float BlurFalloff = 1.0f / (2.0f*BlurSigma*BlurSigma);

    float dz = z0 - z;
    return exp2(-r*r*BlurFalloff - dz*dz);
}

void main(void)
{
    vec2 aoz = SampleAOZ(vec2(0));
    float center_z = aoz.y;

    float w = 1.0;
    float total_ao = aoz.x * w;
    float total_weight = w;
    float i = 1.0;

    for(; i <= KERNEL_RADIUS/2; i += 1.0)
    {
        aoz = SampleAOZ( vec2(i,0) );
        w = CrossBilateralWeight(i, aoz.y, center_z);
        total_ao += aoz.x * w;
        total_weight += w;

        aoz = SampleAOZ( vec2(-i,0) );
        w = CrossBilateralWeight(i, aoz.y, center_z);
        total_ao += aoz.x * w;
        total_weight += w;
    }

    for(; i <= KERNEL_RADIUS; i += 2.0)
    {
        aoz = SampleAOZ( vec2(0.5+i,0) );
        w = CrossBilateralWeight(i, aoz.y, center_z);
        total_ao += aoz.x * w;
        total_weight += w;

        aoz = SampleAOZ( vec2(-0.5-i,0) );
        w = CrossBilateralWeight(i, aoz.y, center_z);
        total_ao += aoz.x * w;
        total_weight += w;
    }

    float ao = total_ao / total_weight;
    fragColor = vec4(ao, center_z, 0.0, 1.0);
}
