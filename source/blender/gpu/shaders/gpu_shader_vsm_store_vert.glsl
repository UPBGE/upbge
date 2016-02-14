varying vec4 v_position;

#ifdef USE_INSTANCING
in mat3 ininstmatrix;
in vec3 ininstposition;
#endif

void main()
{
#ifdef USE_INSTANCING
	mat4 instmat = mat4(vec4(ininstmatrix[0], ininstposition.x),
						vec4(ininstmatrix[1], ininstposition.y),
						vec4(ininstmatrix[2], ininstposition.z),
						vec4(0.0, 0.0, 0.0, 1.0));

	v_position = gl_ProjectionMatrix * gl_ModelViewMatrix * (gl_Vertex * instmat);
	gl_Position = v_position;
#else
	gl_Position = gl_ProjectionMatrix * gl_ModelViewMatrix * gl_Vertex;
	v_position = gl_Position;
#endif
}
