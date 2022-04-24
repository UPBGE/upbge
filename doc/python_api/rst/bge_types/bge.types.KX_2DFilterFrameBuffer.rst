KX_2DFilterOffScreen(EXP_Value)
===============================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.EXP_Value`

.. class:: KX_2DFilterFrameBuffer

   2D filter custom off screen (framebuffer in 0.3.0).

   .. attribute:: width

      The off screen width, always canvas width in 0.3.0 (read-only).

      :type: integer

   .. attribute:: height

      The off screen height, always canvas height in 0.3.0 (read-only).

      :type: integer

   .. attribute:: colorBindCodes

      The bind code of the color textures attached to the off screen (read-only).

      .. warning:: If the off screen can be resized dynamically (:data:`width` of :data:`height` equal to -1), the bind codes may change.

      :type: list of 8 integers

   .. attribute:: depthBindCode

      The bind code of the depth texture attached to the off screen (read-only).

      .. warning:: If the off screen can be resized dynamically (:data:`width` of :data:`height` equal to -1), the bind code may change.

      :type: integer
