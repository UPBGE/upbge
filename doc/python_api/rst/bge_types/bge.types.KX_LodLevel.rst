KX_LodLevel(PyObjectPlus)
=========================

.. module:: bge.types

base class --- :class:`PyObjectPlus`

.. class:: KX_LodLevel(PyObjectPlus)

   A single Lod Level for a KX_GameObject.

   .. attribute:: mesh

      The mesh used at this lod level.

      :type: :class:`RAS_MeshObject`(read only)

   .. attribute:: level

      The id of the KX_LodLevel.

      :type: integer(read only)

   .. attribute:: useMesh

      Return True if this KX_LodLevel has a mesh, False if not.

      :type: boolean(read only)

   .. attribute:: useMaterial

      Return True if this KX_LodLevel has a mesh with at least one material, False if not.

      :type: boolean(read only)

   .. attribute:: useHysteresis

      return true is this lod level use hysteresis override and false if not.

      :type: boolean (read only)

   .. attribute:: distance

      Distance to begin using this level of detail.

      :type: float (0.0 to infinite)

   .. attribute:: hysteresis

      Minimum distance change required to transition to the previous level of detail.

      :type: float (0.0 to 100.0 (percents))
