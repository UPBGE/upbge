in vec4 bgeOfsPos;
uniform mat4 ProjectionMatrix;
uniform mat4 ModelViewMatrix;

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

	gl_Position = ProjectionMatrix * ModelViewMatrix * (bgeOfsPos * instmat);
#else
	gl_Position = ProjectionMatrix * ModelViewMatrix * bgeOfsPos;
#endif
}
