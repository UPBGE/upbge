SCA_ILogicBrick(EXP_Value)
==========================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.EXP_Value`

.. class:: SCA_ILogicBrick

   Base class for all logic bricks.

   .. attribute:: executePriority

      This determines the order controllers are evaluated, and actuators are activated (lower priority is executed first).

      :type: integer

   .. attribute:: owner

      The game object this logic brick is attached to (read-only).

      :type: :class:`~bge.types.KX_GameObject` or None in exceptional cases.

   .. attribute:: name

      The name of this logic brick (read-only).

      :type: string
