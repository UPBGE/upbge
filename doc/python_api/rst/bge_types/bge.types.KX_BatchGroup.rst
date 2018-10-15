KX_BatchGroup(EXP_Value)
========================

base class --- :class:`EXP_Value`

.. class:: KX_BatchGroup(EXP_Value)

   The batch group is class containing a mesh resulting of the merging of meshes used by objects.
   The meshes are merged with the transformation of the objects using it.
   An instance of this class is not owned by one object but shared between objects.
   In consideration an instance of :class:`KX_BatchGroup` have to instanced with as argument a list of at least one object containing the meshes to merge.
   This can be done in a way similar to:

   .. code-block:: python

      import bge

      scene = bge.logic.getCurrentScene()

      batchGroup = types.KX_BatchGroup([scene.objects["Cube"], scene.objects["Plane"]])

   .. warning::

      Rendering settings unique to objects such as :data:`KX_GameObject.layer` and :data:`KX_GameObject.color` are shared when using batch groups.
      These settings are taken from object :attr:`referenceObject`.

   .. attribute:: objects

      The list of the objects merged. (read only)

      :type: :class:`EXP_ListValue` of :class:`KX_GameObject`

   .. attribute:: referenceObject

      The object used for object rendering settings (layer, color...).

   .. method:: merge(objects)

      Merge meshes using the transformation of the objects using them.

      :arg objects: The objects to merge.
      :type object: :class:`EXP_ListValue` of :class:`KX_GameObject`

   .. method:: split(objects)

      Split the meshes of the objects using them and restore their original meshes.

      :arg objects: The objects to unmerge.
      :type object: :class:`EXP_ListValue` of :class:`KX_GameObject`

   .. method:: destruct()

      Destruct the batch group and restore all the objects to their original meshes.
