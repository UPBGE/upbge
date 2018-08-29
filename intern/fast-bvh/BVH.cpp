#include <algorithm>
#include "BVH.h"
#include "Log.h"
#include "Stopwatch.h"

namespace bvh {

//! Node for storing state information during traversal.
struct BVHTraversal {
  uint32_t i; // Node
  float mint; // Minimum hit time for this node.
  BVHTraversal() { }
  BVHTraversal(int _i, float _mint) : i(_i), mint(_mint) { }
};

//! - Compute the nearest intersection of all objects within the tree.
//! - Return true if hit was found, false otherwise.
//! - In the case where we want to find out of there is _ANY_ intersection at all,
//!   set occlusion == true, in which case we exit on the first hit, rather
//!   than find the closest.
bool BVH::getIntersection(const Ray& ray, IntersectionInfo* intersection, bool occlusion) const {
  intersection->t = 999999999.f;
  intersection->object = NULL;
  float bbhits[4];
  int32_t closer, other;

  // Working set
  BVHTraversal todo[64];
  int32_t stackptr = 0;

  // "Push" on the root node to the working set
  todo[stackptr].i = 0;
  todo[stackptr].mint = -9999999.f;

  while(stackptr>=0) {
    // Pop off the next node to work on.
    int ni = todo[stackptr].i;
    float near = todo[stackptr].mint;
    stackptr--;
    const BVHFlatNode &node(flatTree[ ni ]);

    // If this node is further than the closest found intersection, continue
    if(near > intersection->t)
      continue;

    // Is leaf -> Intersect
    if( node.rightOffset == 0 ) {
      for(uint32_t o=0;o<node.nPrims;++o) {
        IntersectionInfo current;

        const Object* obj = (*build_prims)[node.start+o];
        bool hit = obj->getIntersection(ray, &current);

        if (hit) {
          // If we're only looking for occlusion, then any hit is good enough
          if(occlusion) {
            return true;
          }

          // Otherwise, keep the closest intersection only
          if (current.t < intersection->t) {
            *intersection = current;
          }
        }
      }

    } else { // Not a leaf

      bool hitc0 = flatTree[ni+1].bbox.intersect(ray, bbhits, bbhits+1);
      bool hitc1 = flatTree[ni+node.rightOffset].bbox.intersect(ray, bbhits+2, bbhits+3);

      // Did we hit both nodes?
      if(hitc0 && hitc1) {

        // We assume that the left child is a closer hit...
        closer = ni+1;
        other = ni+node.rightOffset;

        // ... If the right child was actually closer, swap the relavent values.
        if(bbhits[2] < bbhits[0]) {
          std::swap(bbhits[0], bbhits[2]);
          std::swap(bbhits[1], bbhits[3]);
          std::swap(closer,other);
        }

        // It's possible that the nearest object is still in the other side, but we'll
        // check the further-awar node later...

        // Push the farther first
        todo[++stackptr] = BVHTraversal(other, bbhits[2]);

        // And now the closer (with overlap test)
        todo[++stackptr] = BVHTraversal(closer, bbhits[0]);
      }

      else if (hitc0) {
        todo[++stackptr] = BVHTraversal(ni+1, bbhits[0]);
      }

      else if(hitc1) {
        todo[++stackptr] = BVHTraversal(ni + node.rightOffset, bbhits[2]);
      }

    }
  }

  // If we hit something,
  if(intersection->object != NULL)
    intersection->hit = ray.o + ray.d * intersection->t;

  return intersection->object != NULL;
}

BVH::~BVH() {
  delete[] flatTree;
}

BVH::BVH()
	:flatTree(nullptr)
{
}

BVH::BVH(std::vector<Object*>* objects, uint32_t leafSize)
  : nNodes(0), nLeafs(0), leafSize(leafSize), build_prims(objects), flatTree(NULL) {
    Stopwatch sw;

    // Build the tree based on the input object data set.
    build();

    // Output tree build time and statistics
    double constructionTime = sw.read();
    LOG_STAT("Built BVH (%d nodes, with %d leafs) in %d ms", nNodes, nLeafs, (int)(1000*constructionTime));
  }

struct BVHBuildEntry {
  // If non-zero then this is the index of the parent. (used in offsets)
  uint32_t parent;
  // The range of objects in the object list covered by this node.
  uint32_t start, end;
};

/*! Build the BVH, given an input data set
 *  - Handling our own stack is quite a bit faster than the recursive style.
 *  - Each build stack entry's parent field eventually stores the offset
 *    to the parent of that node. Before that is finally computed, it will
 *    equal exactly three other values. (These are the magic values Untouched,
 *    Untouched-1, and TouchedTwice).
 *  - The partition here was also slightly faster than std::partition.
 */
void BVH::build()
{
  BVHBuildEntry todo[128];
  uint32_t stackptr = 0;
  const uint32_t Untouched    = 0xffffffff;
  const uint32_t TouchedTwice = 0xfffffffd;

  // Push the root
  todo[stackptr].start = 0;
  todo[stackptr].end = build_prims->size();
  todo[stackptr].parent = 0xfffffffc;
  stackptr++;

  BVHFlatNode node;
  std::vector<BVHFlatNode> buildnodes;
  buildnodes.reserve(build_prims->size()*2);

  while(stackptr > 0) {
    // Pop the next item off of the stack
    BVHBuildEntry &bnode( todo[--stackptr] );
    uint32_t start = bnode.start;
    uint32_t end = bnode.end;
    uint32_t nPrims = end - start;

    nNodes++;
    node.start = start;
    node.nPrims = nPrims;
    node.rightOffset = Untouched;

    // Calculate the bounding box for this node
    BBox bb( (*build_prims)[start]->getBBox());
    BBox bc( (*build_prims)[start]->getCentroid());
    for(uint32_t p = start+1; p < end; ++p) {
      bb.expandToInclude( (*build_prims)[p]->getBBox());
      bc.expandToInclude( (*build_prims)[p]->getCentroid());
    }
    node.bbox = bb;

    // If the number of primitives at this point is less than the leaf
    // size, then this will become a leaf. (Signified by rightOffset == 0)
    if(nPrims <= leafSize) {
      node.rightOffset = 0;
      nLeafs++;
    }

    buildnodes.push_back(node);

    // Child touches parent...
    // Special case: Don't do this for the root.
    if(bnode.parent != 0xfffffffc) {
      buildnodes[bnode.parent].rightOffset --;

      // When this is the second touch, this is the right child.
      // The right child sets up the offset for the flat tree.
      if( buildnodes[bnode.parent].rightOffset == TouchedTwice ) {
        buildnodes[bnode.parent].rightOffset = nNodes - 1 - bnode.parent;
      }
    }

    // If this is a leaf, no need to subdivide.
    if(node.rightOffset == 0)
      continue;

    // Set the split dimensions
    uint32_t split_dim = bc.maxDimension();

    // Split on the center of the longest axis
    float split_coord = .5f * (bc.min[split_dim] + bc.max[split_dim]);

    // Partition the list of objects on this split
    uint32_t mid = start;
    for(uint32_t i=start;i<end;++i) {
      if( (*build_prims)[i]->getCentroid()[split_dim] < split_coord ) {
        std::swap( (*build_prims)[i], (*build_prims)[mid] );
        ++mid;
      }
    }

    // If we get a bad split, just choose the center...
    if(mid == start || mid == end) {
      mid = start + (end-start)/2;
    }

    // Push right child
    todo[stackptr].start = mid;
    todo[stackptr].end = end;
    todo[stackptr].parent = nNodes-1;
    stackptr++;

    // Push left child
    todo[stackptr].start = start;
    todo[stackptr].end = mid;
    todo[stackptr].parent = nNodes-1;
    stackptr++;
  }

  // Copy the temp node data to a flat array
  flatTree = new BVHFlatNode[nNodes];
  for(uint32_t n=0; n<nNodes; ++n) 
    flatTree[n] = buildnodes[n];
}

};
