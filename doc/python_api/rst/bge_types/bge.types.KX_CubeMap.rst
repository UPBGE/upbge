KX_CubeMap(CValue, RAS_CubeMap)
======================

.. module:: bge.types

base class --- :class:`CValue` :class:`RAS_CubeMap`

.. class:: KX_CubeMap(CValue, RAS_CubeMap)

   Python API for CubeMap textures. Can be accessed like: own.meshes[0].materials[0].textures[0].cubeMap(.attribute/method).

   .. attribute:: update

      Choose to update the cubeMap or not.

      :type: boolean

   .. attribute:: viewpointObject

      The KX_GameObject which will be used to place (at the KX_GameObject's position) the camera to render cubeMaps.

      :type: :class:`KX_GameObject`
