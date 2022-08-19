KX_2DFilterManager(EXP_PyObjectPlus)
====================================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.EXP_PyObjectPlus`

.. class:: KX_2DFilterManager

   2D filter manager used to add, remove and find filters in a scene.

   .. method:: addFilter(index, type, fragmentProgram)

      Add a filter to the pass index :data:`index`, type :data:`type` and fragment program if custom filter.

      :arg index: The filter pass index.
      :type index: integer
      :arg type: The filter type, one of:

         * :data:`bge.logic.RAS_2DFILTER_BLUR`
         * :data:`bge.logic.RAS_2DFILTER_DILATION`
         * :data:`bge.logic.RAS_2DFILTER_EROSION`
         * :data:`bge.logic.RAS_2DFILTER_SHARPEN`
         * :data:`bge.logic.RAS_2DFILTER_LAPLACIAN`
         * :data:`bge.logic.RAS_2DFILTER_PREWITT`
         * :data:`bge.logic.RAS_2DFILTER_SOBEL`
         * :data:`bge.logic.RAS_2DFILTER_GRAYSCALE`
         * :data:`bge.logic.RAS_2DFILTER_SEPIA`
         * :data:`bge.logic.RAS_2DFILTER_CUSTOMFILTER`

      :type type: integer
      :arg fragmentProgram: The filter shader fragment program.
          Specified only if :data:`type` is :data:`bge.logic.RAS_2DFILTER_CUSTOMFILTER`. (optional)
      :type fragmentProgram: string
      :return: The 2D Filter.
      :rtype: :class:`~bge.types.KX_2DFilter`

   .. method:: removeFilter(index)

      Remove filter to the pass index :data:`index`.

      :arg index: The filter pass index.
      :type index: integer

   .. method:: getFilter(index)

      Return filter to the pass index :data:`index`.

      :warning: If the 2D Filter is added with a :class:`~bge.types.SCA_2DFilterActuator`, the filter will
          be available only after the 2D Filter program is linked. The python script to get the filter
          has to be executed one frame later. A delay sensor can be used.

      :arg index: The filter pass index.
      :type index: integer
      :return: The filter in the specified pass index or None.
      :rtype: :class:`~bge.types.KX_2DFilter` or None
