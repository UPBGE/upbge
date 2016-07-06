KX_OffScreen(CValue)
====================

.. module:: bge.types

base class --- :class:`CValue`

.. class:: KX_OffScreen(CValue)

   An off-screen render buffer object. 

   Use :func:`bge.render.offScreenCreate` to create it.
   Currently it can only be used in the :class:`bge.texture.ImageRender`
   constructor to render on a FBO rather than the default viewport.

  .. attribute:: width

     The width in pixel of the FBO

     :type: integer

  .. attribute:: height

     The height in pixel of the FBO

     :type: integer

  .. attribute:: color

     The underlying OpenGL bind code of the texture object that holds
     the rendered image, 0 if the FBO is using RenderBuffer.
     The choice between RenderBuffer and Texture is determined
     by the target argument of :func:`bge.render.offScreenCreate`.

     :type: integer