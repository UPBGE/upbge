in vec4 weight;
in vec4 index;
in float numbones;
in mat4 bonematrices[128];
flat out vec3 varposition;
flat out vec3 varnormal;

void hardware_skinning(in vec4 position, in vec3 normal, out vec4 transpos, out vec3 transnorm)
{
        transpos = vec4(0.0);
        transnorm = vec3(0.0);

        vec4 curidx = index;
        vec4 curweight = weight;

        for (int i = 0; i < int(numbones); ++i)
        {
                mat4 m44 = bonematrices[int(curidx.x)];

                transpos += m44 * position * curweight.x;

                mat3 m33 = mat3(m44[0].xyz,
                                m44[1].xyz,
                                m44[2].xyz);

                transnorm += m33 * normal * curweight.x;

                curidx = curidx.yzwx;
                curweight = curweight.yzwx;
        }
}

void main()
{

    hardware_skinning(gl_Vertex, gl_Normal, position, normal);

    vec4 co = gl_ModelViewMatrix * position;

    varposition = co.xyz;
    varnormal = normalize(gl_NormalMatrix * normal);
    gl_Position = gl_ProjectionMatrix * co;
}

