SCA_VisibilityActuator(SCA_IActuator)
=====================================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.SCA_IActuator`

.. class:: SCA_VisibilityActuator

   Visibility Actuator.

   .. attribute:: visibility

      whether the actuator makes its parent object visible or invisible.

      :type: boolean

   .. attribute:: useOcclusion

      whether the actuator makes its parent object an occluder or not.

      :type: boolean

   .. attribute:: useRecursion

      whether the visibility/occlusion should be propagated to all children of the object.

      :type: boolean
