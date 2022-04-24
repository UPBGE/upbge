SCA_CollisionSensor(SCA_ISensor)
================================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.SCA_ISensor`

.. class:: SCA_CollisionSensor

   Collision sensor detects collisions between objects.

   .. attribute:: propName

      The property or material to collide with.

      :type: string

   .. attribute:: useMaterial

      Determines if the sensor is looking for a property or material. KX_True = Find material; KX_False = Find property.

      :type: boolean

   .. attribute:: usePulseCollision

      When enabled, changes to the set of colliding objects generate a pulse.

      :type: boolean

   .. attribute:: hitObject

      The last collided object. (read-only).

      :type: :class:`~bge.types.KX_GameObject` or None

   .. attribute:: hitObjectList

      A list of colliding objects. (read-only).

      :type: :class:`~bge.types.EXP_ListValue` of :class:`~bge.types.KX_GameObject`

   .. attribute:: hitMaterial

      The material of the object in the face hit by the ray. (read-only).

      :type: string
