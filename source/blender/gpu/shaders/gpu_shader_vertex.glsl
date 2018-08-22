#ifdef USE_OPENSUBDIV
in vec3 normal;
in vec4 position;

out block {
	VertexData v;
} outpt;
#endif

#ifdef USE_INSTANCING
in mat3 ininstmatrix;
in vec3 ininstposition;
in vec4 ininstcolor;

out vec4 varinstcolor;
out mat4 varinstmat;
out mat4 varinstinvmat;

uniform mat4 unfviewmat;
#endif

attribute vec4 weights;
attribute ivec4 indices;
attribute int numbones;
uniform bool useshwskin;
uniform mat4 bonematrices[128];

out vec3 varposition;
out vec3 varnormal;

#ifdef CLIP_WORKAROUND
varying float gl_ClipDistance[6];
#endif

void hardware_skinning(in vec4 position, in vec3 normal, out vec4 transpos, out vec3 transnorm)
{
	transpos = vec4(0.0);
	transnorm = vec3(0.0);

	ivec4 curidx = indices;
	vec4 curweight = weights;

	for (int i = 0; i < numbones; ++i) {
		mat4 m44 = bonematrices[curidx.x];

		transpos += m44 * position * curweight.x;

		mat3 m33 = mat3(m44);

		transnorm += m33 * normal * curweight.x;

		curidx = curidx.yzwx;
		curweight = curweight.yzwx;
	}
}

/* Color, keep in sync with: gpu_shader_vertex_world.glsl */

float srgb_to_linearrgb(float c)
{
	if (c < 0.04045)
		return (c < 0.0) ? 0.0 : c * (1.0 / 12.92);
	else
		return pow((c + 0.055) * (1.0 / 1.055), 2.4);
}

void srgb_to_linearrgb(vec3 col_from, out vec3 col_to)
{
	col_to.r = srgb_to_linearrgb(col_from.r);
	col_to.g = srgb_to_linearrgb(col_from.g);
	col_to.b = srgb_to_linearrgb(col_from.b);
}

void srgb_to_linearrgb(vec4 col_from, out vec4 col_to)
{
	col_to.r = srgb_to_linearrgb(col_from.r);
	col_to.g = srgb_to_linearrgb(col_from.g);
	col_to.b = srgb_to_linearrgb(col_from.b);
	col_to.a = col_from.a;
}

bool is_srgb(int info)
{
#ifdef USE_NEW_SHADING
	return (info == 1)? true: false;
#else
	return false;
#endif
}

void set_var_from_attr(float attr, int info, out float var)
{
	var = attr;
}

void set_var_from_attr(vec2 attr, int info, out vec2 var)
{
	var = attr;
}

void set_var_from_attr(vec3 attr, int info, out vec3 var)
{
	if (is_srgb(info)) {
		srgb_to_linearrgb(attr, var);
	}
	else {
		var = attr;
	}
}

void set_var_from_attr(vec4 attr, int info, out vec4 var)
{
	if (is_srgb(info)) {
		srgb_to_linearrgb(attr, var);
	}
	else {
		var = attr;
	}
}

/* end color code */


void main()
{
#ifndef USE_OPENSUBDIV
	vec4 position = gl_Vertex;
	vec3 normal = gl_Normal;
#endif

#ifdef USE_INSTANCING
	mat4 instmat = mat4(vec4(ininstmatrix[0], ininstposition.x),
						vec4(ininstmatrix[1], ininstposition.y),
						vec4(ininstmatrix[2], ininstposition.z),
						vec4(0.0, 0.0, 0.0, 1.0));

	varinstmat = transpose(instmat);
#if !defined(GPU_ATI)
	varinstinvmat = inverse(varinstmat);
#else
	varinstinvmat = varinstmat;
#endif
	varinstcolor = ininstcolor;

	position *= instmat;
	normal *= ininstmatrix;
#endif

// 	if (useshwskin) {
		hardware_skinning(position, normal, position, normal);
// 	}

	vec4 co = gl_ModelViewMatrix * position;

	varposition = co.xyz;
	varnormal = normalize(gl_NormalMatrix * normal);
	gl_Position = gl_ProjectionMatrix * co;

#ifdef CLIP_WORKAROUND
	int i;
	for (i = 0; i < 6; i++)
		gl_ClipDistance[i] = dot(co, gl_ClipPlane[i]);
#elif !defined(GPU_ATI)
	// Setting gl_ClipVertex is necessary to get glClipPlane working on NVIDIA
	// graphic cards, while on ATI it can cause a software fallback.
	gl_ClipVertex = co;
#endif

#ifdef USE_OPENSUBDIV
	outpt.v.position = co;
	outpt.v.normal = varnormal;
#endif
