KX_MeshProxy(EXP_Value)
=======================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.EXP_Value`

.. class:: KX_MeshProxy

   A mesh object.

   You can only change the vertex properties of a mesh object, not the mesh topology.

   To use mesh objects effectively, you should know a bit about how the game engine handles them.

   #. Mesh Objects are converted from Blender at scene load.
   #. The Converter groups polygons by Material.  This means they can be sent to the renderer efficiently.  A material holds:

      #. The texture.
      #. The Blender material.
      #. The Tile properties
      #. The face properties - (From the "Texture Face" panel)
      #. Transparency & z sorting
      #. Light layer
      #. Polygon shape (triangle/quad)
      #. Game Object

   #. Vertices will be split by face if necessary.  Vertices can only be shared between faces if:

      #. They are at the same position
      #. UV coordinates are the same
      #. Their normals are the same (both polygons are "Set Smooth")
      #. They are the same color, for example: a cube has 24 vertices: 6 faces with 4 vertices per face.

   The correct method of iterating over every :class:`~bge.types.KX_VertexProxy` in a game object

   .. code-block:: python

      from bge import logic

      cont = logic.getCurrentController()
      object = cont.owner

      for mesh in object.meshes:
         for m_index in range(len(mesh.materials)):
            for v_index in range(mesh.getVertexArrayLength(m_index)):
               vertex = mesh.getVertex(m_index, v_index)
               # Do something with vertex here...
               # ... eg: color the vertex red.
               vertex.color = [1.0, 0.0, 0.0, 1.0]

   .. attribute:: materials

      :type: list of :class:`~bge.types.KX_BlenderMaterial` type

   .. attribute:: numPolygons

      :type: integer

   .. attribute:: numMaterials

      :type: integer

   .. attribute:: polygons

      Returns the list of polygons of this mesh.

      :type: :class:`~bge.types.KX_PolyProxy` list (read only)

   .. method:: getMaterialName(matid)

      Gets the name of the specified material.

      :arg matid: the specified material.
      :type matid: integer
      :return: the attached material name.
      :rtype: string

   .. method:: getTextureName(matid)

      Gets the name of the specified material's texture.

      :arg matid: the specified material
      :type matid: integer
      :return: the attached material's texture name.
      :rtype: string

   .. method:: getVertexArrayLength(matid)

      Gets the length of the vertex array associated with the specified material.

      There is one vertex array for each material.

      :arg matid: the specified material
      :type matid: integer
      :return: the number of verticies in the vertex array.
      :rtype: integer

   .. method:: getVertex(matid, index)

      Gets the specified vertex from the mesh object.

      :arg matid: the specified material
      :type matid: integer
      :arg index: the index into the vertex array.
      :type index: integer
      :return: a vertex object.
      :rtype: :class:`~bge.types.KX_VertexProxy`

   .. method:: getPolygon(index)

      Gets the specified polygon from the mesh.

      :arg index: polygon number
      :type index: integer
      :return: a polygon object.
      :rtype: :class:`~bge.types.KX_PolyProxy`

   .. method:: transform(matid, matrix)

      Transforms the vertices of a mesh.

      :arg matid: material index, -1 transforms all.
      :type matid: integer
      :arg matrix: transformation matrix.
      :type matrix: 4x4 matrix [[float]]

   .. method:: transformUV(matid, matrix, uv_index=-1, uv_index_from=-1)

      Transforms the vertices UV's of a mesh.

      :arg matid: material index, -1 transforms all.
      :type matid: integer
      :arg matrix: transformation matrix.
      :type matrix: 4x4 matrix [[float]]
      :arg uv_index: optional uv index, -1 for all, otherwise 0 or 1.
      :type uv_index: integer
      :arg uv_index_from: optional uv index to copy from, -1 to transform the current uv.
      :type uv_index_from: integer

   .. method:: replaceMaterial(matid, material)

      Replace the material in slot :data:`matid` by the material :data:`material`.

      :arg matid: The material index.
      :type matid: integer
      :arg material: The material replacement.
      :type material: :class:`~bge.types.KX_BlenderMaterial`

      .. warning::

         Changing the material of a mesh used by many objects can be slow. This function should be not called every frames

