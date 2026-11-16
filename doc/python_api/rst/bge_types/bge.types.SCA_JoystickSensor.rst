SCA_JoystickSensor(SCA_ISensor)
===============================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.SCA_ISensor`

.. class:: SCA_JoystickSensor

   This sensor detects player joystick events.

   The sensor has two modes, which offer different event types:

   * **Gamepad**, for most games: the standard game controller, in the Xbox 360 layout. The
     controller is read through its mapping, from ``gamecontrollerdb.txt`` or SDL's own, so the
     same sensor works with every known controller. Event types: Stick Directions, Stick Axis,
     Shoulder Triggers, Buttons, Motion and Connection. Face buttons are named by position:
     South (A), East (B), West (X), North (Y).

     A controller without a mapping is opened as a raw joystick (see
     :attr:`~bge.types.SCA_PythonJoystick.isGamepad`), and Gamepad mode falls back to its axes
     0 to 5 and buttons 0 to 25 as if they were the gamepad layout, so players still have
     something to configure in game.
   * **Joystick**, for games built around devices that do not fit the Xbox 360 layout (flight
     sticks, throttles, wheels, pedals): the device is read as it reports itself, by index,
     bypassing any mapping, as the sensor did before the gamepad layout existed. This works with
     gamepads too. Event types: Joystick Button (index from 0), Joystick Axis (number from 1,
     with the sign that triggers it), Hat (number from 1) and Connection.

   Most gamepad drivers report the D-pad as hat 1. Where one does not, hat 1 is built from the
   D-pad buttons, so Hat sensors from BGE 2.7x files work with any gamepad.

   A Hat sensor triggers on the exact direction, as in BGE 2.7x: "Up" does not trigger on
   up-left or up-right. Use All Events to trigger on any direction.

   A Connection sensor is positive while a controller is connected at its index. It gives a
   positive pulse when a controller connects, also when it replaces another one within the same
   logic tick, and a negative pulse when it disconnects. Since script settings last as long as a
   connection (see :class:`~bge.types.SCA_PythonJoystick`), it is the place to apply them
   again.

   A Motion sensor triggers on the rotation speed around the controller's pitch, yaw or roll axis
   (threshold in degrees per second), or on shake: the acceleration departing from 1 g by more
   than the threshold (in g). It uses the fastest sample since the previous logic tick, so a
   flick shorter than a tick still triggers. It turns on the motion sensor it needs; while it
   exists, that sensor stays on even if a script turns it off with
   :meth:`~bge.types.SCA_PythonJoystick.enableSensors`. Controllers without that sensor never
   trigger it.

   .. attribute:: axisValues

      The state of the joysticks axis as a list of values :attr:`numAxis` long. (read-only).
      Like :attr:`numAxis`, :attr:`numButtons`, :attr:`numHats`, :attr:`hatValues`,
      :meth:`getButtonActiveList` and :meth:`getButtonStatus`, it follows the sensor's mode: the
      device's own inputs for Joystick-mode sensors, the gamepad layout otherwise.

      :type: list of ints.

      Each specifying the value of an axis between -32767 and 32767 depending on how far the axis is pushed, 0 for nothing.
      The first 2 values are used by most joysticks and gamepads for directional control. 3rd and 4th values are only on some joysticks and can be used for arbitrary controls.

      * left:[-32767, 0, ...]
      * right:[32767, 0, ...]
      * up:[0, -32767, ...]
      * down:[0, 32767, ...]

   .. attribute:: axisSingle

      like :attr:`axisValues` but returns a single axis value that is set by the sensor. (read-only).

      :type: integer

      .. note::

         Only use this for Stick Axis, Shoulder Triggers and Joystick Axis sensors, otherwise it
         raises an error.

   .. attribute:: hatValues

      The state of the joysticks hats as a list of values :attr:`numHats` long. On a gamepad,
      hat 1 is the D-pad. (read-only).

      :type: list of ints

      Each specifying the direction of the hat from 1 to 12, 0 when inactive.

      Hat directions are as follows...

      * 0:None
      * 1:Up
      * 2:Right
      * 4:Down
      * 8:Left
      * 3:Up - Right
      * 6:Down - Right
      * 12:Down - Left
      * 9:Up - Left

   .. attribute:: hatSingle

      Like :attr:`hatValues` but returns the direction of the hat set by the sensor. (read-only).

      :type: integer

      .. note::

         Only use this for Hat sensors, otherwise it raises an error.

   .. attribute:: numAxis

      The number of axes for the joystick at this index: 6 on a gamepad in Gamepad mode, the
      device's own count (up to 16) in Joystick mode and on a raw joystick. (read-only).

      :type: integer

   .. attribute:: numButtons

      The number of buttons for the joystick at this index: 26 on a gamepad in Gamepad mode, the
      device's own count (up to 128) in Joystick mode and on a raw joystick. (read-only).

      :type: integer

   .. attribute:: numHats

      The number of hats for the joystick at this index: at least 1 (the D-pad) on a gamepad, the
      device's own count (up to 4) on a raw joystick. (read-only).

      :type: integer

   .. attribute:: connected

      True if a joystick is connected at this joysticks index. (read-only).

      :type: boolean

   .. attribute:: index

      The joystick index to use (from 0 to 15). The first joystick is always 0.

      :type: integer

   .. attribute:: threshold

      Axis threshold. Joystick axis motion below this threshold wont trigger an event. Use values between (0 and 32767), lower values are more sensitive.
      For Motion sensors, the rotation speed in degrees per second, or the shake in thousandths
      of g.

      :type: integer

   .. attribute:: button

      The button index the sensor reacts to (first button = 0): 0 to 25 for Buttons sensors, 0
      to 127 for Joystick Button sensors. When the "All Events" toggle is set, this option has
      no effect.

      :type: integer

   .. attribute:: axis

      The axis this sensor reacts to, as a list of two values [axisIndex, axisDirection]

      * axisIndex: the axis index to use when detecting axis movement, 1=primary directional control, 2=secondary directional control.
        For Joystick Axis sensors, the axis number (1 to 16); for Motion sensors, 1=pitch,
        2=yaw, 3=roll, 4=shake.
      * axisDirection: 0=right, 1=up, 2=left, 3=down.
        For Joystick Axis sensors, 0=positive, 1=negative, 2=both.

      :type: [integer, integer]

   .. attribute:: hat

      The hat the sensor reacts to, as a list of two values: [hatIndex, hatDirection]

      * hatIndex: the hat index to use when detecting hat movement, 1=primary hat, 2=secondary hat (4 max).
        On a gamepad, the D-pad is hat 1.
      * hatDirection: 1-12, as in :attr:`hatValues`.

      :type: [integer, integer]

   .. method:: getButtonActiveList()

      :return: A list containing the indicies of the currently pressed buttons.
      :rtype: list

   .. method:: getButtonStatus(buttonIndex)

      :arg buttonIndex: the button index, 0=first button
      :type buttonIndex: integer
      :return: The current pressed state of the specified button.
      :rtype: boolean
