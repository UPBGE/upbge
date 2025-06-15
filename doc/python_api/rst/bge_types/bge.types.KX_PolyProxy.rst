KX_PolyProxy(SCA_IObject)
=========================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.SCA_IObject`

.. class:: KX_PolyProxy

   A polygon holds the index of the vertex forming the poylgon.
   You can only read the vertex properties of a mesh object. In upbge 0.3+, KX_MeshProxy,
   KX_PolyProxy, and KX_VertexProxy are only a representation of the physics shape as it was
   when it was converted in BL_DataConversion.
   Previously this kind of meshes were used both for render and physics, but since 0.3+,
   it is only useful in limited cases. In most cases, bpy API should be used instead.

   Note:
   The physics simulation doesn't currently update KX_Mesh/Poly/VertexProxy.

   .. attribute:: material_name

      The name of polygon material, empty if no material.

      :type: string

   .. attribute:: material

      The material of the polygon.

      :type: :class:`~bge.types.KX_BlenderMaterial`

   .. attribute:: texture_name

      The texture name of the polygon.

      :type: string

   .. attribute:: material_id

      The material index of the polygon, use this to retrieve vertex proxy from mesh proxy.

      :type: integer

   .. attribute:: v1

      vertex index of the first vertex of the polygon, use this to retrieve vertex proxy from mesh proxy.

      :type: integer

   .. attribute:: v2

      vertex index of the second vertex of the polygon, use this to retrieve vertex proxy from mesh proxy.

      :type: integer

   .. attribute:: v3

      vertex index of the third vertex of the polygon, use this to retrieve vertex proxy from mesh proxy.

      :type: integer

   .. attribute:: v4

      Vertex index of the fourth vertex of the polygon, 0 if polygon has only 3 vertex
      Use this to retrieve vertex proxy from mesh proxy.

      :type: integer

   .. attribute:: visible

      visible state of the polygon: 1=visible, 0=invisible.

      :type: integer

   .. attribute:: collide

      collide state of the polygon: 1=receives collision, 0=collision free.

      :type: integer

   .. attribute:: vertices

      Returns the list of vertices of this polygon.

      :type: :class:`~bge.types.KX_VertexProxy` list (read only)

   .. method:: getMaterialName()

      Returns the polygon material name with MA prefix

      :return: material name
      :rtype: string

   .. method:: getMaterial()

      :return: The polygon material
      :rtype: :class:`~bge.types.KX_BlenderMaterial`

   .. method:: getTextureName()

      :return: The texture name
      :rtype: string

   .. method:: getMaterialIndex()

      Returns the material bucket index of the polygon.
      This index and the ones returned by getVertexIndex() are needed to retrieve the vertex proxy from :class:`~bge.types.KX_MeshProxy`.

      :return: the material index in the mesh
      :rtype: integer

   .. method:: getNumVertex()

      Returns the number of vertex of the polygon.

      :return: number of vertex, 3 or 4.
      :rtype: integer

   .. method:: isVisible()

      Returns whether the polygon is visible or not

      :return: 0=invisible, 1=visible
      :rtype: boolean

   .. method:: isCollider()

      Returns whether the polygon is receives collision or not

      :return: 0=collision free, 1=receives collision
      :rtype: integer

   .. method:: getVertexIndex(vertex)

      Returns the mesh vertex index of a polygon vertex
      This index and the one returned by getMaterialIndex() are needed to retrieve the vertex proxy from :class:`~bge.types.KX_MeshProxy`.

      :arg vertex: index of the vertex in the polygon: 0->3
      :arg vertex: integer
      :return: mesh vertex index
      :rtype: integer

   .. method:: getMesh()

      Returns a mesh proxy

      :return: mesh proxy
      :rtype: :class:`~bge.types.KX_MeshProxy`
