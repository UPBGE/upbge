KX_FontObject(KX_GameObject)
============================

base class --- :class:`KX_GameObject`

.. class:: KX_FontObject(KX_GameObject)

   A Font object.

   .. code-block:: python

      # Display a message about the exit key using a Font object.
      import bge

      co = bge.logic.getCurrentController()
      font = co.owner

      exit_key = bge.events.EventToString(bge.logic.getExitKey())

      if exit_key.endswith("KEY"):
          exit_key = exit_key[:-3]

      font.text = "Press key '%s' to quit the game." % exit_key

   .. attribute:: text

      The text displayed by this Font object.

      :type: string
   .. attribute:: resolution

      The resolution of the font police.

      .. warning::

         High resolution can use a lot of memory and may crash.

      :type: float (0.1 to 50.0)

   .. attribute:: size

      The size (scale factor) of the font object, scaled from font object origin (affects text resolution).

      .. warning::

         High size can use a lot of memory and may crash.

      :type: float (0.0001 to 40.0)

   .. attribute:: dimensions

      The size (width and height) of the current text in Blender Units.

      :type: :class:`mathutils.Vector`

