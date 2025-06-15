KX_MeshProxy(EXP_Value)
=======================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.EXP_Value`

.. class:: KX_MeshProxy

   A mesh object.

   You can only read the vertex properties of a mesh object. In upbge 0.3+, KX_MeshProxy,
   KX_PolyProxy, and KX_VertexProxy are only a representation of the physics shape as it was
   when it was converted in BL_DataConversion.
   Previously this kind of meshes were used both for render and physics, but since 0.3+,
   it is only useful in limited cases. In most cases, bpy API should be used instead.

   Note:
   The physics simulation doesn't currently update KX_Mesh/Poly/VertexProxy.

   #. Mesh Objects are converted from Blender at scene load.
   #. The Converter groups polygons by Material. A material holds:

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
      :return: the number of vertices in the vertex array.
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

