SCA_LedActuator(SCA_IActuator)
==============================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.SCA_IActuator`

.. class:: SCA_LedActuator

   LED Actuator: sets the light bar or LED of a controller (DualShock 4, DualSense, Switch Pro and
   others) when activated, as :meth:`~bge.types.SCA_PythonJoystick.setLED` does. On controllers
   with a single-color LED, the brightest channel sets the brightness. Controllers without an
   LED ignore it.

   Each activation is at most one write to the controller, and none when the color does not
   change.

   .. attribute:: joyindex

      Joystick index.

      :type: integer (0 to 7)

   .. attribute:: color

      Color of the light bar or LED, as red, green and blue from 0.0 to 1.0.

      :type: list of 3 floats
