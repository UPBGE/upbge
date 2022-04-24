KX_CollisionContactPoint(EXP_Value)
===================================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.EXP_Value`

.. class:: KX_CollisionContactPoint

   A collision contact point passed to the collision callbacks.

   .. code-block:: python

      import bge

      def oncollision(object, point, normal, points):
          print("Hit by", object)
          for point in points:
              print(point.localPointA)
              print(point.localPointB)
              print(point.worldPoint)
              print(point.normal)
              print(point.combinedFriction)
              print(point.combinedRestitution)
              print(point.appliedImpulse)

      cont = bge.logic.getCurrentController()
      own = cont.owner
      own.collisionCallbacks = [oncollision]

   .. attribute:: localPointA

      The contact point in the owner object space.

      :type: :class:`mathutils.Vector`

   .. attribute:: localPointB

      The contact point in the collider object space.

      :type: :class:`mathutils.Vector`

   .. attribute:: worldPoint

      The contact point in world space.

      :type: :class:`mathutils.Vector`

   .. attribute:: normal

      The contact normal in owner object space.

      :type: :class:`mathutils.Vector`

   .. attribute:: combinedFriction

      The combined friction of the owner and collider object.

      :type: float

   .. attribute:: combinedRollingFriction

      The combined rolling friction of the owner and collider object.

      :type: float

   .. attribute:: combinedRestitution

      The combined restitution of the owner and collider object.

      :type: float

   .. attribute:: appliedImpulse

      The applied impulse to the owner object.

      :type: float


