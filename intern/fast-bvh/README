Code: Fast-BVH, an optimized Bounding Volume Hierarchy
Author: Brandon Pelfrey (brandonpelfrey@gmail.com)
        Borrowed AABB Intersection code referenced in BBox.cpp
Date: April 17, 2012

This code, which I am releasing for public use, provides an optimized Bounding Volume Hierarchy (BVH).
The vector and axis-aligned bounding box (AABB) code included requires SSE in order to function.
The entire build and intersection test functionality is recursion-less and is much more optimized than
the one found in say, PBRT (Physically Based Rendering, by Pharr + Humphreys). With that said, if you
see another cool trick to speed things up, I'd love to see it!

The BVH itself makes minimal assumptions on the items it may contain: Objects need only implement the 
getIntersection(), getBBox(), and getCentroid() functions in order to be queryable.
The included example code renders a million spheres distributed inside of a cube using the BVH (in <3 seconds on my old MacBook!)   

I have attempted to comment this code well and make everything well-understood. If there is an issue 
with the code, a question as to how something works, or if you use this in a project, please let me know!
