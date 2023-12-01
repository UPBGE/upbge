SCA_ISensor(SCA_ILogicBrick)
============================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.SCA_ILogicBrick`

.. class:: SCA_ISensor

   Base class for all sensor logic bricks.

   .. attribute:: usePosPulseMode

      Flag to turn positive pulse mode on and off.

      :type: boolean

   .. attribute:: useNegPulseMode

      Flag to turn negative pulse mode on and off.

      :type: boolean

   .. attribute:: frequency

      The frequency for pulse mode sensors.

      :type: integer

      .. deprecated:: 0.0.1

         Use :attr:`skippedTicks`

   .. attribute:: skippedTicks

      Number of logic ticks skipped between 2 active pulses

      :type: integer

   .. attribute:: level

      level Option whether to detect level or edge transition when entering a state.
      It makes a difference only in case of logic state transition (state actuator).
      A level detector will immediately generate a pulse, negative or positive
      depending on the sensor condition, as soon as the state is activated.
      A edge detector will wait for a state change before generating a pulse.
      note: mutually exclusive with :attr:`tap`, enabling will disable :attr:`tap`.

      :type: boolean

   .. attribute:: tap

      When enabled only sensors that are just activated will send a positive event,
      after this they will be detected as negative by the controllers.
      This will make a key that's held act as if its only tapped for an instant.
      note: mutually exclusive with :attr:`level`, enabling will disable :attr:`level`.

      :type: boolean

   .. attribute:: invert

      Flag to set if this sensor activates on positive or negative events.

      :type: boolean

   .. attribute:: triggered

      True if this sensor brick is in a positive state. (read-only).

      :type: boolean

   .. attribute:: positive

      True if this sensor brick is in a positive state. (read-only).

      :type: boolean

   .. attribute:: pos_ticks

      The number of ticks since the last positive pulse (read-only).

      :type: int

   .. attribute:: neg_ticks

      The number of ticks since the last negative pulse (read-only).

      :type: int

   .. attribute:: status

      The status of the sensor (read-only): can be one of :ref:`these constants<sensor-status>`.

      :type: int

      .. note::

         This convenient attribute combines the values of triggered and positive attributes.

   .. method:: reset()

      Reset sensor internal state, effect depends on the type of sensor and settings.

      The sensor is put in its initial state as if it was just activated.
