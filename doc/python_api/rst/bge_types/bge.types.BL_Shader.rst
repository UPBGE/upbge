BL_Shader(EXP_PyObjectPlus)
===========================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.EXP_PyObjectPlus`

.. class:: BL_Shader

   BL_Shader is a class used to compile and use custom shaders scripts.
   This header set the ``#version`` directive, so the user must not define his own `#version`.
   Since 0.3.0, this class is only used with custom 2D filters.

   .. attribute:: enabled

      Set shader enabled to use.

      :type: boolean

   .. attribute:: objectCallbacks

   .. deprecated:: 0.3.0

      The list of python callbacks executed when the shader is used to render an object.
      All the functions can expect as argument the object currently rendered.

      .. code-block:: python

         def callback(object):
             print("render object %r" % object.name)

      :type: list of functions and/or methods

   .. attribute:: bindCallbacks

   .. deprecated:: 0.3.0

      The list of python callbacks executed when the shader is begin used to render.

      :type: list of functions and/or methods

   .. method:: setUniformfv(name, fList)

      Set a uniform with a list of float values

      :arg name: the uniform name
      :type name: string
      :arg fList: a list (2, 3 or 4 elements) of float values
      :type fList: list[float]

   .. method:: delSource()

   .. deprecated:: 0.3.0

      Clear the shader. Use this method before the source is changed with :data:`setSource`.

   .. method:: getFragmentProg()

      Returns the fragment program.

      :return: The fragment program.
      :rtype: string

   .. method:: getVertexProg()

      Get the vertex program.

      :return: The vertex program.
      :rtype: string

   .. method:: isValid()

      Check if the shader is valid.

      :return: True if the shader is valid
      :rtype: boolean

   .. method:: setAttrib(enum)

   .. deprecated:: 0.3.0

      Set attribute location. (The parameter is ignored a.t.m. and the value of "tangent" is always used.)

      :arg enum: attribute location value
      :type enum: integer

   .. method:: setSampler(name, index)

      Set uniform texture sample index.

      :arg name: Uniform name
      :type name: string
      :arg index: Texture sample index.
      :type index: integer

   .. method:: setSource(vertexProgram, fragmentProgram, apply)

   .. deprecated:: 0.3.0

      Set the vertex and fragment programs

      :arg vertexProgram: Vertex program
      :type vertexProgram: string
      :arg fragmentProgram: Fragment program
      :type fragmentProgram: string
      :arg apply: Enable the shader.
      :type apply: boolean

   .. method:: setSourceList(sources, apply)

   .. deprecated:: 0.3.0

      Set the vertex, fragment and geometry shader programs.

      :arg sources: Dictionary of all programs. The keys :data:`vertex`, :data:`fragment` and :data:`geometry` represent shader programs of the same name.
          :data:`geometry` is an optional program.
          This dictionary can be similar to:

          .. code-block:: python

             sources = {
                 "vertex" : vertexProgram,
                 "fragment" : fragmentProgram,
                 "geometry" : geometryProgram
             }

      :type sources: dict
      :arg apply: Enable the shader.
      :type apply: boolean

   .. method:: setUniform1f(name, fx)

      Set a uniform with 1 float value.

      :arg name: the uniform name
      :type name: string
      :arg fx: Uniform value
      :type fx: float

   .. method:: setUniform1i(name, ix)

      Set a uniform with an integer value.

      :arg name: the uniform name
      :type name: string
      :arg ix: the uniform value
      :type ix: integer

   .. method:: setUniform2f(name, fx, fy)

      Set a uniform with 2 float values

      :arg name: the uniform name
      :type name: string
      :arg fx: first float value
      :type fx: float

      :arg fy: second float value
      :type fy: float

   .. method:: setUniform2i(name, ix, iy)

      Set a uniform with 2 integer values

      :arg name: the uniform name
      :type name: string
      :arg ix: first integer value
      :type ix: integer
      :arg iy: second integer value
      :type iy: integer

   .. method:: setUniform3f(name, fx, fy, fz)

      Set a uniform with 3 float values.

      :arg name: the uniform name
      :type name: string
      :arg fx: first float value
      :type fx: float
      :arg fy: second float value
      :type fy: float
      :arg fz: third float value
      :type fz: float

   .. method:: setUniform3i(name, ix, iy, iz)

      Set a uniform with 3 integer values

      :arg name: the uniform name
      :type name: string
      :arg ix: first integer value
      :type ix: integer
      :arg iy: second integer value
      :type iy: integer
      :arg iz: third integer value
      :type iz: integer

   .. method:: setUniform4f(name, fx, fy, fz, fw)

      Set a uniform with 4 float values.

      :arg name: the uniform name
      :type name: string
      :arg fx: first float value
      :type fx: float
      :arg fy: second float value
      :type fy: float
      :arg fz: third float value
      :type fz: float
      :arg fw: fourth float value
      :type fw: float

   .. method:: setUniform4i(name, ix, iy, iz, iw)

      Set a uniform with 4 integer values

      :arg name: the uniform name
      :type name: string
      :arg ix: first integer value
      :type ix: integer
      :arg iy: second integer value
      :type iy: integer
      :arg iz: third integer value
      :type iz: integer
      :arg iw: fourth integer value
      :type iw: integer

   .. method:: setUniformDef(name, type)

      Define a new uniform

      :arg name: the uniform name
      :type name: string
      :arg type: uniform type, one of :ref:`these constants <shader-defined-uniform>`
      :type type: integer

   .. method:: setUniformMatrix3(name, mat, transpose)

      Set a uniform with a 3x3 matrix value

      :arg name: the uniform name
      :type name: string
      :arg mat: A 3x3 matrix [[f, f, f], [f, f, f], [f, f, f]]
      :type mat: 3x3 matrix
      :arg transpose: set to True to transpose the matrix
      :type transpose: boolean

   .. method:: setUniformMatrix4(name, mat, transpose)

      Set a uniform with a 4x4 matrix value

      :arg name: the uniform name
      :type name: string
      :arg mat: A 4x4 matrix [[f, f, f, f], [f, f, f, f], [f, f, f, f], [f, f, f, f]]
      :type mat: 4x4 matrix
      :arg transpose: set to True to transpose the matrix
      :type transpose: boolean

   .. method:: setUniformiv(name, iList)

      Set a uniform with a list of integer values

      :arg name: the uniform name
      :type name: string
      :arg iList: a list (2, 3 or 4 elements) of integer values
      :type iList: list[integer]

   .. method:: setUniformEyef(name)

   .. deprecated:: 0.3.0

      Set a uniform with a float value that reflects the eye being render in stereo mode:
      0.0 for the left eye, 0.5 for the right eye. In non stereo mode, the value of the uniform
      is fixed to 0.0. The typical use of this uniform is in stereo mode to sample stereo textures
      containing the left and right eye images in a top-bottom order.

      :arg name: the uniform name
      :type name: string

   .. method:: validate()

      Validate the shader object.
