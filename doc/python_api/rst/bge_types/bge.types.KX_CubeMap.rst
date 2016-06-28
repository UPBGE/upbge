KX_CubeMap(CValue)
==================

.. module:: bge.types

base class --- :class:`CValue`

.. class:: KX_CubeMap(CValue)

   Python API for realtime cube map textures.

   .. code-block:: python

      import bge

      scene = bge.logic.getCurrentScene()
      # The object using a realtime cube map in its material.
      obj = scene.objects["Suzanne"]

      mat = obj.meshes[0].materials[0]
      # Obtain the realtime cube map from the material texture.
      cubemap = mat.textures[0].cubeMap

      # Set the render position to the "Cube" object position.
      cubemap.viewpointObject = scene.objects["Cube"]

      # Change the culling clip start and clip end.
      cubemap.clipStart = 5.0
      cubemap.clipEnd = 20.0

      # Disable render on third layer.
      cubemap.ignoreLayers = (1 << 2)

      # Disable per frame update.
      cubemap.autoUpdate = False
      # Ask to update for this frame only.
      cubemap.update()

   .. attribute:: autoUpdate

      Choose to update automatically each frame the cube map or not.

      :type: boolean

   .. attribute:: viewpointObject

      The object where the cube map will render the scene.

      :type: :class:`KX_GameObject`

   .. attribute:: enabled

      Enable the cube map to render the scene.

      :type: boolean

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
