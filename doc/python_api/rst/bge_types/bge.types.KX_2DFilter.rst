KX_2DFilter(BL_Shader)
======================

.. module:: bge.types

base class --- :class:`BL_Shader`

.. class:: KX_2DFilter(BL_Shader)

   2D filter shader object. Can be alterate with :class:`BL_Shader`'s functions.

   .. method:: setTexture(index, bindCode)

      Set specified texture bind code :data:`bindCode` in specified slot :data:`index`. Any call to :data:`setTexture`
      should be followed by a call to :data:`BL_Shader.setSampler` with the same :data:`index`.

      :arg index: The texture slot.
      :type index: integer
      :arg bindCode: The texture bind code/Id.
      :type bindCode: integer
