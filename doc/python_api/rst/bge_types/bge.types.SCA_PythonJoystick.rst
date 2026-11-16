SCA_PythonJoystick(EXP_PyObjectPlus)
====================================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.EXP_PyObjectPlus`

.. class:: SCA_PythonJoystick

   A Python interface to a joystick.

   SDL recognizes most controllers as gamepads and reports them in a fixed layout: 6 axes (left
   stick, right stick, triggers) and 26 buttons. Other devices, such as flight sticks, throttles,
   wheels, pedals and force-feedback joysticks, open as raw joysticks: :attr:`isGamepad` is
   False, and :attr:`axisValues`, :attr:`activeButtons` and :attr:`hatValues` report the axes,
   buttons and hats of the device by index. A mapping for the device's :attr:`guid` in
   ``gamecontrollerdb.txt`` makes it a gamepad instead.

   On a raw joystick, joystick sensors (logic bricks) see axes 0 to 5 and buttons 0 to 25 as if
   they were the gamepad layout, and so do :attr:`leftStick`, :attr:`rightStick` and
   :attr:`triggers`, whatever the device uses those axes for. The other axes, buttons and hats
   are available from Python only.

   Each connection of a controller gets its own object, and the settings a script applies last as
   long as that connection: :attr:`strengthLeft`, :attr:`strengthRight`, :attr:`duration`,
   :attr:`stickDeadzone`, :attr:`stickOuter`, :attr:`stickExponent`, :attr:`triggerDeadzone`,
   the sensors enabled with :meth:`enableSensors`, the color set with :meth:`setLED` and
   :attr:`playerIndex`. When the controller disconnects (for example, its battery runs out), its
   object is replaced, and the controller that connects next, the same one or another, starts
   with the defaults. Read the object from :data:`bge.logic.joysticks` every frame and apply the
   settings again when it changes.

   .. attribute:: name

      The name assigned to the joystick by the operating system. (read-only)

      :type: string

   .. attribute:: isGamepad

      False for raw joysticks: devices SDL does not recognize as gamepads (see above).
      (read-only)

      :type: boolean

   .. attribute:: activeButtons

      The indices of the pressed buttons, from 0 to :attr:`numButtons` - 1. (read-only)

      :type: list of integers

   .. attribute:: axisValues

      The state of the joysticks axis as a list of values :attr:`numAxis` long. (read-only).
      On a raw joystick, which axis is which depends on the device.

      :type: list of floats

      Each specifying the value of an axis between -1.0 and 1.0
      depending on how far the axis is pushed, 0 for nothing.
      The first 2 values are used by most joysticks and gamepads for directional control.
      3rd and 4th values are only on some joysticks and can be used for arbitrary controls.

      * left:[-1.0, 0.0, ...]
      * right:[1.0, 0.0, ...]
      * up:[0.0, -1.0, ...]
      * down:[0.0, 1.0, ...]

   .. attribute:: hatValues

      The hats of a raw joystick, as a list of values :attr:`numHats` long: 0 when centered,
      otherwise 1 (up), 2 (right), 4 (down) or 8 (left), added together on diagonals (3 is up and
      right). (read-only)

      .. deprecated:: 0.2.2

         On gamepads, which report the d-pad as buttons and return an empty list here. Use
         :attr:`activeButtons` instead.

      :type: list of integers

   .. attribute:: numAxis

      The number of axes: 6 on gamepads, the device's own count (up to 16) on raw joysticks.
      (read-only).

      :type: integer

   .. attribute:: numButtons

      The number of buttons: 26 on gamepads, the device's own count (up to 128) on raw joysticks.
      (read-only).

      :type: integer

   .. attribute:: numHats

      The number of hats of a raw joystick (up to 4). (read-only).

      .. deprecated:: 0.2.2

         On gamepads, which have no hats and return 0 here. Use :attr:`numButtons` instead.

      :type: integer

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

   .. attribute:: type

      The controller type identified by SDL: ``"xbox360"``, ``"xboxone"``, ``"ps3"``, ``"ps4"``,
      ``"ps5"``, ``"switchpro"``, ``"joyconleft"``, ``"joyconright"``, ``"joyconpair"``,
      ``"gamecube"``, ``"standard"`` or ``"unknown"``, and ``"joystick"`` for raw joysticks.
      (read-only)

      :type: string

   .. attribute:: buttonLabels

      The labels printed on face buttons 0 to 3 (the indices :attr:`activeButtons` reports), for
      choosing button prompts: ``"A"``, ``"B"``, ``"X"``, ``"Y"``, ``"CROSS"``, ``"CIRCLE"``,
      ``"SQUARE"``, ``"TRIANGLE"`` or ``"UNKNOWN"``. (read-only)

      .. code-block:: python

         # ("A", "B", "X", "Y") on Xbox, ("CROSS", "CIRCLE", "SQUARE", "TRIANGLE") on PlayStation,
         # ("B", "A", "Y", "X") on Switch controllers.
         confirm_label = joystick.buttonLabels[0]

      :type: tuple of 4 strings

   .. attribute:: guid

      The SDL GUID of the controller, the identifier used by ``gamecontrollerdb.txt`` mappings.
      (read-only)

      :type: string (32 hexadecimal digits)

   .. attribute:: connection

      ``"wired"``, ``"wireless"`` or ``"unknown"``. (read-only)

      :type: string

   .. attribute:: playerIndex

      The player number shown by the controller: player LEDs on PS5 and Switch controllers, ring
      quadrant on Xbox 360 controllers. Set to the joystick index (0 is player 1) when the
      controller connects. -1 clears it. Some platforms assign it themselves (XInput on
      Windows) and ignore changes.

      :type: integer (-1 or greater)

   .. method:: enableSensors(gyro=True, accel=False)

      Turns the motion sensors on or off. They are off by default; while off, they cost nothing.

      :arg gyro: Enable the gyroscope.
      :type gyro: boolean
      :arg accel: Enable the accelerometer.
      :type accel: boolean
      :return: False if a requested sensor is missing or could not be switched.
      :rtype: boolean

   .. attribute:: hasGyro

      Whether the controller has a gyroscope. (read-only)

      :type: boolean

   .. attribute:: hasAccel

      Whether the controller has an accelerometer. (read-only)

      :type: boolean

   .. attribute:: gyro

      Latest angular velocity, in radians per second, as (pitch, yaw, roll): rotation around the
      x (right), y (up) and z (towards the player) axes of the controller held in front of you,
      counter-clockwise positive. These match the local axes of a camera. All zero until
      :meth:`enableSensors` turns the gyroscope on. (read-only)

      :type: tuple of 3 floats

   .. attribute:: gyroDelta

      Rotation, in radians, accumulated from every gyroscope sample received since the previous
      logic tick, in the same axes as :attr:`gyro`. Controllers report 200 to 1000 samples per
      second, several per tick; this sums all of them, so it is the rotation to apply this tick
      whatever the logic rate. (read-only)

      .. code-block:: python

         pitch, yaw, roll = joystick.gyroDelta
         camera.applyRotation((pitch * sensitivity, yaw * sensitivity, 0.0), True)

      :type: tuple of 3 floats

   .. attribute:: accel

      Latest acceleration, in meters per second squared, gravity included (a controller at rest
      reads about 9.81 along the axis pointing up). (read-only)

      :type: tuple of 3 floats

   .. attribute:: touchpads

      The fingers on each touchpad (DualShock 4 and DualSense have one, the Steam Deck two), as
      ``(down, x, y, pressure)`` tuples; x and y go from 0.0 to 1.0 from the top-left corner. The
      touchpad click is reported as button 20. (read-only)

      :type: list of lists of (boolean, float, float, float) tuples

   .. attribute:: battery

      ``(state, percent)``: state is ``"on_battery"``, ``"no_battery"``, ``"charging"``,
      ``"charged"``, ``"unknown"`` or ``"error"``; percent is the charge level, or -1 when the
      controller does not report it. (read-only)

      :type: tuple of (string, integer)

   .. attribute:: hasLED

      Whether the controller has a light bar or LED that :meth:`setLED` can set. (read-only)

      :type: boolean

   .. method:: setLED(red, green, blue)

      Sets the light bar color. On controllers with a single-color LED, the brightest channel sets
      the brightness. Call it when the color changes, not every frame.

      :arg red: Red channel, 0.0 to 1.0.
      :type red: float
      :arg green: Green channel, 0.0 to 1.0.
      :type green: float
      :arg blue: Blue channel, 0.0 to 1.0.
      :type blue: float
      :return: False if the controller has no LED.
      :rtype: boolean

   .. attribute:: hasTriggerVibration

      Whether the controller has trigger motors (Xbox One and Series controllers). (read-only)

      :type: boolean

   .. method:: startTriggerVibration(left, right, duration)

      Vibrates the trigger motors. The effect stops by itself after ``duration`` milliseconds.

      :arg left: Left trigger strength, 0.0 to 1.0.
      :type left: float
      :arg right: Right trigger strength, 0.0 to 1.0.
      :type right: float
      :arg duration: Duration in milliseconds.
      :type duration: integer
      :return: False if the controller has no trigger motors.
      :rtype: boolean

   .. method:: stopTriggerVibration()

      Stops the trigger motors.

      :return: False if the controller has no trigger motors.
      :rtype: boolean

   .. method:: sendEffect(data)

      Sends a raw, controller-specific effect packet, for example DualSense adaptive-trigger
      settings. The packet layout is defined per controller by SDL (see ``SDL_SendGamepadEffect``).

      :arg data: The packet.
      :type data: bytes-like object
      :return: False if the controller does not accept effect packets.
      :rtype: boolean
