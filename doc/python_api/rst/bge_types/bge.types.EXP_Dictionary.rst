EXP_Dictionary(EXP_Value)
=========================

base class --- :class:`EXP_Value`

.. class:: EXP_Dictionary(EXP_Value)

   This class allow properties support through python dictionary access.

   .. code-block:: python

        

   .. method:: get(key[, default])

      Return the value matching key, or the default value if its not found.
      :arg key: the matching key
      :type key: string
      :arg default: optional default value is the key isn't matching, defaults to None if no value passed.
      :return: The key value or a default.

   .. method:: getPropertyNames()

      Gets a list of all property names.

      :return: All property names for this object.
      :rtype: list
