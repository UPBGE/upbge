EXP_ListValue(EXP_PropValue)
============================

base class --- :class:`EXP_PropValue`

.. class:: EXP_ListValue(EXP_PropValue)

   This is a list like object used in the game engine internally that behaves similar to a python list in most ways.

   As well as the normal index lookup (``val= clist[i]``), EXP_ListValue supports string lookups (``val= scene.objects["Cube"]``)

   Other operations such as ``len(clist)``, ``list(clist)``, ``clist[0:10]`` are also supported.

   .. method:: count(val)

      Count the number of instances of a value in the list.

      :return: number of instances
      :rtype: integer

   .. method:: index(val)

      Return the index of a value in the list.

      :return: The index of the value in the list.
      :rtype: integer

   .. method:: get(key, default=None)

      Return the value matching key, or the default value if its not found.

      :return: The key value or a default.

   .. method:: filter(name, prop)

      Return a list of items with name matching `name` regex and with a property matching `prop` regex.
      If `name` is empty every items are checked, if `prop` is empty no property check is proceeded.

      :return: The list of matching items.

   .. method:: from_id(id)

      This is a funtion especially for the game engine to return a value with a spesific id.

      Since object names are not always unique, the id of an object can be used to get an object from the EXP_ValueList.

      Example:

      .. code-block:: python
        
         myObID=id(gameObject)
         ob= scene.objects.from_id(myObID)

      Where ``myObID`` is an int or long from the id function.

      This has the advantage that you can store the id in places you could not store a gameObject.

      .. warning::

         The id is derived from a memory location and will be different each time the game engine starts.

      .. warning::

         The id can't be stored as an integer in game object properties, as those only have a limited range that the id may not be contained in. Instead an id can be stored as a string game property and converted back to an integer for use in from_id lookups.
