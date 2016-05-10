KX_2DFilter(RAS_2DFilter, BL_Shader)
====================================

.. module:: bge.types

base class --- :class:`RAS_2DFilter` :class:`BL_Shader`

.. class:: KX_2DFilter(RAS_2DFilter, BL_Shader)

   This is the python interface to GLSL 2D Filters in the game engine.
   All the methods from :class:`BL_Shader` are available too.

   .. method:: setTexture(textureName, textureBindCode, index(optional))

      Bind texture named textureName in the GLSL 2D Filter (uniform sampler2D textureName;) with the corresponding textureBindCode.
      index is an optional argument: each GLSL 2D Filter has 7 slots(indexes) that can be used to bind a texture with his bindcode.
      If the slot is not explicitly defined, the setTexture function will automatically choose an empty slot to bind the texture.
      If all slots are already in use, you can choose to use index option to force the slot to be used. This will overwrite the previous slot with the new defined bindCode.

      :arg textureName: Specifies the name of the sampler2D used in the GLSL 2D Filter.
      :type textureName: string
      :arg textureBindCode: Specifies the bindCode of the texture that will be used in the GLSL 2D Filter.
      This bindCode can be generally found like that: KX_GameObject.meshes[meshIndex].materials[materialIndex].textures[textureIndex].bindCode.
      :type textureBindCode: integer
      :arg index: Specifies the slot to be used to bind the texture.
      :type index: integer (beetween 1 and 7 included)