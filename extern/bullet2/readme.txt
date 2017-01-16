This is the new refactored version of Bullet physics library version 2.x

Questions? mail blender at erwincoumans.com, or check the bf-blender mailing list.
Thanks,
Erwin

Applied patches/coneshape_setheight.patch to add access to the posibility of
modify cone's height and angle is already upstreamed from bullet 2.85.
There is no necessity to backport this patch to the next bullet version

Apply patches/blender.patch to fix a few build errors and warnings and dd original
vertex access for BMesh convex hull operator.

Documentation is available at:
http://code.google.com/p/bullet/source/browse/trunk/Bullet_User_Manual.pdf
and:
https://github.com/bulletphysics/bullet3/tree/master/docs
