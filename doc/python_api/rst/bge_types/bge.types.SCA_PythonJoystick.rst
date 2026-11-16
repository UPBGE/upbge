SCA_PythonJoystick(EXP_PyObjectPlus)
====================================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.EXP_PyObjectPlus`

.. class:: SCA_PythonJoystick

   A Python interface to a joystick.

   .. attribute:: name

      The name assigned to the joystick by the operating system. (read-only)

      :type: string

   .. attribute:: activeButtons

      A list of active button values. (read-only)

      :type: list

   .. attribute:: axisValues

      The state of the joysticks axis as a list of values :attr:`numAxis` long. (read-only).

      :type: list of ints.

      Each specifying the value of an axis between -1.0 and 1.0
      depending on how far the axis is pushed, 0 for nothing.
      The first 2 values are used by most joysticks and gamepads for directional control.
      3rd and 4th values are only on some joysticks and can be used for arbitrary controls.

      * left:[-1.0, 0.0, ...]
      * right:[1.0, 0.0, ...]
      * up:[0.0, -1.0, ...]
      * down:[0.0, 1.0, ...]

   .. attribute:: hatValues

      .. deprecated:: 0.2.2

         Use :attr:`activeButtons` instead.

   .. attribute:: numAxis

      The number of axes for the joystick at this index. (read-only).

      :type: integer

   .. attribute:: numButtons

      The number of buttons for the joystick at this index. (read-only).

      :type: integer

   .. attribute:: numHats

      .. deprecated:: 0.2.2

         Use :attr:`numButtons` instead.

   .. method:: startVibration()

      Starts the vibration.

      :return: None

   .. method:: stopVibration()

      Stops the vibration.

      :return: None

   .. attribute:: strengthLeft

      Strength of the Low frequency joystick's motor (placed at left position usually).
      Defaults to 0.0.

      :type: float (0.0 to 1.0)

   .. attribute:: strengthRight

      Strength of the High frequency joystick's motor (placed at right position usually).
      Defaults to 0.0.

      :type: float (0.0 to 1.0)

   .. attribute:: duration

      Duration of the vibration in milliseconds. Defaults to 0.

      :type: integer (0 to infinite)

   .. attribute:: isVibrating

      Check status of joystick vibration

      :type: bool (true vibrating and false stopped)

   .. attribute:: hasVibration

      Check if the joystick supports vibration

      :type: bool (true supported and false not supported)

   .. attribute:: leftStick

      Position of the left stick after the radial deadzone and response curve set by
      :attr:`stickDeadzone`, :attr:`stickOuter` and :attr:`stickExponent`. (read-only)

      The length of the vector is 0.0 inside the deadzone and grows from 0.0 at its edge to 1.0 at
      :attr:`stickOuter`; its direction is the direction of the stick. Unlike a per-axis
      deadzone, a near-diagonal push keeps its angle. The y value is positive when the stick is
      pushed down (towards the player).

      .. code-block:: python

         import bge

         obj = bge.logic.getCurrentController().owner
         joystick = bge.logic.joysticks[0]
         if joystick is not None:
             x, y = joystick.leftStick
             obj.applyMovement((x * 0.1, -y * 0.1, 0.0), True)

      :type: tuple of 2 floats (-1.0 to 1.0)

   .. attribute:: rightStick

      Position of the right stick, with the same response as :attr:`leftStick`. (read-only)

      :type: tuple of 2 floats (-1.0 to 1.0)

   .. attribute:: triggers

      Positions of the left and right triggers after :attr:`triggerDeadzone`: 0.0 inside the
      deadzone, growing from 0.0 at its edge to 1.0 when fully pressed. (read-only)

      :type: tuple of 2 floats (0.0 to 1.0)

   .. attribute:: stickDeadzone

      Radius of the stick deadzone used by :attr:`leftStick` and :attr:`rightStick`, as a
      fraction of full deflection. Must be lower than :attr:`stickOuter`. Defaults to 0.12.

      :type: float (0.0 to 1.0)

   .. attribute:: stickOuter

      Stick deflection at which :attr:`leftStick` and :attr:`rightStick` reach 1.0. Values above
      1.0 are not possible, so a value slightly below it lets every controller reach full output
      in every direction. Must be greater than :attr:`stickDeadzone`. Defaults to 0.95.

      :type: float (0.0 to 1.0)

   .. attribute:: stickExponent

      Exponent applied to the stick output length: 1.0 is linear, greater values give finer
      control near the center. Defaults to 1.0.

      :type: float (greater than 0.0, up to 10.0)

   .. attribute:: triggerDeadzone

      Deadzone at the rest end of the triggers, used by :attr:`triggers`. Defaults to 0.05.

      :type: float (0.0 to less than 1.0)

   .. note::

      Each joystick object keeps its own response settings. Values outside the type range are
      clamped; values that break the relations above, or NaN, raise :exc:`ValueError` and leave
      the previous value unchanged.
