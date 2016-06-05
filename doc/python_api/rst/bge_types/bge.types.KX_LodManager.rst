KX_LodManager(PyObjectPlus)
===========================

.. module:: bge.types

base class --- :class:`PyObjectPlus`

.. class:: KX_LodManager(PyObjectPlus)

   This class contains a list of all levels of detail for a KX_GameObject.

   .. attribute:: levels

      Return the list of all levels of detail of the KX_GameObject.
      To select one level (:class:`KX_LodLevel`), you can do:
      KX_GameObject.lodManager.lodLevel[index] and then you can set
      individual attributes for this level of detail.

      :type: list (read only)

   .. attribute:: distanceFactor

      Method to multiply the distance to the camera.

      :type: float
