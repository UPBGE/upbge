KX_CubeMap(CValue, RAS_CubeMap)
======================

.. module:: bge.types

base class --- :class:`CValue`

.. class:: KX_CubeMap(CValue)

   Python API for realtime cube map textures.

   .. attribute:: autoUpdate

      Choose to update automatically each frame the cube map or not.

      :type: boolean

   .. attribute:: viewpointObject

      The object where the cube map will render the scene.

      :type: :class:`KX_GameObject`

   .. attribute:: ignoreLayers

      The layers to ignore when rendering the cube map.

      :type: bitfield

   .. attribute:: clipStart

      The projection view matrix near plane, used for culling.

      :type: float

   .. attribute:: clipEnd

      The projection view matrix far plane, used for culling.

      :type: float

   .. method:: update()

      Request to update this cube map during the rendering stage. This function is effective only when :data:`autoUpdate` is disabled.
