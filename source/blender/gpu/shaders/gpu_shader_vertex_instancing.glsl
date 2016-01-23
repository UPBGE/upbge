in mat3 ininstmatrix;
in vec3 ininstposition;
in vec4 ininstcolor;

varying vec4 varinstcolor;
varying mat4 varinstmat;
varying mat4 varinstinvmat;
varying mat4 varinstlocaltoviewmat;
varying mat4 varinstinvlocaltoviewmat;

uniform mat4 unfviewmat;

varying vec3 varposition;
varying vec3 varnormal;

#ifdef CLIP_WORKAROUND
varying float gl_ClipDistance[6];
#endif

void main()
{
	mat4 instmat = mat4(vec4(ininstmatrix[0], ininstposition.x),
						vec4(ininstmatrix[1], ininstposition.y),
						vec4(ininstmatrix[2], ininstposition.z),
						vec4(0.0, 0.0, 0.0, 1.0));
	varinstmat = transpose(instmat);
	varinstinvmat = inverse(varinstmat);
	varinstlocaltoviewmat = unfviewmat * varinstmat;
	varinstinvlocaltoviewmat = inverse(varinstlocaltoviewmat);
	varinstcolor = ininstcolor;

	vec4 co = gl_ModelViewMatrix * (gl_Vertex * instmat);
	varnormal = normalize(gl_NormalMatrix * (gl_Normal * ininstmatrix));

	varposition = co.xyz;
	gl_Position = gl_ProjectionMatrix * co;

#ifdef CLIP_WORKAROUND
	int i;
	for(i = 0; i < 6; i++)
		gl_ClipDistance[i] = dot(co, gl_ClipPlane[i]);
#elif !defined(GPU_ATI)
	// Setting gl_ClipVertex is necessary to get glClipPlane working on NVIDIA
	// graphic cards, while on ATI it can cause a software fallback.
	gl_ClipVertex = co; 
#endif  
