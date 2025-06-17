KX_2DFilterOffScreen(EXP_Value)
===============================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.EXP_Value`

.. class:: KX_2DFilterFrameBuffer

   2D filter custom off screen (framebuffer in 0.3+).

   .. attribute:: width

      The off screen width, always canvas width in 0.3+ (read-only).

      :type: integer

   .. attribute:: height

      The off screen height, always canvas height in 0.3+ (read-only).

      :type: integer

   .. method:: getColorTexture()
      Returns the buffer color texture.

      :return: Texture object.
      :rtype: :class:`~gpu.types.GPUTexture`

   .. method:: getDepthTexture()
      Returns the buffer depth texture.

      :return: Texture object.
      :rtype: :class:`~gpu.types.GPUTexture`
