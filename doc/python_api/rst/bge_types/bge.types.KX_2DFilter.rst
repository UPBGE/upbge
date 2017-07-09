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

   .. attribute:: mipmap

      Request mipmap generation of the render `bgl_RenderedTexture` texture. Accessing mipmapping level is similar to:

      .. code-block:: glsl

         uniform sampler2D bgl_RenderedTexture;

         void main()
         {
             float level = 2.0; // mipmap level
             gl_FragColor = textureLod(bgl_RenderedTexture, gl_TexCoord[0].st, level);
         }

      :type: boolean

   .. attribute:: offScreen

      The custom off screen the filter render to (read-only).

      :type: :class:`bge.types.KX_2DFilterOffScreen` or None

   .. method:: setTexture(index, bindCode, samplerName="")

      Set specified texture bind code :data:`bindCode` in specified slot :data:`index`. Any call to :data:`setTexture`
      should be followed by a call to :data:`BL_Shader.setSampler` with the same :data:`index` if :data:`sampleName` is not specified.

      :arg index: The texture slot.
      :type index: integer
      :arg bindCode: The texture bind code/Id.
      :type bindCode: integer
      :arg samplerName: The shader sampler name set to :data:`index` if :data:`samplerName` is passed in the function. (optional)
      :type samplerName: string

   .. method:: setCubeMap(index, bindCode, samplerName="")

      Set specified cube map texture bind code :data:`bindCode` in specified slot :data:`index`. Any call to :data:`setCubeMap`
      should be followed by a call to :data:`BL_Shader.setSampler` with the same :data:`index` if :data:`sampleName` is not specified.

      :arg index: The texture slot.
      :type index: integer
      :arg bindCode: The cube map texture bind code/Id.
      :type bindCode: integer
      :arg samplerName: The shader sampler name set to :data:`index` if :data:`samplerName` is passed in the function. (optional)
      :type samplerName: string

   .. method:: addOffScreen(slots, depth=False, width=-1, height=-1, hdr=bge.render.HDR_NONE, mipmap=False)

      Register a custom off screen to render the filter to.

      :arg slots: The number of color texture attached to the off screen, between 0 and 8 excluded.
      :type slots: integer
      :arg depth: True of the off screen use a depth texture (optional).
      :type depth: boolean
      :arg width: The off screen width, -1 if it can be resized dynamically when the viewport dimensions changed (optional).
      :type width: integer
      :arg height: The off screen height, -1 if it can be resized dynamically when the viewport dimensions changed (optional).
      :type height: integer
      :arg hdr: The image quality HDR of the color textures (optional).
      :type hdr: one of :ref:`these constants<render-hdr>`
      :arg mipmap: True if the color texture generate mipmap at the end of the filter rendering (optional).
      :type mipmap: boolean

      .. note::
        If the off screen is created using a dynamic size (`width` and `height` to -1) its bind codes will be unavailable before
        the next render of the filter and the it can change when the viewport is resized.

   .. method:: removeOffScreen()

      Unregister the custom off screen the filter render to.
