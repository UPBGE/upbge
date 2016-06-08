SCA_PythonKeyboard(PyObjectPlus)
================================

.. module:: bge.types

base class --- :class:`PyObjectPlus`

.. class:: SCA_PythonKeyboard(PyObjectPlus)

   The current keyboard.

   .. attribute:: events

      A dictionary containing the events of each keyboard key. (read-only).

      :type: dictionary {:ref:`keycode<keyboard-keys>`::class:`SCA_InputEvent`, ...}

   .. attribute:: active_events

      A dictionary containing the event of only the active keyboard keys. (read-only).

      :type: dictionary {:ref:`keycode<keyboard-keys>`::class:`SCA_InputEvent`, ...}

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

