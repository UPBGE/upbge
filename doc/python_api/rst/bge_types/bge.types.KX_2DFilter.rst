KX_2DFilter(BL_Shader)
======================

.. module:: bge.types

base class --- :class:`BL_Shader`

.. class:: KX_2DFilter(BL_Shader)

   2D filter shader object. Can be alterated with :class:`BL_Shader`'s functions.

   .. warning::

      The vertex shader must not apply modelview and projection transformation. It should be similar to:

      .. code-block:: glsl

         void main()
         {
             gl_Position = gl_Vertex;
         }

   .. method:: setTexture(index, bindCode, samplerName)

      Set specified texture bind code :data:`bindCode` in specified slot :data:`index`. Any call to :data:`setTexture`
      should be followed by a call to :data:`BL_Shader.setSampler` with the same :data:`index`.

      :arg index: The texture slot.
      :type index: integer
      :arg bindCode: The texture bind code/Id.
      :type bindCode: integer
      :arg samplerName: The shader sampler name set to :data:`index` if :data:`samplerName` is passed in the function. (optional)
      :type samplerName: string
