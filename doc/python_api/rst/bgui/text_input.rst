text_input
==========

This module defines the following constants:

InputText options:
* BGUI_INPUT_NONE = 0
* BGUI_INPUT_SELECT_ALL = 1
* BGUI_INPUT_DEFAULT = BGUI_INPUT_NONE

.. class:: bgui.text_input.TextInput(parent, name=None, text='', prefix='', font=None, pt_size=None, color=None, aspect=None, size=[1, 1], pos=[0, 0], sub_theme='', input_options=0, options=0)

   Bases: :class:`~bgui.widget.Widget`

   Widget for getting text input

   :arg parent: The widget’s parent
   :arg name: The name of the widget
   :arg text: The text to display (this can be changed later via the text property)
   :arg prefix: Prefix text displayed before user input, cannot be edited by user (this can be changed later via the prefix property)
   :arg font: The font to use
   :arg pt_size: The point size of the text to draw
   :arg color: Color of the font for this widget
   :arg aspect: Constrain the widget size to a specified aspect ratio
   :arg size: A tuple containing the width and height
   :arg pos: A tuple containing the x and y position
   :arg sub_theme: Name of a sub_theme defined in the theme file (similar to CSS classes)
   :arg options: Various other options


   .. attribute:: theme_section= 'TextInput'

   .. attribute:: theme_options= {'InactiveBorderColor': (0, 0, 0, 0), 'HighlightColor': (0.6, 0.6, 0.6, 0.5), 'TextColor': (1, 1, 1, 1), 'FrameColor': (0, 0, 0, 0), 'BorderColor': (0, 0, 0, 0), 'LabelSubTheme': '', 'InactiveHighlightColor': (0.6, 0.6, 0.6, 0.5), 'BorderSize': 0, 'InactiveBorderSize': 0, 'InactiveFrameColor': (0, 0, 0, 0), 'InactiveTextColor': (1, 1, 1, 1)}

   .. attribute:: text

   .. attribute:: prefix

   .. attribute:: on_enter_key

      A callback for when the enter key is pressed while the TextInput has focus

   .. method:: select_all()

      Change the selection to include all of the text.

   .. method:: select_none()

      Change the selection to include none of the text.

   .. method:: activate()

   .. method:: deactivate()

   .. method:: swapcolors(state=0)

   .. method:: update_selection()

   .. method:: find_mouse_slice(pos)

   .. method:: calc_mouse_cursor(pos)

