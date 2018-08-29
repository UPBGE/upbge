#ifndef BBox_h
#define BBox_h

#include "Ray.h"
#include <stdint.h>

namespace bvh {

struct BBox {
  mt::vec3 min, max, extent;
  BBox() { }
  BBox(const mt::vec3& min, const mt::vec3& max);
  BBox(const mt::vec3& p);

  bool intersect(const Ray& ray, float *tnear, float *tfar) const;
  void expandToInclude(const mt::vec3& p);
  void expandToInclude(const BBox& b);
  uint32_t maxDimension() const;
  float surfaceArea() const;
};

};

#endif
