KX_VertexProxy(SCA_IObject)
===========================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.SCA_IObject`

.. class:: KX_VertexProxy

   A vertex holds position, UV, color and normal information.
   You can only read the vertex properties of a mesh object. In upbge 0.3+, KX_MeshProxy,
   KX_PolyProxy, and KX_VertexProxy are only a representation of the physics shape as it was
   when it was converted in BL_DataConversion.
   Previously this kind of meshes were used both for render and physics, but since 0.3+,
   it is only useful in limited cases. In most cases, bpy API should be used instead.

   Note:
   The physics simulation doesn't currently update KX_Mesh/Poly/VertexProxy.

   .. attribute:: XYZ

      The position of the vertex.

      :type: Vector((x, y, z))

   .. attribute:: UV

      The texture coordinates of the vertex.

      :type: Vector((u, v))

   .. attribute:: uvs

      The texture coordinates list of the vertex.

      :type: list of Vector((u, v))

   .. attribute:: normal

      The normal of the vertex.

      :type: Vector((nx, ny, nz))

   .. attribute:: color

      The color of the vertex.

      :type: Vector((r, g, b, a))

      Black = [0.0, 0.0, 0.0, 1.0], White = [1.0, 1.0, 1.0, 1.0]

   .. attribute:: colors

      The color list of the vertex.

      :type: list of Vector((r, g, b, a))

   .. attribute:: x

      The x coordinate of the vertex.

      :type: float

   .. attribute:: y

      The y coordinate of the vertex.

      :type: float

   .. attribute:: z

      The z coordinate of the vertex.

      :type: float

   .. attribute:: u

      The u texture coordinate of the vertex.

      :type: float

   .. attribute:: v

      The v texture coordinate of the vertex.

      :type: float

   .. attribute:: u2

      The second u texture coordinate of the vertex.

      :type: float

   .. attribute:: v2

      The second v texture coordinate of the vertex.

      :type: float

   .. attribute:: r

      The red component of the vertex color. 0.0 <= r <= 1.0.

      :type: float

   .. attribute:: g

      The green component of the vertex color. 0.0 <= g <= 1.0.

      :type: float

   .. attribute:: b

      The blue component of the vertex color. 0.0 <= b <= 1.0.

      :type: float

   .. attribute:: a

      The alpha component of the vertex color. 0.0 <= a <= 1.0.

      :type: float

   .. method:: getXYZ()

      Gets the position of this vertex.

      :return: this vertexes position in local coordinates.
      :rtype: Vector((x, y, z))

   .. method:: getUV()

      Gets the UV (texture) coordinates of this vertex.

      :return: this vertexes UV (texture) coordinates.
      :rtype: Vector((u, v))

   .. method:: getUV2()

      Gets the 2nd UV (texture) coordinates of this vertex.

      :return: this vertexes UV (texture) coordinates.
      :rtype: Vector((u, v))

   .. method:: getRGBA()

      Gets the color of this vertex.

      The color is represented as four bytes packed into an integer value.  The color is
      packed as RGBA.

      Since Python offers no way to get each byte without shifting, you must use the struct module to
      access color in an machine independent way.

      Because of this, it is suggested you use the r, g, b and a attributes or the color attribute instead.

      .. code-block:: python

         import struct;
         col = struct.unpack('4B', struct.pack('I', v.getRGBA()))
         # col = (r, g, b, a)
         # black = (  0, 0, 0, 255)
         # white = (255, 255, 255, 255)

      :return: packed color. 4 byte integer with one byte per color channel in RGBA format.
      :rtype: integer

   .. method:: getNormal()

      Gets the normal vector of this vertex.

      :return: normalized normal vector.
      :rtype: Vector((nx, ny, nz))
