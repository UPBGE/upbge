// This is a HBAO-Shader for OpenGL, based upon nvidias directX implementation
// supplied in their SampleSDK available from nvidia.com
// The slides describing the implementation is available at
// http://www.nvidia.co.uk/object/siggraph-2008-HBAO.html

uniform sampler2D bgl_DepthTexture;
uniform sampler2D bgl_NoiseTexture;
uniform float bgl_RenderedTextureWidth;
uniform float bgl_RenderedTextureHeight;
//Camera Settings
uniform float near = 0.1;
uniform float far = 100.0;
uniform float flen = 50.0;
uniform float AOStrength = 5.0;

const float PI = 3.14159265;

in vec4 uvcoordsvar;
out vec4 fragColor;

vec2 resolution = vec2(bgl_RenderedTextureWidth, bgl_RenderedTextureHeight);

float fov = flen / 180.0 * PI;
vec2 FocalLen = vec2(1.0 / tan(fov*0.5) * resolution.y / resolution.x, 1.0 / tan(fov * 0.5));
vec2 InvF = 1.0 / FocalLen;
vec2 UVToViewA = vec2(2.0 * InvF.x, -2.0 * InvF.y);
vec2 UVToViewB = vec2(-1.0 * InvF.x, 1.0 * InvF.y);
vec2 LinMAD = vec2((near - far)/(2.0 * near * far),(near + far)/(2.0 * near * far));
vec2 AORes = resolution;
vec2 InvAORes = 1.0 / resolution;
vec2 NoiseScale = resolution / 4.0;

float R = 0.3;
float R2 = 0.3*0.3;
float NegInvR2 = - 1.0 / (0.3*0.3);
float TanBias = tan(30.0 * PI / 180.0);
float MaxRadiusPixels = 50.0;

int NumDirections = 6;
int NumSamples = 4;

float ViewSpaceZFromDepth(float d)
{
    // [0,1] -> [-1,1] clip space
    d = d * 2.0 - 1.0;

    // Get view space Z
    return -1.0 / (LinMAD.x * d + LinMAD.y);
}

vec3 UVToViewSpace(vec2 uv, float z)
{
    uv = UVToViewA * uv + UVToViewB;
    return vec3(uv * z, z);
}

vec3 GetViewPos(vec2 uv)
{
    float z = ViewSpaceZFromDepth(texture(bgl_DepthTexture, uv).r);
    //float z = texture(bgl_DepthTexture, uv).r;
    return UVToViewSpace(uv, z);
}

vec3 GetViewPosPoint(ivec2 uv)
{
    ivec2 coord = ivec2(gl_FragCoord.xy) + uv;
    float z = texelFetch(bgl_DepthTexture, coord, 0).r;
    return UVToViewSpace(uv, z);
}

float TanToSin(float x)
{
    return x * inversesqrt(x*x + 1.0);
}

float InvLength(vec2 V)
{
    return inversesqrt(dot(V,V));
}

float Tangent(vec3 V)
{
    return V.z * InvLength(V.xy);
}

float BiasedTangent(vec3 V)
{
    return V.z * InvLength(V.xy) + TanBias;
}

float Tangent(vec3 P, vec3 S)
{
    return -(P.z - S.z) * InvLength(S.xy - P.xy);
}

float Length2(vec3 V)
{
    return dot(V,V);
}

vec3 MinDiff(vec3 P, vec3 Pr, vec3 Pl)
{
    vec3 V1 = Pr - P;
    vec3 V2 = P - Pl;
    return (Length2(V1) < Length2(V2)) ? V1 : V2;
}

vec2 SnapUVOffset(vec2 uv)
{
    return round(uv * AORes) * InvAORes;
}

float Falloff(float d2)
{
    return d2 * NegInvR2 + 1.0;
}

float HorizonOcclusion(	vec2 deltaUV,
                        vec3 P,
                        vec3 dPdu,
                        vec3 dPdv,
                        float randstep,
                        float numSamples)
{
    float ao = 0.0;

    // Offset the first coord with some noise
    vec2 uv = uvcoordsvar.xy + SnapUVOffset(randstep*deltaUV);
    deltaUV = SnapUVOffset( deltaUV );

    // Calculate the tangent vector
    vec3 T = deltaUV.x * dPdu + deltaUV.y * dPdv;

    // Get the angle of the tangent vector from the viewspace axis
    float tanH = BiasedTangent(T);
    float sinH = TanToSin(tanH);

    float tanS;
    float d2;
    vec3 S;

    // Sample to find the maximum angle
    for(float s = 1.0; s <= numSamples; ++s)
    {
        uv += deltaUV;
        S = GetViewPos(uv);
        tanS = Tangent(P, S);
        d2 = Length2(S - P);

        // Is the sample within the radius and the angle greater?
        if(d2 < R2 && tanS > tanH)
        {
            float sinS = TanToSin(tanS);
            // Apply falloff based on the distance
            ao += Falloff(d2) * (sinS - sinH);

            tanH = tanS;
            sinH = sinS;
        }
    }
    
    return ao;
}

vec2 RotateDirections(vec2 Dir, vec2 CosSin)
{
    return vec2(Dir.x*CosSin.x - Dir.y*CosSin.y,
                  Dir.x*CosSin.y + Dir.y*CosSin.x);
}

void ComputeSteps(inout vec2 stepSizeUv, inout float numSteps, float rayRadiusPix, float rand)
{
    // Avoid oversampling if numSteps is greater than the kernel radius in pixels
    numSteps = min(NumSamples, rayRadiusPix);

    // Divide by Ns+1 so that the farthest samples are not fully attenuated
    float stepSizePix = rayRadiusPix / (numSteps + 1);

    // Clamp numSteps if it is greater than the max kernel footprint
    float maxNumSteps = MaxRadiusPixels / stepSizePix;
    if (maxNumSteps < numSteps)
    {
        // Use dithering to avoid AO discontinuities
        numSteps = floor(maxNumSteps + rand);
        numSteps = max(numSteps, 1);
        stepSizePix = MaxRadiusPixels / numSteps;
    }

    // Step size in uv space
    stepSizeUv = stepSizePix * InvAORes;
}

void main(void)
{
    float numDirections = NumDirections;

    vec3 P, Pr, Pl, Pt, Pb;
    P 	= GetViewPos(uvcoordsvar.xy);

    // Sample neighboring pixels
    Pr 	= GetViewPos(uvcoordsvar.xy + vec2( InvAORes.x, 0));
    Pl 	= GetViewPos(uvcoordsvar.xy + vec2(-InvAORes.x, 0));
    Pt 	= GetViewPos(uvcoordsvar.xy + vec2( 0, InvAORes.y));
    Pb 	= GetViewPos(uvcoordsvar.xy + vec2( 0,-InvAORes.y));

    // Calculate tangent basis vectors using the minimu difference
    vec3 dPdu = MinDiff(P, Pr, Pl);
    vec3 dPdv = MinDiff(P, Pt, Pb) * (AORes.y * InvAORes.x);

    // Get the random samples from the noise texture
    vec3 random = texture(bgl_NoiseTexture, uvcoordsvar.xy * NoiseScale).rgb;

    // Calculate the projected size of the hemisphere
    vec2 rayRadiusUV = 0.5 * R * FocalLen / -P.z;
    float rayRadiusPix = rayRadiusUV.x * AORes.x;

    float ao = 1.0;

    // Make sure the radius of the evaluated hemisphere is more than a pixel
    if(rayRadiusPix > 1.0)
    {
        ao = 0.0;
        float numSteps;
        vec2 stepSizeUV;

        // Compute the number of steps
        ComputeSteps(stepSizeUV, numSteps, rayRadiusPix, random.z);

        float alpha = 2.0 * PI / numDirections;

        // Calculate the horizon occlusion of each direction
        for(float d = 0; d < numDirections; ++d)
        {
            float theta = alpha * d;

            // Apply noise to the direction
            vec2 dir = RotateDirections(vec2(cos(theta), sin(theta)), random.xy);
            vec2 deltaUV = dir * stepSizeUV;

            // Sample the pixels along the direction
            ao += HorizonOcclusion(	deltaUV,
                                    P,
                                    dPdu,
                                    dPdv,
                                    random.z,
                                    numSteps);
        }

        // Average the results and produce the final AO
        ao = 1.0 - ao / numDirections * AOStrength;
    }

    fragColor = vec4(ao, 30.0 * P.z, 0.0, 1.0);
}
