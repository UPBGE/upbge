text_block
==========

.. class:: bgui.text_block.TextBlock(parent, name=None, text='', font=None, pt_size=None, color=None, aspect=None, size=[1, 1], pos=[0, 0], sub_theme='', overflow=1, options=0)

   Bases: :class:`~bgui.widget.Widget`

   Widget for displaying blocks of text

   :arg parent: The widget’s parent
   :arg name: The name of the widget
   :arg text: The text to display (this can be changed later via the text property)
   :arg font: The font to use
   :arg pt_size: The point size of the text to draw
   :arg color: The color to use when rendering the font
   :arg aspect: Constrain the widget size to a specified aspect ratio
   :arg size: A tuple containing the width and height
   :arg pos: A tuple containing the x and y position
   :arg sub_theme: Name of a sub_theme defined in the theme file (similar to CSS classes)
   :arg overflow: How to handle excess text
   :arg options: Various other options


   .. attribute:: theme_section= 'TextBlock'

   .. attribute:: theme_options= {'LabelSubTheme': ''}

   .. attribute:: text

      The text to display

