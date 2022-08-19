theme
=====

.. class:: bgui.theme.NewSectionProxy(parser, name)

   Bases: :class:`configparser.SectionProxy`

   Creates a view on a section of the specified name in parser.

.. class:: bgui.theme.Theme(file)

   Bases: :class:`configparser.ConfigParser`

   Creates a view on a section of the specified name in parser.
   :arg path= '':

   .. method:: supports(widget)

      Checks to see if the theme supports a given widget.

      :arg widget: The widget to check for support

   .. method:: warn_legacy(section)

   .. method:: warn_support(section)

