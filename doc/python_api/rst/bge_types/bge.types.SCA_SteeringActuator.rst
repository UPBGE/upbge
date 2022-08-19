SCA_SteeringActuator(SCA_IActuator)
===================================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.SCA_IActuator`

.. class:: SCA_SteeringActuator

   Steering Actuator for navigation.

   .. attribute:: behavior

      The steering behavior to use. One of :ref:`these constants <logic-steering-actuator>`.

      :type: integer

   .. attribute:: velocity

      Velocity magnitude

      :type: float

   .. attribute:: acceleration

      Max acceleration

      :type: float

   .. attribute:: turnspeed

      Max turn speed

      :type: float

   .. attribute:: distance

      Relax distance

      :type: float

   .. attribute:: target

      Target object

      :type: :class:`~bge.types.KX_GameObject`

   .. attribute:: navmesh

      Navigation mesh

      :type: :class:`~bge.types.KX_GameObject`

   .. attribute:: selfterminated

      Terminate when target is reached

      :type: boolean

   .. attribute:: enableVisualization

      Enable debug visualization

      :type: boolean

   .. attribute:: pathUpdatePeriod

      Path update period

      :type: int

   .. attribute:: path

      Path point list.

      :type: list of :class:`mathutils.Vector`
