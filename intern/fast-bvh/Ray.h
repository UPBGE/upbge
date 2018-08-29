#ifndef Ray_h
#define Ray_h

#include "mathfu.h"

namespace bvh {

struct Ray {
  mt::vec3 o; // Ray Origin
  mt::vec3 d; // Ray Direction
  mt::vec3 inv_d; // Inverse of each Ray Direction component

  Ray(const mt::vec3& o, const mt::vec3& d)
    : o(o), d(d), inv_d(1.0f / d.x, 1.0f / d.y, 1.0f / d.z) { }
};

};

#endif
