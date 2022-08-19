label
=====

.. class:: bgui.label.Label(parent, name=None, text='', font=None, pt_size=None, color=None, outline_color=None, outline_size=None, outline_smoothing=None, pos=[0, 0], sub_theme='', options=0)

   Bases: :class:`~bgui.widget.Widget`

   Widget for displaying images

   :arg parent: The widget’s parent
   :arg name: The name of the widget
   :arg text: The text to display (this can be changed later via the text property)
   :arg font: The font to use
   :arg pt_size: The point size of the text to draw (defaults to 30 if None)
   :arg color: The color to use when rendering the font
   :arg pos: A tuple containing the x and y position
   :arg sub_theme: Name of a sub_theme defined in the theme file (similar to CSS classes)
   :arg options: Various other options


   .. attribute:: theme_section= 'Label'

   .. attribute:: theme_options= {'Size': 30, 'Font': '', 'OutlineSize': 0, 'OutlineSmoothing': False, 'Color': (1, 1, 1, 1), 'OutlineColor': (0, 0, 0, 1)}

   .. attribute:: text

      The text to display

   .. attribute:: pt_size

      The point size of the label’s font

