SCA_PythonKeyboard(EXP_PyObjectPlus)
====================================

base class --- :class:`EXP_PyObjectPlus`

.. class:: SCA_PythonKeyboard(EXP_PyObjectPlus)

   The current keyboard.

   .. attribute:: inputs

      A dictionary containing the input of each keyboard key. (read-only).

      :type: dictionary {:ref:`keycode<keyboard-keys>`::class:`SCA_InputEvent`, ...}

   .. attribute:: events

      A dictionary containing the status of each keyboard event or key. (read-only).

      .. deprecated:: use :data:`inputs`

      :type: dictionary {:ref:`keycode<keyboard-keys>`::ref:`status<input-status>`, ...}

   .. attribute:: activeInputs

      A dictionary containing the input of only the active keyboard keys. (read-only).

      :type: dictionary {:ref:`keycode<keyboard-keys>`::class:`SCA_InputEvent`, ...}

   .. attribute:: active_events

      A dictionary containing the status of only the active keyboard events or keys. (read-only).

      .. deprecated:: use :data:`activeInputs`

      :type: dictionary {:ref:`keycode<keyboard-keys>`::ref:`status<input-status>`, ...}

   .. attribute:: text

      The typed unicode text from the last frame.

      :type: string

   .. method:: getClipboard()

      Gets the clipboard text.

      :rtype: string

   .. method:: setClipboard(text)

      Sets the clipboard text.

      :arg text: New clipboard text
      :type text: string
