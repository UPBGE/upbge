varying vec4 v_position;

#ifdef USE_INSTANCING
in mat3 ininstmatrix;
in vec3 ininstposition;

uniform int unfinstmode;
#endif

void main()
{
	vec4 vertex = gl_Vertex;
#ifdef USE_INSTANCING
	if (unfinstmode == 1) {
		mat4 instmat = mat4(vec4(ininstmatrix[0], ininstposition.x),
							vec4(ininstmatrix[1], ininstposition.y),
							vec4(ininstmatrix[2], ininstposition.z),
							vec4(0.0, 0.0, 0.0, 1.0));
		vertex *= instmat;
	}
#endif

	v_position = gl_ProjectionMatrix * gl_ModelViewMatrix * vertex;
	gl_Position = v_position;
}
