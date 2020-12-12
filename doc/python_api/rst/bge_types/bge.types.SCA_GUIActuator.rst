SCA_GUIActuator(SCA_IActuator)
==============================

base class --- :class:`SCA_IActuator`

.. class:: SCA_GUIActuator(SCA_IActuator)

   The GUI actuator loads a new .blend file, restarts the current .blend file or quits the game.

   .. attribute:: cursorName

      the name of cursor.

      :type: string

   .. attribute:: layoutName

      the name of layout to use.

      :type: string

   .. attribute:: prefix

      the prefix to use.

      :type: string

   .. attribute:: changeDefault

      the default cursor.

      :type: bool

   .. attribute:: mode

      The mode of this actuator. Can be on of :ref:`these constants <gui-actuator>`

      :type: Int
