KX_LodLevel(PyObjectPlus)
=========================

.. module:: bge.types

base class --- :class:`PyObjectPlus`

.. class:: KX_LodLevel(PyObjectPlus)

   A single Lod Level for a KX_GameObject.

   .. attribute:: meshName

      The name of the mesh used at this lod level.

      :type: string (read only)

   .. attribute:: useHysteresis

      return true is this lod level use hysteresis override and false if not.

      :type: boolean (read only)

   .. attribute:: distance

      Distance to begin using this level of detail.

      :type: float (0.0 to 99999999.0)

   .. attribute:: hysteresis

      Minimum distance change required to transition to the previous level of detail.

      :type: float (0.0 to 100.0 (percents))
