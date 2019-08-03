KX_CharacterWrapper(EXP_PyObjectPlus)
=====================================

base class --- :class:`EXP_PyObjectPlus`

.. class:: KX_CharacterWrapper(EXP_PyObjectPlus)

   A wrapper to expose character physics options.

   .. attribute:: onGround

      Whether or not the character is on the ground. (read-only)

      :type: boolean

   .. attribute:: gravity

      The gravity value used for the character.

      :type: float

   .. attribute:: fallSpeed

      The character falling speed.

      :type: float

   .. attribute:: maxJumps

      The maximum number of jumps a character can perform before having to touch the ground. By default this is set to 1. 2 allows for a double jump, etc.

      :type: int in [0, 255], default 1

   .. attribute:: jumpCount

      The current jump count. This can be used to have different logic for a single jump versus a double jump. For example, a different animation for the second jump.

      :type: int

   .. attribute:: jumpSpeed

      The character jumping speed.

      :type: float

   .. attribute:: maxSlope

      The maximum slope which the character can climb.

      :type: float

   .. attribute:: walkDirection

      The speed and direction the character is traveling in using world coordinates. This should be used instead of applyMovement() to properly move the character.

      :type: Vector((x, y, z))

   .. method:: jump()

      The character jumps based on it's jump speed.
   .. method:: setVelocity(velocity, time, local=False)

      Sets the character's linear velocity for a given period.

      This method sets character's velocity through it's center of mass during a period.

      :arg velocity: Linear velocity vector.
      :type velocity: 3D Vector
      :arg time: Period while applying linear velocity.
      :type time: float
      :arg local:
         * False: you get the "global" velocity ie: relative to world orientation.
         * True: you get the "local" velocity ie: relative to object orientation.
      :type local: boolean

   .. method:: reset()

      Resets the character velocity and walk direction.
