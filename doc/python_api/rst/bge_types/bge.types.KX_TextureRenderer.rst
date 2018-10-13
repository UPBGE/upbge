KX_TextureRenderer(EXP_Value)
=============================

base class --- :class:`EXP_Value`

.. class:: KX_TextureRenderer(EXP_Value)

   Python API for object doing a render stored in a texture.

   .. attribute:: autoUpdate

      Choose to update automatically each frame the texture renderer or not.

      :type: boolean

   .. attribute:: viewpointObject

      The object where the texture renderer will render the scene.

      :type: :class:`KX_GameObject`

   .. attribute:: enabled

      Enable the texture renderer to render the scene.

      :type: boolean

   .. attribute:: ignoreLayers

      The layers to ignore when rendering.

      :type: bitfield

   .. attribute:: clipStart

      The projection view matrix near plane, used for culling.

      :type: float

   .. attribute:: clipEnd

      The projection view matrix far plane, used for culling.

      :type: float

   .. attribute:: lodDistanceFactor

      The factor to multiply distance to camera to adjust levels of detail.
      A float < 1.0f will make the distance to camera used to compute
      levels of detail decrease.

      :type: float

   .. method:: update()

      Request to update this texture renderer during the rendering stage. This function is effective only when :data:`autoUpdate` is disabled.
