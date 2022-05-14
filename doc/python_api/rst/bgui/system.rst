system
======

.. class:: bgui.system.System(textlib, theme=None)

   Bases: :class:`~bgui.widget.Widget`

   The main gui system. Add widgets to this and then call the render() method draw the gui.

   :arg theme: The path to a theme directory

   .. attribute:: normalize_text= True

   .. attribute:: focused_widget

      The widget which currently has “focus”

   .. method:: update_mouse(pos, click_state=0)

      Updates the system’s mouse data

      :arg key: The key being input
      :arg is_shifted: Is the shift key held down?

      :return type: None.

   .. method:: render()

      Renders the GUI system

      :return type: None.

