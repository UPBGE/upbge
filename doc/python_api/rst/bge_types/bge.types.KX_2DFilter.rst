KX_2DFilter(BL_Shader)
======================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.BL_Shader`

.. class:: KX_2DFilter

   2D filter shader object. Can be alternated with :class:`~bge.types.BL_Shader`'s functions.

   .. warning::

      The vertex shader must not apply modelview and projection transformation. It should be similar to:

      .. code-block:: glsl

        in vec4 pos;
        in vec2 texCoord;

        out vec4 bgl_TexCoord;

        void main(void)
        {
            gl_Position = pos;
            bgl_TexCoord = vec4(texCoord, 0.0, 0.0);
        }

   .. attribute:: mipmap

      Request mipmap generation of the render `bgl_RenderedTexture` texture. Accessing mipmapping level is similar to:

      .. code-block:: glsl

         uniform sampler2D bgl_RenderedTexture;
         in vec4 bgl_TexCoord;
         out vec4 fragColor;

         void main()
         {
             float level = 2.0; // mipmap level
             fragColor = textureLod(bgl_RenderedTexture, bgl_TexCoord.xy, level);
         }

      :type: boolean

   .. attribute:: offScreen

      The custom off screen (framebuffer in 0.3.0) the filter render to (read-only).

      :type: :class:`~bge.types.KX_2DFilterFrameBuffer` or None

   .. method:: setTexture(index, bindCode, samplerName="")

      Set specified texture bind code :data:`bindCode` in specified slot :data:`index`. Any call to :data:`setTexture`
      should be followed by a call to :data:`BL_Shader.setSampler <bge.types.BL_Shader.setSampler>` with the same :data:`index` if :data:`sampleName` is not specified.

      :arg index: The texture slot.
      :type index: integer
      :arg bindCode: The texture bind code/Id.
      :type bindCode: integer
      :arg samplerName: The shader sampler name set to :data:`index` if :data:`samplerName` is passed in the function. (optional)
      :type samplerName: string

   .. method:: setCubeMap(index, bindCode, samplerName="")

      Set specified cube map texture bind code :data:`bindCode` in specified slot :data:`index`. Any call to :data:`setCubeMap`
      should be followed by a call to :data:`BL_Shader.setSampler <bge.types.BL_Shader.setSampler>` with the same :data:`index` if :data:`sampleName` is not specified.

      :arg index: The texture slot.
      :type index: integer
      :arg bindCode: The cube map texture bind code/Id.
      :type bindCode: integer
      :arg samplerName: The shader sampler name set to :data:`index` if :data:`samplerName` is passed in the function. (optional)
      :type samplerName: string

   .. method:: addOffScreen(slots, width=None, height=None, mipmap=False)

      Register a custom off screen (framebuffer in 0.3.0) to render the filter to.

      :arg slots: The number of color texture attached to the off screen, between 0 and 8 excluded.
      :type slots: integer
      :arg width: In 0.3.0, always canvas width (optional).
      :type width: integer
      :arg height: In 0.3.0, always canvas height (optional).
      :type height: integer
      :arg mipmap: True if the color texture generate mipmap at the end of the filter rendering (optional).
      :type mipmap: boolean

      .. note::
        If the off screen is created using a dynamic size (`width` and `height` to -1) its bind codes will be unavailable before
        the next render of the filter and the it can change when the viewport is resized.

   .. method:: removeOffScreen()

      Unregister the custom off screen (framebuffer in 0.3.0) the filter render to.
