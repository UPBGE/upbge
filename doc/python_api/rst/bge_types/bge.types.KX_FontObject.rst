KX_FontObject(KX_GameObject)
============================

.. module:: bge.types

base class --- :class:`KX_GameObject`

.. class:: KX_FontObject(KX_GameObject)

   A Font object.
   
   It is possible to use attributes from :type: :class:`bpy.types.TextCurve`

   .. code-block:: python

      # Display a message about the exit key using a Font object.
      import bge

      font_object = (bge.logic.getCurrentController()).owner

      exit_key = bge.events.EventToString(bge.logic.getExitKey())

      if exit_key.endswith("KEY"):
          exit_key = exit_key[:-3]

      font_object.worldPosition.y = -7
      
      # This way we can use bpy.types.TextCurve attributes
      font_object_text = font_object.blenderObject.data
      font_object_text.body = "Press key '%s' to quit the game." % exit_key
      font_object_text.size = 1
      font_object_text.resolution_u = 1
      font_object_text.align_x = LEFT
