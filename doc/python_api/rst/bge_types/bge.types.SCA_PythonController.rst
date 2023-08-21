SCA_PythonController(SCA_IController)
=====================================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.SCA_IController`

.. class:: SCA_PythonController

   A Python controller uses a Python script to activate it's actuators,
   based on it's sensors.

   .. attribute:: owner

      The object the controller is attached to.

      :type: :class:`~bge.types.KX_GameObject`

   .. attribute:: script

      The value of this variable depends on the execution method.

      * When 'Script' execution mode is set this value contains the entire python script as a single string (not the script name as you might expect) which can be modified to run different scripts.
      * When 'Module' execution mode is set this value will contain a single line string - module name and function "module.func" or "package.module.func" where the module names are python textblocks or external scripts.

      :type: string

      .. note::

         Once this is set the script name given for warnings will remain unchanged.

   .. attribute:: mode

      the execution mode for this controller (read-only).

      * Script: 0, Execite the :attr:`script` as a python code.
      * Module: 1, Execite the :attr:`script` as a module and function.

      :type: integer

   .. method:: activate(actuator)

      Activates an actuator attached to this controller.

      :arg actuator: The actuator to operate on. Expects either an actuator instance or its name.
      :type actuator: :class:`~bge.types.SCA_IActuator` or string

   .. method:: deactivate(actuator)

      Deactivates an actuator attached to this controller.

      :arg actuator: The actuator to operate on. Expects either an actuator instance or its name.
      :type actuator: :class:`~bge.types.SCA_IActuator` or string

