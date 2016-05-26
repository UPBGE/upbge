KX_2DFilter(BL_Shader)
======================

.. module:: bge.types

base class --- :class:`BL_Shader`

.. class:: KX_2DFilter(BL_Shader)

   2D filter shader object. Can be alterated with :class:`BL_Shader`'s functions.

   .. method:: setTexture(index, bindCode)

      Set specified texture bind code :data:`bindCode` in specified slot :data:`index`. Any call to :data:`setTexture`
      should be followed by a call to :data:`BL_Shader.setSampler` with the same :data:`index`.

      :arg index: The texture slot.
      :type index: integer
      :arg bindCode: The texture bind code/Id.
      :type bindCode: integer

   .. method:: setUniformTexture(textureName, textureBindCode, index)

      Set uniform sampler2D textureName in the 2D Filter fragment shader.

      :arg name: Uniform sampler2D name in the 2D Filter.
      :type name: string
      :arg bindCode: The bind code/Id of the texture we want to bind.
      :type bindCode: integer
      :arg index: The emplacement in range 0 to 7 included where we want to assign the texture (optional argument).
      :type index: integer
