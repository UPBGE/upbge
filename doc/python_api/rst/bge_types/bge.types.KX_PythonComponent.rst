KX_PythonComponent(EXP_Value)
=============================

base class --- :class:`EXP_Value`

.. class:: KX_PythonComponent(EXP_Value)

   Python component can be compared to python logic bricks with parameters.
   The python component is a script loaded in the UI, this script defined a component class by inheriting from :class:`KX_PythonComponent`.
   This class must contain a dictionary of properties: :attr:`args` and two default functions: :meth:`start` and :meth:`update`.

   The script must have .py extension.

   The component properties are loaded from the :attr:`args` attribute from the UI at loading time.
   When the game start the function :meth:`start` is called with as arguments a dictionary of the properties' name and value.
   The :meth:`update` function is called every frames during the logic stage before running logics bricks,
   the goal of this function is to handle and process everything.

   The following component example moves and rotates the object when pressing the keys W, A, S and D.

   .. code-block:: python

      import bge
      from collections import OrderedDict
      
      class ThirdPerson(bge.types.KX_PythonComponent):
          """Basic third person controls
      
          W: move forward
          A: turn left
          S: move backward
          D: turn right
      
          """
      
          #
      
          args = OrderedDict([
              ("Move Speed", 0.1),
              ("Turn Speed", 0.04)
          ])
      
          def start(self, args):
              self.move_speed = args['Move Speed']
              self.turn_speed = args['Turn Speed']
      
          def update(self):
              keyboard = bge.logic.keyboard.events
      
              move = 0
              rotate = 0
      
              if keyboard[bge.events.WKEY]:
                  move += self.move_speed
              if keyboard[bge.events.SKEY]:
                  move -= self.move_speed
      
              if keyboard[bge.events.AKEY]:
                  rotate += self.turn_speed
              if keyboard[bge.events.DKEY]:
                  rotate -= self.turn_speed
      
              self.object.applyMovement((0, move, 0), True)
              self.object.applyRotation((0, 0, rotate), True)

   Since the components are loaded for the first time outside the bge, then :attr:`bge` is a fake module that contains only the class
   :class:`KX_PythonComponent` to avoid importing all the bge modules.
   This behavior is safer but creates some issues at loading when the user want to use functions or attributes from the bge modules other
   than the :class:`KX_PythonComponent` class. The way is to not call these functions at loading outside the bge. To detect it, the bge
   module contains the attribute :attr:`__component__` when it's imported outside the bge.

   The following component example add a "Cube" object at initialization and move it along x for each update. It shows that the user can
   use functions from scene and load the component outside the bge by setting global attributes in a condition at the beginning of the
   script.

   .. code-block:: python

      import bge
      
      if not hasattr(bge, "__component__"):
          global scene
          scene = bge.logic.getCurrentScene()

      class Component(bge.types.KX_PythonComponent):
          args = {}

          def start(self, args):
              scene.addObject("Cube")

          def update(self):
              scene.objects["Cube"].worldPosition.x += 0.1

   The property types supported are float, integer, boolean, string, set (for enumeration) and Vector 2D, 3D and 4D. The following example
   show all of these property types.

   .. code-block:: python

      from bge import *
      from mathutils import *
      from collections import OrderedDict

      class Component(types.KX_PythonComponent):
           args = OrderedDict([
               ("Float", 58.6),
               ("Integer", 150),
               ("Boolean", True),
               ("String", "Cube"),
               ("Enum", {"Enum 1", "Enum 2", "Enum 3"}),
               ("Vector 2D", Vector((0.8, 0.7))),
               ("Vector 3D", Vector((0.4, 0.3, 0.1))),
               ("Vector 4D", Vector((0.5, 0.2, 0.9, 0.6)))
           ])

           def start(self, args):
               print(args)

           def update(self):
               pass

   .. attribute:: object

      The object owner of the component.

      :type: :class:`KX_GameObject`

   .. attribute:: args

      Dictionary of the component properties, the keys are string and the value can be: float, integer, Vector(2D/3D/4D), set, string.

      :type: dict

   .. method:: start(args)

      Initialize the component.

      :arg args: The dictionary of the properties' name and value.
      :type args: dict

      .. warning::

         This function must be inherited in the python component class.

   .. method:: update()

      Process the logic of the component.

      .. warning::

         This function must be inherited in the python component class.
