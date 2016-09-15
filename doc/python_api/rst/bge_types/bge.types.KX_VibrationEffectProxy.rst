KX_VibrationEffectProxy(CValue)
===============================

.. module:: bge.types

base class --- :class:`CValue`

.. class:: KX_VibrationEffectProxy(CValue)

   Python API for complex vibration effects (based on SDL 2.0.4). Note:
   With SDL, attributes related to durations are set in milliseconds.

   .. attribute:: type

      Effect type.

     SDL_HAPTIC_CONSTANT     (1 << 0)
     brief Constant effect supported.
     Constant haptic effect.

     SDL_HAPTIC_SINE         (1 << 1)
     brief Sine wave effect supported.

     SDL_HAPTIC_LEFTRIGHT    (1 << 2)
     brief Left/Right effect supported.
     Haptic effect for direct control over high/low frequency motors.
 
     SDL_HAPTIC_TRIANGLE     (1 << 3)
     brief Triangle wave effect supported.
     Periodic haptic effect that simulates triangular waves.

     SDL_HAPTIC_SAWTOOTHUP   (1 << 4)
     brief Sawtoothup wave effect supported.
     Periodic haptic effect that simulates saw tooth up waves.

     SDL_HAPTIC_SAWTOOTHDOWN (1 << 5)
     brief Sawtoothdown wave effect supported.
     Periodic haptic effect that simulates saw tooth down waves.

      :type: bitfield

   .. attribute:: conditionType

      Effect condition type.

     SDL_HAPTIC_RAMP       (1 << 6)
     brief Ramp effect supported.
     Ramp haptic effect.

     SDL_HAPTIC_SPRING     (1 << 7)
     brief Spring effect supported - uses axes position.
     Condition haptic effect that simulates a spring.  Effect is based on the
     axes position.

     SDL_HAPTIC_DAMPER     (1 << 8)
     brief Damper effect supported - uses axes velocity.
     Condition haptic effect that simulates dampening.  Effect is based on the
     axes velocity.

     SDL_HAPTIC_INERTIA    (1 << 9)
     brief Inertia effect supported - uses axes acceleration.
     Condition haptic effect that simulates inertia.  Effect is based on the axes
     acceleration.

     SDL_HAPTIC_FRICTION   (1 << 10)
     brief Friction effect supported - uses axes movement.
     Condition haptic effect that simulates friction.  Effect is based on the
     axes movement.

      :type: bitfield

   .. attribute:: periodicDirectionType

     SDL_HAPTIC_POLAR      0
     brief Uses polar coordinates for the direction.

     SDL_HAPTIC_CARTESIAN  1
     brief Uses cartesian coordinates for the direction.

     SDL_HAPTIC_SPHERICAL  2
     brief Uses spherical coordinates for the direction.

      :type: integer

   .. attribute:: periodicDirection0

      Left/right vibration direction.

      :type: integer

   .. attribute:: periodicDirection1

      Up/down vibration direction.

      :type: integer

   .. attribute:: periodicMagnitude

      Vibration strength.

      :type: integer (0 to 32767)

   .. attribute:: periodicLength

      Effect duration.

      :type: integer

   .. attribute:: periodicAttackLength

      Duration of the vibration effect attack.

      :type: integer

   .. attribute:: periodicFadeLength

      Duration of the vibration effect ending.

      :type: integer

