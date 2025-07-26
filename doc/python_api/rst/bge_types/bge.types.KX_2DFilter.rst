KX_2DFilter(BL_Shader)
======================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.BL_Shader`

.. class:: KX_2DFilter

   2D filter shader object. Can be alternated with :class:`~bge.types.BL_Shader`'s functions.

   .. note::

      Since version 0.5+, the following builtin uniforms are available via the UBO ``g_data`` (type ``bgl_Data``):

         ``g_data.width``
            Rendered texture width (float)

         ``g_data.height``
            Rendered texture height (float)

         ``g_data.coo_offset[9]``
            Texture coordinate offsets for 3x3 filters (vec4[9])

      The following builtin samplers are available:

         ``bgl_RenderedTexture``
            RENDERED_TEXTURE_UNIFORM

         ``bgl_DepthTexture``
            DEPTH_TEXTURE_UNIFORM

      The following builtin attributes are available:

         ``bgl_TexCoord``
            texture coordinates / UV

         ``fragColor``
            returned result of 2D filter

Example:

.. code-block:: glsl

   void main()
   {
     fragColor = texture(bgl_RenderedTexture, bgl_TexCoord.xy) * vec4(0.0, 0.0, 1.0, 1.0); // will make the rendered image blueish
   }

Example using offsets:

.. code-block:: glsl

   for (int i = 0; i < 9; i++) {
     vec2 offset = g_data.coo_offset[i].xy;
     vec4 sample = texture(bgl_RenderedTexture, bgl_TexCoord.xy + offset);
     // ...
   }

.. attribute:: mipmap

   Request mipmap generation of the render ``bgl_RenderedTexture`` texture.

   :type: boolean

.. attribute:: offScreen

   The custom off screen (framebuffer in 0.3+) the filter render to (read-only).

   :type: :class:`~bge.types.KX_2DFilterFrameBuffer` or None

.. method:: setTexture(textureName="", gputexture)

   Set specified GPUTexture as uniform for the 2D filter.

   :arg textureName: The name of the texture in the 2D filter code. For example if you declare:
      uniform sampler2D myTexture;
      you will have to call filter.setTexture("myTexture", gputex).
   :type textureName: string
   :arg gputexture: The gputexture (see gpu module documentation).
   :type gputexture: :class:`~gpu.types.GPUTexture`

.. method:: addOffScreen(width=None, height=None, mipmap=False)

   Register a custom off screen (framebuffer in 0.3+) to render the filter to.

   :arg width: In 0.3+, always canvas width (optional).
   :type width: integer
   :arg height: In 0.3+, always canvas height (optional).
   :type height: integer
   :arg mipmap: True if the color texture generate mipmap at the end of the filter rendering (optional).
   :type mipmap: boolean

.. method:: removeOffScreen()

   Unregister the custom off screen (framebuffer in 0.3+) the filter render to.