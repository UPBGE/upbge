KX_LodManager(PyObjectPlus)
===========================

.. module:: bge.types

base class --- :class:`PyObjectPlus`

.. class:: KX_LodManager(PyObjectPlus)

   This class contains a list of all levels of detail for a KX_GameObject.

   .. attribute:: lodLevel

      Return the list of all levels of detail of the KX_GameObject.
      To select one level (:class:`KX_LodLevel`), you can do:
      KX_GameObject.lodManager.lodLevel[index] and then you can set
      individual attributes for this level of detail.

      :type: list (read only)

   .. attribute:: lodLevelScale

      Method to set the distance of all levels of detail for a game object at the same time.
      For example, if a KX_GameObject has a lod1 with a distance = 25.0 and a lod2 with
      a distance = 50.0 and you set the lodLevelScale to 2.0, lod1.distance will be equal to 50.0
      and lod2 will be equal to 100.0.

      :type: float
