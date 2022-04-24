SCA_SceneActuator(SCA_IActuator)
================================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.SCA_IActuator`

.. class:: SCA_SceneActuator

   Scene Actuator logic brick.

   .. warning::

      Scene actuators that use a scene name will be ignored if at game start, the named scene doesn't exist or is empty

      This will generate a warning in the console:

      .. code-block:: none

         Error: GameObject 'Name' has a SceneActuator 'ActuatorName' (SetScene) without scene

   .. attribute:: scene

      the name of the scene to change to/overlay/underlay/remove/suspend/resume.

      :type: string

   .. attribute:: camera

      the camera to change to.

      :type: :class:`~bge.types.KX_Camera` on read, string or :class:`~bge.types.KX_Camera` on write

      .. note::

         When setting the attribute, you can use either a :class:`~bge.types.KX_Camera` or the name of the camera.

   .. attribute:: useRestart

      Set flag to True to restart the sene.

      :type: boolean

   .. attribute:: mode

      The mode of the actuator.

      :type: integer from 0 to 5.
