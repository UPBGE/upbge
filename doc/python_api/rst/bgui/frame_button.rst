frame_button
============

.. class:: bgui.frame_button.FrameButton(parent, name=None, base_color=None, text='', font=None, pt_size=None, aspect=None, size=[1, 1], pos=[0, 0], sub_theme='', options=0)

   Bases: :class:`~bgui.widget.Widget`

   A clickable frame-based button.

   :arg parent: The widget’s parent
   :arg name: The name of the widget
   :arg base_color: The color of the button
   :arg text: The text to display (this can be changed later via the text property)
   :arg font: The font to use
   :arg pt_size: The point size of the text to draw (defaults to 30 if None)
   :arg aspect: Constrain the widget size to a specified aspect ratio
   :arg size: A tuple containing the width and height
   :arg pos: A tuple containing the x and y position
   :arg sub_theme: Name of a sub_theme defined in the theme file (similar to CSS classes)
   :arg options: Various other options


   .. attribute:: theme_section= 'FrameButton'

   .. attribute:: theme_options= {'Color': (0.4, 0.4, 0.4, 1), 'BorderSize': 1, 'BorderColor': (0, 0, 0, 1), 'LabelSubTheme': ''}

   .. attribute:: text

   .. attribute:: color
