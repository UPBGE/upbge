/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * An IndexMask references an array of unsigned integers with the following property:
 *   The integers must be in ascending order and there must not be duplicates.
 *
 * Remember that the array is only referenced and not owned by an IndexMask instance.
 *
 * In most cases the integers in the array represent some indices into another array. So they
 * "select" or "mask" a some elements in that array. Hence the name IndexMask.
 *
 * The invariant stated above has the nice property that it makes it easy to check if an integer
 * array is an IndexRange, i.e. no indices are skipped. That allows functions to implement two code
 * paths: One where it iterates over the index array and one where it iterates over the index
 * range. The latter one is more efficient due to less memory reads and potential usage of SIMD
 * instructions.
 *
 * The IndexMask.foreach_index method helps writing code that implements both code paths at the
 * same time.
 */

#include "BLI_index_range.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender {

class IndexMask {
 private:
  /** The underlying reference to sorted integers. */
  Span<int64_t> indices_;

 public:
  /** Creates an IndexMask that contains no indices. */
  IndexMask() = default;

  /**
   * Create an IndexMask using the given integer array.
   * This constructor asserts that the given integers are in ascending order and that there are no
   * duplicates.
   */
  IndexMask(Span<int64_t> indices) : indices_(indices)
  {
    BLI_assert(IndexMask::indices_are_valid_index_mask(indices));
  }

  /**
   * Use this method when you know that no indices are skipped. It is more efficient than preparing
   * an integer array all the time.
   */
  IndexMask(IndexRange range) : indices_(range.as_span())
  {
  }

  /**
   * Construct an IndexMask from a sorted list of indices. Note, the created IndexMask is only
   * valid as long as the initializer_list is valid.
   *
   * Don't do this:
   *   IndexMask mask = {3, 4, 5};
   *
   * Do this:
   *   do_something_with_an_index_mask({3, 4, 5});
   */
  IndexMask(const std::initializer_list<int64_t> &indices) : IndexMask(Span<int64_t>(indices))
  {
  }

  /**
   * Creates an IndexMask that references the indices [0, n-1].
   */
  explicit IndexMask(int64_t n) : IndexMask(IndexRange(n))
  {
  }

  /** Checks that the indices are non-negative and in ascending order. */
  static bool indices_are_valid_index_mask(Span<int64_t> indices)
  {
    if (!indices.is_empty()) {
      if (indices.first() < 0) {
        return false;
      }
    }
    for (int64_t i = 1; i < indices.size(); i++) {
      if (indices[i - 1] >= indices[i]) {
        return false;
      }
    }
    return true;
  }

  operator Span<int64_t>() const
  {
    return indices_;
  }

  const int64_t *begin() const
  {
    return indices_.begin();
  }

  const int64_t *end() const
  {
    return indices_.end();
  }

  /**
   * Returns the n-th index referenced by this IndexMask. The `index_range` method returns an
   * IndexRange containing all indices that can be used as parameter here.
   */
  int64_t operator[](int64_t n) const
  {
    return indices_[n];
  }

  /**
   * Returns the minimum size an array has to have, if the integers in this IndexMask are going to
   * be used as indices in that array.
   */
  int64_t min_array_size() const
  {
    if (indices_.size() == 0) {
      return 0;
    }
    else {
      return indices_.last() + 1;
    }
  }

  Span<int64_t> indices() const
  {
    return indices_;
  }

  /**
   * Returns true if this IndexMask does not skip any indices. This check requires O(1) time.
   */
  bool is_range() const
  {
    return indices_.size() > 0 && indices_.last() - indices_.first() == indices_.size() - 1;
  }

  /**
   * Returns the IndexRange referenced by this IndexMask. This method should only be called after
   * the caller made sure that this IndexMask is actually a range.
   */
  IndexRange as_range() const
  {
    BLI_assert(this->is_range());
    return IndexRange{indices_.first(), indices_.size()};
  }

  /**
   * Calls the given callback for every referenced index. The callback has to take one unsigned
   * integer as parameter.
   *
   * This method implements different code paths for the cases when the IndexMask represents a
   * range or not.
   */
  template<typename CallbackT> void foreach_index(const CallbackT &callback) const
  {
    this->to_best_mask_type([&](const auto &mask) {
      for (const int64_t i : mask) {
        callback(i);
      }
    });
  }

  /**
   * Often an #IndexMask wraps a range of indices without any gaps. In this case, it is more
   * efficient to compute the indices in a loop on-the-fly instead of reading them from memory.
   * This method makes it easy to generate code for both cases.
   *
   * The given function is expected to take one parameter that can either be of type #IndexRange or
   * #Span<int64_t>.
   */
  template<typename Fn> void to_best_mask_type(const Fn &fn) const
  {
    if (this->is_range()) {
      const IndexRange masked_range = this->as_range();
      fn(masked_range);
    }
    else {
      const Span<int64_t> masked_indices = indices_;
      fn(masked_indices);
    }
  }

  /**
   * Returns an IndexRange that can be used to index this IndexMask.
   *
   * The range is [0, number of indices - 1].
   *
   * This is not to be confused with the `as_range` method.
   */
  IndexRange index_range() const
  {
    return indices_.index_range();
  }

  /**
   * Returns the largest index that is referenced by this IndexMask.
   */
  int64_t last() const
  {
    return indices_.last();
  }

  /**
   * Returns the number of indices referenced by this IndexMask.
   */
  int64_t size() const
  {
    return indices_.size();
  }

  bool is_empty() const
  {
    return indices_.is_empty();
  }

  bool contained_in(const IndexRange range) const
  {
    if (indices_.is_empty()) {
      return true;
    }
    if (range.size() < indices_.size()) {
      return false;
    }
    return indices_.first() >= range.first() && indices_.last() <= range.last();
  }

  IndexMask slice(int64_t start, int64_t size) const;
  IndexMask slice(IndexRange slice) const;
  /**
   * Create a sub-mask that is also shifted to the beginning.
   * The shifting to the beginning allows code to work with smaller indices,
   * which is more memory efficient.
   *
   * \return New index mask with the size of #slice. It is either empty or starts with 0.
   * It might reference indices that have been appended to #r_new_indices.
   *
   * Example:
   * \code{.unparsed}
   * this:   [2, 3, 5, 7, 8, 9, 10]
   * slice:      ^--------^
   * output: [0, 2, 4, 5]
   * \endcode
   *
   * All the indices in the sub-mask are shifted by 3 towards zero,
   * so that the first index in the output is zero.
   */
  IndexMask slice_and_offset(IndexRange slice, Vector<int64_t> &r_new_indices) const;

  /**
   * Get a new mask that contains all the indices that are not in the current mask.
   * If necessary, the indices referenced by the new mask are inserted in #r_new_indices.
   */
  IndexMask invert(const IndexRange full_range, Vector<int64_t> &r_new_indices) const;

  /**
   * Get all contiguous index ranges within the mask.
   */
  Vector<IndexRange> extract_ranges() const;

  /**
   * Similar to #extract ranges, but works on the inverted mask. So the returned ranges are
   * in-between the indices in the mask.
   *
   * Using this method is generally more efficient than first inverting the index mask and then
   * extracting the ranges.
   *
   * If #r_skip_amounts is passed in, it will contain the number of indices that have been skipped
   * before each range in the return value starts.
   */
  Vector<IndexRange> extract_ranges_invert(const IndexRange full_range,
                                           Vector<int64_t> *r_skip_amounts = nullptr) const;
};

}  // namespace blender
