KX_CubeMap(KX_TextureRenderer)
==============================

base class --- :class:`KX_TextureRenderer`

.. class:: KX_CubeMap(KX_TextureRenderer)

   Python API for realtime cube map textures.

   .. code-block:: python

      import bge

      scene = bge.logic.getCurrentScene()
      # The object using a realtime cube map in its material.
      obj = scene.objects["Suzanne"]

      mat = obj.meshes[0].materials[0]
      # Obtain the realtime cube map from the material texture.
      cubemap = mat.textures[0].renderer

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
