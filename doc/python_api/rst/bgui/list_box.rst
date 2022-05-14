list_box
========

ListBoxes make use of a ListBoxRenderer. The default ListBoxRenderer simply displays an item’s string representation.
To make your own ListBoxRenderer create a class that has a render_item() method that accepts the item to be rendered and returns a widget to render.

Here is an simple example of using the ListBox widget:

.. code-block:: python

   class MySys(bgui.System):
      def lb_click(self, lb):
         print(lb.selected)

      def __init__(self):
         bgui.System.__init__(self)

         items = ["One", "Two", 4, 4.6]
         self.frame = bgui.Frame(self, 'window', border=2, size=[0.5, 0.5], options=bgui.BGUI_DEFAULT|bgui.BGUI_CENTERED)
         self.lb = bgui.ListBox(self.frame, "lb", items=items, padding=0.05, size=[0.9, 0.9], pos=[0.05, 0.05])
         self.lb.on_click = self.lb_click

         # ... rest of __init__


.. class:: bgui.list_box.ListBoxRenderer(listbox)

   Bases: :class:`~object`

   Base class for rendering an item in a ListBox

   :arg listbox: The listbox the renderer will be used with (used for parenting)

   .. method:: render_item(item)

      Creates and returns a :class:`bgui.label.Label` representation of the supplied item.

      :arg item: The item to be rendered
      :return type: `bgui.label.Label`

.. class:: bgui.list_box.ListBox(parent, name=None, items=[], padding=0, aspect=None, size=[1, 1], pos=[0, 0], sub_theme='', options=0)

   Bases: :class:`~bgui.widget.Widget`

   Widget for displaying a list of data

   :arg parent: The widget’s parent
   :arg name: The name of the widget
   :arg items: The items to fill the list with (can also be changed via ListBox.items)
   :arg padding: The amount of extra spacing to put between items (can also be changed via ListBox.padding)
   :arg aspect: Constrain the widget size to a specified aspect ratio
   :arg size: A tuple containing the width and height
   :arg pos: A tuple containing the x and y position
   :arg sub_theme: Name of a sub_theme defined in the theme file (similar to CSS classes)
   :arg options: Various other options


   .. attribute:: theme_section= 'ListBox'

      The UV texture coordinates to use for the image.

   .. attribute:: theme_options= {'Padding': 0, 'HighlightColor4': (0, 0, 1, 1), 'HighlightColor3': (0, 0, 1, 1), 'HighlightColor1': (1, 1, 1, 1), 'Border': 1, 'HighlightColor2': (0, 0, 1, 1)}

      The color of the plane the texture is on.

   .. attribute:: padding= None

      The amount of extra spacing to put between items

   .. attribute:: renderer= None

      The ListBoxRenderer to use to display items

   .. attribute:: items

      The list of items to display in the ListBox


