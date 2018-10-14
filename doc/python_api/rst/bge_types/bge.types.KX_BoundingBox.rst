KX_BoundingBox(EXP_PyObjectPlus)
================================

base class --- :class:`EXP_PyObjectPlus`

.. class:: KX_BoundingBox(EXP_PyObjectPlus)

   A bounding volume box of a game object. Used to get and alterate the volume box or AABB.

   .. code-block:: python

      from bge import logic
      from mathutils import Vector
      
      owner = logic.getCurrentController().owner
      box = owner.cullingBox
      
      # Disable auto update to allow modify the box.
      box.autoUpdate = False
      
      print(box)
      # Increase the height of the box of 1.
      box.max = box.max + Vector((0, 0, 1))
      
      print(box)

   .. attribute:: min

      The minimal point in x, y and z axis of the bounding box.

      :type: :class:`mathutils.Vector`

   .. attribute:: max

      The maximal point in x, y and z axis of the bounding box.

      :type: :class:`mathutils.Vector`

   .. attribute:: center

      The center of the bounding box. (read only)

      :type: :class:`mathutils.Vector`

   .. attribute:: radius

      The radius of the bounding box. (read only)

      :type: float

   .. attribute:: autoUpdate

      Allow to update the bounding box if the mesh is modified.

      :type: boolean
