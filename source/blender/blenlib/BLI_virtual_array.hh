/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * A virtual array is a data structure that behaves similar to an array, but its elements are
 * accessed through virtual methods. This improves the decoupling of a function from its callers,
 * because it does not have to know exactly how the data is laid out in memory, or if it is stored
 * in memory at all. It could just as well be computed on the fly.
 *
 * Taking a virtual array as parameter instead of a more specific non-virtual type has some
 * tradeoffs. Access to individual elements of the individual elements is higher due to function
 * call overhead. On the other hand, potential callers don't have to convert the data into the
 * specific format required for the function. This can be a costly conversion if only few of the
 * elements are accessed in the end.
 *
 * Functions taking a virtual array as input can still optimize for different data layouts. For
 * example, they can check if the array is stored as an array internally or if it is the same
 * element for all indices. Whether it is worth to optimize for different data layouts in a
 * function has to be decided on a case by case basis. One should always do some benchmarking to
 * see of the increased compile time and binary size is worth it.
 */

#include "BLI_any.hh"
#include "BLI_array.hh"
#include "BLI_index_mask.hh"
#include "BLI_span.hh"

namespace blender {

/** Forward declarations for generic virtual arrays. */
class GVArray;
class GVMutableArray;

/**
 * Is used to quickly check if a varray is a span or single value. This struct also allows
 * retrieving multiple pieces of data with a single virtual method call.
 */
struct CommonVArrayInfo {
  enum class Type : uint8_t {
    /* Is not one of the common special types below. */
    Any,
    Span,
    Single,
  };

  Type type = Type::Any;

  /** True when the #data becomes a dangling pointer when the virtual array is destructed. */
  bool may_have_ownership = true;

  /**
   * Points either to nothing, a single value or array of values, depending on #type.
   * If this is a span of a mutable virtual array, it is safe to cast away const.
   */
  const void *data;

  CommonVArrayInfo() = default;
  CommonVArrayInfo(const Type _type, const bool _may_have_ownership, const void *_data)
      : type(_type), may_have_ownership(_may_have_ownership), data(_data)
  {
  }
};

/**
 * Implements the specifics of how the elements of a virtual array are accessed. It contains a
 * bunch of virtual methods that are wrapped by #VArray.
 */
template<typename T> class VArrayImpl {
 protected:
  /**
   * Number of elements in the virtual array. All virtual arrays have a size, but in some cases it
   * may make sense to set it to the max value.
   */
  int64_t size_;

 public:
  VArrayImpl(const int64_t size) : size_(size)
  {
    BLI_assert(size_ >= 0);
  }

  virtual ~VArrayImpl() = default;

  int64_t size() const
  {
    return size_;
  }

  /**
   * Get the element at #index. This does not return a reference, because the value may be computed
   * on the fly.
   */
  virtual T get(int64_t index) const = 0;

  virtual CommonVArrayInfo common_info() const
  {
    return {};
  }

  /**
   * Copy values from the virtual array into the provided span. The index of the value in the
   * virtual array is the same as the index in the span.
   */
  virtual void materialize(IndexMask mask, MutableSpan<T> r_span) const
  {
    T *dst = r_span.data();
    /* Optimize for a few different common cases. */
    const CommonVArrayInfo info = this->common_info();
    switch (info.type) {
      case CommonVArrayInfo::Type::Any: {
        mask.foreach_index([&](const int64_t i) { dst[i] = this->get(i); });
        break;
      }
      case CommonVArrayInfo::Type::Span: {
        const T *src = static_cast<const T *>(info.data);
        mask.foreach_index([&](const int64_t i) { dst[i] = src[i]; });
        break;
      }
      case CommonVArrayInfo::Type::Single: {
        const T single = *static_cast<const T *>(info.data);
        mask.foreach_index([&](const int64_t i) { dst[i] = single; });
        break;
      }
    }
  }

  /**
   * Same as #materialize but #r_span is expected to be uninitialized.
   */
  virtual void materialize_to_uninitialized(IndexMask mask, MutableSpan<T> r_span) const
  {
    T *dst = r_span.data();
    /* Optimize for a few different common cases. */
    const CommonVArrayInfo info = this->common_info();
    switch (info.type) {
      case CommonVArrayInfo::Type::Any: {
        mask.foreach_index([&](const int64_t i) { new (dst + i) T(this->get(i)); });
        break;
      }
      case CommonVArrayInfo::Type::Span: {
        const T *src = static_cast<const T *>(info.data);
        mask.foreach_index([&](const int64_t i) { new (dst + i) T(src[i]); });
        break;
      }
      case CommonVArrayInfo::Type::Single: {
        const T single = *static_cast<const T *>(info.data);
        mask.foreach_index([&](const int64_t i) { new (dst + i) T(single); });
        break;
      }
    }
  }

  /**
   * Copy values from the virtual array into the provided span. Contrary to #materialize, the index
   * in virtual array is not the same as the index in the output span. Instead, the span is filled
   * without gaps.
   */
  virtual void materialize_compressed(IndexMask mask, MutableSpan<T> r_span) const
  {
    BLI_assert(mask.size() == r_span.size());
    mask.to_best_mask_type([&](auto best_mask) {
      for (const int64_t i : IndexRange(best_mask.size())) {
        r_span[i] = this->get(best_mask[i]);
      }
    });
  }

  /**
   * Same as #materialize_compressed but #r_span is expected to be uninitialized.
   */
  virtual void materialize_compressed_to_uninitialized(IndexMask mask, MutableSpan<T> r_span) const
  {
    BLI_assert(mask.size() == r_span.size());
    T *dst = r_span.data();
    mask.to_best_mask_type([&](auto best_mask) {
      for (const int64_t i : IndexRange(best_mask.size())) {
        new (dst + i) T(this->get(best_mask[i]));
      }
    });
  }

  /**
   * If this virtual wraps another #GVArray, this method should assign the wrapped array to the
   * provided reference. This allows losslessly converting between generic and typed virtual
   * arrays in all cases.
   * Return true when the virtual array was assigned and false when nothing was done.
   */
  virtual bool try_assign_GVArray(GVArray &UNUSED(varray)) const
  {
    return false;
  }

  /**
   * Return true when the other virtual array should be considered to be the same, e.g. because it
   * shares the same underlying memory.
   */
  virtual bool is_same(const VArrayImpl<T> &UNUSED(other)) const
  {
    return false;
  }
};

/** Similar to #VArrayImpl, but adds methods that allow modifying the referenced elements. */
template<typename T> class VMutableArrayImpl : public VArrayImpl<T> {
 public:
  using VArrayImpl<T>::VArrayImpl;

  /**
   * Assign the provided #value to the #index.
   */
  virtual void set(int64_t index, T value) = 0;

  /**
   * Copy all elements from the provided span into the virtual array.
   */
  virtual void set_all(Span<T> src)
  {
    const CommonVArrayInfo info = this->common_info();
    if (info.type == CommonVArrayInfo::Type::Span) {
      initialized_copy_n(
          src.data(), this->size_, const_cast<T *>(static_cast<const T *>(info.data)));
    }
    else {
      const int64_t size = this->size_;
      for (int64_t i = 0; i < size; i++) {
        this->set(i, src[i]);
      }
    }
  }

  /**
   * Similar to #VArrayImpl::try_assign_GVArray but for mutable virtual arrays.
   */
  virtual bool try_assign_GVMutableArray(GVMutableArray &UNUSED(varray)) const
  {
    return false;
  }
};

/**
 * A virtual array implementation that references that wraps a span. This implementation is used by
 * mutable and immutable spans to avoid code duplication.
 */
template<typename T> class VArrayImpl_For_Span : public VMutableArrayImpl<T> {
 protected:
  T *data_ = nullptr;

 public:
  VArrayImpl_For_Span(const MutableSpan<T> data)
      : VMutableArrayImpl<T>(data.size()), data_(data.data())
  {
  }

 protected:
  VArrayImpl_For_Span(const int64_t size) : VMutableArrayImpl<T>(size)
  {
  }

  T get(const int64_t index) const final
  {
    return data_[index];
  }

  void set(const int64_t index, T value) final
  {
    data_[index] = value;
  }

  CommonVArrayInfo common_info() const override
  {
    return CommonVArrayInfo(CommonVArrayInfo::Type::Span, true, data_);
  }

  bool is_same(const VArrayImpl<T> &other) const final
  {
    if (other.size() != this->size_) {
      return false;
    }
    const CommonVArrayInfo other_info = other.common_info();
    if (other_info.type != CommonVArrayInfo::Type::Span) {
      return false;
    }
    return data_ == static_cast<const T *>(other_info.data);
  }

  void materialize_compressed(IndexMask mask, MutableSpan<T> r_span) const override
  {
    mask.to_best_mask_type([&](auto best_mask) {
      for (const int64_t i : IndexRange(best_mask.size())) {
        r_span[i] = data_[best_mask[i]];
      }
    });
  }

  void materialize_compressed_to_uninitialized(IndexMask mask,
                                               MutableSpan<T> r_span) const override
  {
    T *dst = r_span.data();
    mask.to_best_mask_type([&](auto best_mask) {
      for (const int64_t i : IndexRange(best_mask.size())) {
        new (dst + i) T(data_[best_mask[i]]);
      }
    });
  }
};

/**
 * A version of #VArrayImpl_For_Span that can not be subclassed. This allows safely overwriting the
 * #may_have_ownership method.
 */
template<typename T> class VArrayImpl_For_Span_final final : public VArrayImpl_For_Span<T> {
 public:
  using VArrayImpl_For_Span<T>::VArrayImpl_For_Span;

  VArrayImpl_For_Span_final(const Span<T> data)
      /* Cast const away, because the implementation for const and non const spans is shared. */
      : VArrayImpl_For_Span<T>({const_cast<T *>(data.data()), data.size()})
  {
  }

 private:
  CommonVArrayInfo common_info() const final
  {
    return CommonVArrayInfo(CommonVArrayInfo::Type::Span, false, this->data_);
  }
};

template<typename T>
inline constexpr bool is_trivial_extended_v<VArrayImpl_For_Span_final<T>> = true;

/**
 * A variant of `VArrayImpl_For_Span` that owns the underlying data.
 * The `Container` type has to implement a `size()` and `data()` method.
 * The `data()` method has to return a pointer to the first element in the continuous array of
 * elements.
 */
template<typename Container, typename T = typename Container::value_type>
class VArrayImpl_For_ArrayContainer : public VArrayImpl_For_Span<T> {
 private:
  Container container_;

 public:
  VArrayImpl_For_ArrayContainer(Container container)
      : VArrayImpl_For_Span<T>((int64_t)container.size()), container_(std::move(container))
  {
    this->data_ = const_cast<T *>(container_.data());
  }
};

/**
 * A virtual array implementation that returns the same value for every index. This class is final
 * so that it can be devirtualized by the compiler in some cases (e.g. when #devirtualize_varray is
 * used).
 */
template<typename T> class VArrayImpl_For_Single final : public VArrayImpl<T> {
 private:
  T value_;

 public:
  VArrayImpl_For_Single(T value, const int64_t size)
      : VArrayImpl<T>(size), value_(std::move(value))
  {
  }

 protected:
  T get(const int64_t UNUSED(index)) const override
  {
    return value_;
  }

  CommonVArrayInfo common_info() const override
  {
    return CommonVArrayInfo(CommonVArrayInfo::Type::Single, true, &value_);
  }

  void materialize_compressed(IndexMask mask, MutableSpan<T> r_span) const override
  {
    BLI_assert(mask.size() == r_span.size());
    UNUSED_VARS_NDEBUG(mask);
    r_span.fill(value_);
  }

  void materialize_compressed_to_uninitialized(IndexMask mask,
                                               MutableSpan<T> r_span) const override
  {
    BLI_assert(mask.size() == r_span.size());
    uninitialized_fill_n(r_span.data(), mask.size(), value_);
  }
};

template<typename T>
inline constexpr bool is_trivial_extended_v<VArrayImpl_For_Single<T>> = is_trivial_extended_v<T>;

/**
 * This class makes it easy to create a virtual array for an existing function or lambda. The
 * `GetFunc` should take a single `index` argument and return the value at that index.
 */
template<typename T, typename GetFunc> class VArrayImpl_For_Func final : public VArrayImpl<T> {
 private:
  GetFunc get_func_;

 public:
  VArrayImpl_For_Func(const int64_t size, GetFunc get_func)
      : VArrayImpl<T>(size), get_func_(std::move(get_func))
  {
  }

 private:
  T get(const int64_t index) const override
  {
    return get_func_(index);
  }

  void materialize(IndexMask mask, MutableSpan<T> r_span) const override
  {
    T *dst = r_span.data();
    mask.foreach_index([&](const int64_t i) { dst[i] = get_func_(i); });
  }

  void materialize_to_uninitialized(IndexMask mask, MutableSpan<T> r_span) const override
  {
    T *dst = r_span.data();
    mask.foreach_index([&](const int64_t i) { new (dst + i) T(get_func_(i)); });
  }

  void materialize_compressed(IndexMask mask, MutableSpan<T> r_span) const override
  {
    BLI_assert(mask.size() == r_span.size());
    T *dst = r_span.data();
    mask.to_best_mask_type([&](auto best_mask) {
      for (const int64_t i : IndexRange(best_mask.size())) {
        dst[i] = get_func_(best_mask[i]);
      }
    });
  }

  void materialize_compressed_to_uninitialized(IndexMask mask,
                                               MutableSpan<T> r_span) const override
  {
    BLI_assert(mask.size() == r_span.size());
    T *dst = r_span.data();
    mask.to_best_mask_type([&](auto best_mask) {
      for (const int64_t i : IndexRange(best_mask.size())) {
        new (dst + i) T(get_func_(best_mask[i]));
      }
    });
  }
};

/**
 * \note This is `final` so that #may_have_ownership can be implemented reliably.
 */
template<typename StructT,
         typename ElemT,
         ElemT (*GetFunc)(const StructT &),
         void (*SetFunc)(StructT &, ElemT) = nullptr>
class VArrayImpl_For_DerivedSpan final : public VMutableArrayImpl<ElemT> {
 private:
  StructT *data_;

 public:
  VArrayImpl_For_DerivedSpan(const MutableSpan<StructT> data)
      : VMutableArrayImpl<ElemT>(data.size()), data_(data.data())
  {
  }

  template<typename OtherStructT,
           typename OtherElemT,
           OtherElemT (*OtherGetFunc)(const OtherStructT &),
           void (*OtherSetFunc)(OtherStructT &, OtherElemT)>
  friend class VArrayImpl_For_DerivedSpan;

 private:
  ElemT get(const int64_t index) const override
  {
    return GetFunc(data_[index]);
  }

  void set(const int64_t index, ElemT value) override
  {
    SetFunc(data_[index], std::move(value));
  }

  void materialize(IndexMask mask, MutableSpan<ElemT> r_span) const override
  {
    ElemT *dst = r_span.data();
    mask.foreach_index([&](const int64_t i) { dst[i] = GetFunc(data_[i]); });
  }

  void materialize_to_uninitialized(IndexMask mask, MutableSpan<ElemT> r_span) const override
  {
    ElemT *dst = r_span.data();
    mask.foreach_index([&](const int64_t i) { new (dst + i) ElemT(GetFunc(data_[i])); });
  }

  void materialize_compressed(IndexMask mask, MutableSpan<ElemT> r_span) const override
  {
    BLI_assert(mask.size() == r_span.size());
    ElemT *dst = r_span.data();
    mask.to_best_mask_type([&](auto best_mask) {
      for (const int64_t i : IndexRange(best_mask.size())) {
        dst[i] = GetFunc(data_[best_mask[i]]);
      }
    });
  }

  void materialize_compressed_to_uninitialized(IndexMask mask,
                                               MutableSpan<ElemT> r_span) const override
  {
    BLI_assert(mask.size() == r_span.size());
    ElemT *dst = r_span.data();
    mask.to_best_mask_type([&](auto best_mask) {
      for (const int64_t i : IndexRange(best_mask.size())) {
        new (dst + i) ElemT(GetFunc(data_[best_mask[i]]));
      }
    });
  }

  bool is_same(const VArrayImpl<ElemT> &other) const override
  {
    if (other.size() != this->size_) {
      return false;
    }
    if (const VArrayImpl_For_DerivedSpan<StructT, ElemT, GetFunc> *other_typed =
            dynamic_cast<const VArrayImpl_For_DerivedSpan<StructT, ElemT, GetFunc> *>(&other)) {
      return other_typed->data_ == data_;
    }
    if (const VArrayImpl_For_DerivedSpan<StructT, ElemT, GetFunc, SetFunc> *other_typed =
            dynamic_cast<const VArrayImpl_For_DerivedSpan<StructT, ElemT, GetFunc, SetFunc> *>(
                &other)) {
      return other_typed->data_ == data_;
    }
    return false;
  }
};

template<typename StructT,
         typename ElemT,
         ElemT (*GetFunc)(const StructT &),
         void (*SetFunc)(StructT &, ElemT)>
inline constexpr bool
    is_trivial_extended_v<VArrayImpl_For_DerivedSpan<StructT, ElemT, GetFunc, SetFunc>> = true;

namespace detail {

/**
 * Struct that can be passed as `ExtraInfo` into an #Any.
 * This struct is only intended to be used by #VArrayCommon.
 */
template<typename T> struct VArrayAnyExtraInfo {
  /**
   * Gets the virtual array that is stored at the given pointer.
   */
  const VArrayImpl<T> *(*get_varray)(const void *buffer);

  template<typename StorageT> static constexpr VArrayAnyExtraInfo get()
  {
    /* These are the only allowed types in the #Any. */
    static_assert(
        std::is_base_of_v<VArrayImpl<T>, StorageT> ||
        is_same_any_v<StorageT, const VArrayImpl<T> *, std::shared_ptr<const VArrayImpl<T>>>);

    /* Depending on how the virtual array implementation is stored in the #Any, a different
     * #get_varray function is required. */
    if constexpr (std::is_base_of_v<VArrayImpl<T>, StorageT>) {
      return {[](const void *buffer) {
        return static_cast<const VArrayImpl<T> *>((const StorageT *)buffer);
      }};
    }
    else if constexpr (std::is_same_v<StorageT, const VArrayImpl<T> *>) {
      return {[](const void *buffer) { return *(const StorageT *)buffer; }};
    }
    else if constexpr (std::is_same_v<StorageT, std::shared_ptr<const VArrayImpl<T>>>) {
      return {[](const void *buffer) { return ((const StorageT *)buffer)->get(); }};
    }
    else {
      BLI_assert_unreachable();
      return {};
    }
  }
};

}  // namespace detail

/**
 * Utility class to reduce code duplication for methods available on #VArray and #VMutableArray.
 * Deriving #VMutableArray from #VArray would have some issues:
 * - Static methods on #VArray would also be available on #VMutableArray.
 * - It would allow assigning a #VArray to a #VMutableArray under some circumstances which is not
 *   allowed and could result in hard to find bugs.
 */
template<typename T> class VArrayCommon {
 protected:
  /**
   * Store the virtual array implementation in an #Any. This makes it easy to avoid a memory
   * allocation if the implementation is small enough and is copyable. This is the case for the
   * most common virtual arrays.
   * Other virtual array implementations are typically stored as #std::shared_ptr. That works even
   * when the implementation itself is not copyable and makes copying #VArrayCommon cheaper.
   */
  using Storage = Any<detail::VArrayAnyExtraInfo<T>, 24, 8>;

  /**
   * Pointer to the currently contained virtual array implementation. This is allowed to be null.
   */
  const VArrayImpl<T> *impl_ = nullptr;
  /**
   * Does the memory management for the virtual array implementation. It contains one of the
   * following:
   * - Inlined subclass of #VArrayImpl.
   * - Non-owning pointer to a #VArrayImpl.
   * - Shared pointer to a #VArrayImpl.
   */
  Storage storage_;

 protected:
  VArrayCommon() = default;

  /** Copy constructor. */
  VArrayCommon(const VArrayCommon &other) : storage_(other.storage_)
  {
    impl_ = this->impl_from_storage();
  }

  /** Move constructor. */
  VArrayCommon(VArrayCommon &&other) noexcept : storage_(std::move(other.storage_))
  {
    impl_ = this->impl_from_storage();
    other.storage_.reset();
    other.impl_ = nullptr;
  }

  /**
   * Wrap an existing #VArrayImpl and don't take ownership of it. This should rarely be used in
   * practice.
   */
  VArrayCommon(const VArrayImpl<T> *impl) : impl_(impl)
  {
    storage_ = impl_;
  }

  /**
   * Wrap an existing #VArrayImpl that is contained in a #std::shared_ptr. This takes ownership.
   */
  VArrayCommon(std::shared_ptr<const VArrayImpl<T>> impl) : impl_(impl.get())
  {
    if (impl) {
      storage_ = std::move(impl);
    }
  }

  /**
   * Replace the contained #VArrayImpl.
   */
  template<typename ImplT, typename... Args> void emplace(Args &&...args)
  {
    /* Make sure we are actually constructing a #VArrayImpl. */
    static_assert(std::is_base_of_v<VArrayImpl<T>, ImplT>);
    if constexpr (std::is_copy_constructible_v<ImplT> && Storage::template is_inline_v<ImplT>) {
      /* Only inline the implementation when it is copyable and when it fits into the inline
       * buffer of the storage. */
      impl_ = &storage_.template emplace<ImplT>(std::forward<Args>(args)...);
    }
    else {
      /* If it can't be inlined, create a new #std::shared_ptr instead and store that in the
       * storage. */
      std::shared_ptr<const VArrayImpl<T>> ptr = std::make_shared<ImplT>(
          std::forward<Args>(args)...);
      impl_ = &*ptr;
      storage_ = std::move(ptr);
    }
  }

  /** Utility to implement a copy assignment operator in a subclass. */
  void copy_from(const VArrayCommon &other)
  {
    if (this == &other) {
      return;
    }
    storage_ = other.storage_;
    impl_ = this->impl_from_storage();
  }

  /** Utility to implement a move assignment operator in a subclass. */
  void move_from(VArrayCommon &&other) noexcept
  {
    if (this == &other) {
      return;
    }
    storage_ = std::move(other.storage_);
    impl_ = this->impl_from_storage();
    other.storage_.reset();
    other.impl_ = nullptr;
  }

  /** Get a pointer to the virtual array implementation that is currently stored in #storage_, or
   * null. */
  const VArrayImpl<T> *impl_from_storage() const
  {
    if (!storage_.has_value()) {
      return nullptr;
    }
    return storage_.extra_info().get_varray(storage_.get());
  }

 public:
  /** Return false when there is no virtual array implementation currently. */
  operator bool() const
  {
    return impl_ != nullptr;
  }

  /**
   * Get the element at a specific index.
   * \note This can't return a reference because the value may be computed on the fly. This also
   * implies that one can not use this method for assignments.
   */
  T operator[](const int64_t index) const
  {
    BLI_assert(*this);
    BLI_assert(index >= 0);
    BLI_assert(index < this->size());
    return impl_->get(index);
  }

  /**
   * Same as the #operator[] but is sometimes easier to use when one has a pointer to a virtual
   * array.
   */
  T get(const int64_t index) const
  {
    return (*this)[index];
  }

  /**
   * Return the size of the virtual array. It's allowed to call this method even when there is no
   * virtual array. In this case 0 is returned.
   */
  int64_t size() const
  {
    if (impl_ == nullptr) {
      return 0;
    }
    return impl_->size();
  }

  /** True when the size is zero or when there is no virtual array. */
  bool is_empty() const
  {
    return this->size() == 0;
  }

  IndexRange index_range() const
  {
    return IndexRange(this->size());
  }

  CommonVArrayInfo common_info() const
  {
    BLI_assert(*this);
    return impl_->common_info();
  }

  /** Return true when the virtual array is stored as a span internally. */
  bool is_span() const
  {
    BLI_assert(*this);
    const CommonVArrayInfo info = impl_->common_info();
    return info.type == CommonVArrayInfo::Type::Span;
  }

  /**
   * Returns the internally used span of the virtual array. This invokes undefined behavior if the
   * virtual array is not stored as a span internally.
   */
  Span<T> get_internal_span() const
  {
    BLI_assert(this->is_span());
    const CommonVArrayInfo info = impl_->common_info();
    return Span<T>(static_cast<const T *>(info.data), this->size());
  }

  /** Return true when the virtual array returns the same value for every index. */
  bool is_single() const
  {
    BLI_assert(*this);
    const CommonVArrayInfo info = impl_->common_info();
    return info.type == CommonVArrayInfo::Type::Single;
  }

  /**
   * Return the value that is returned for every index. This invokes undefined behavior if the
   * virtual array would not return the same value for every index.
   */
  T get_internal_single() const
  {
    BLI_assert(this->is_single());
    const CommonVArrayInfo info = impl_->common_info();
    return *static_cast<const T *>(info.data);
  }

  /**
   * Return true when the other virtual references the same underlying memory.
   */
  bool is_same(const VArrayCommon<T> &other) const
  {
    if (!*this || !other) {
      return false;
    }
    /* Check in both directions in case one does not know how to compare to the other
     * implementation. */
    if (impl_->is_same(*other.impl_)) {
      return true;
    }
    if (other.impl_->is_same(*impl_)) {
      return true;
    }
    return false;
  }

  /** Copy the entire virtual array into a span. */
  void materialize(MutableSpan<T> r_span) const
  {
    this->materialize(IndexMask(this->size()), r_span);
  }

  /** Copy some indices of the virtual array into a span. */
  void materialize(IndexMask mask, MutableSpan<T> r_span) const
  {
    BLI_assert(mask.min_array_size() <= this->size());
    impl_->materialize(mask, r_span);
  }

  void materialize_to_uninitialized(MutableSpan<T> r_span) const
  {
    this->materialize_to_uninitialized(IndexMask(this->size()), r_span);
  }

  void materialize_to_uninitialized(IndexMask mask, MutableSpan<T> r_span) const
  {
    BLI_assert(mask.min_array_size() <= this->size());
    impl_->materialize_to_uninitialized(mask, r_span);
  }

  /** Copy some elements of the virtual array into a span. */
  void materialize_compressed(IndexMask mask, MutableSpan<T> r_span) const
  {
    impl_->materialize_compressed(mask, r_span);
  }

  void materialize_compressed_to_uninitialized(IndexMask mask, MutableSpan<T> r_span) const
  {
    impl_->materialize_compressed_to_uninitialized(mask, r_span);
  }

  /** See #GVArrayImpl::try_assign_GVArray. */
  bool try_assign_GVArray(GVArray &varray) const
  {
    return impl_->try_assign_GVArray(varray);
  }
};

template<typename T> class VMutableArray;

/**
 * Various tags to disambiguate constructors of virtual arrays.
 * Generally it is easier to use `VArray::For*` functions to construct virtual arrays, but
 * sometimes being able to use the constructor can result in better performance For example, when
 * constructing the virtual array directly in a vector. Without the constructor one would have to
 * construct the virtual array first and then move it into the vector.
 */
namespace varray_tag {
struct span {
};
struct single_ref {
};
struct single {
};
}  // namespace varray_tag

/**
 * A #VArray wraps a virtual array implementation and provides easy access to its elements. It can
 * be copied and moved. While it is relatively small, it should still be passed by reference if
 * possible (other than e.g. #Span).
 */
template<typename T> class VArray : public VArrayCommon<T> {
  friend VMutableArray<T>;

 public:
  VArray() = default;
  VArray(const VArray &other) = default;
  VArray(VArray &&other) noexcept = default;

  VArray(const VArrayImpl<T> *impl) : VArrayCommon<T>(impl)
  {
  }

  VArray(std::shared_ptr<const VArrayImpl<T>> impl) : VArrayCommon<T>(std::move(impl))
  {
  }

  VArray(varray_tag::span /* tag */, Span<T> span)
  {
    this->template emplace<VArrayImpl_For_Span_final<T>>(span);
  }

  VArray(varray_tag::single /* tag */, T value, const int64_t size)
  {
    this->template emplace<VArrayImpl_For_Single<T>>(std::move(value), size);
  }

  /**
   * Construct a new virtual array for a custom #VArrayImpl.
   */
  template<typename ImplT, typename... Args> static VArray For(Args &&...args)
  {
    static_assert(std::is_base_of_v<VArrayImpl<T>, ImplT>);
    VArray varray;
    varray.template emplace<ImplT>(std::forward<Args>(args)...);
    return varray;
  }

  /**
   * Construct a new virtual array that has the same value at every index.
   */
  static VArray ForSingle(T value, const int64_t size)
  {
    return VArray(varray_tag::single{}, std::move(value), size);
  }

  /**
   * Construct a new virtual array for an existing span. This does not take ownership of the
   * underlying memory.
   */
  static VArray ForSpan(Span<T> values)
  {
    return VArray(varray_tag::span{}, values);
  }

  /**
   * Construct a new virtual that will invoke the provided function whenever an element is
   * accessed.
   */
  template<typename GetFunc> static VArray ForFunc(const int64_t size, GetFunc get_func)
  {
    return VArray::For<VArrayImpl_For_Func<T, decltype(get_func)>>(size, std::move(get_func));
  }

  /**
   * Construct a new virtual array for an existing span with a mapping function. This does not take
   * ownership of the span.
   */
  template<typename StructT, T (*GetFunc)(const StructT &)>
  static VArray ForDerivedSpan(Span<StructT> values)
  {
    /* Cast const away, because the virtual array implementation for const and non const derived
     * spans is shared. */
    MutableSpan<StructT> span{const_cast<StructT *>(values.data()), values.size()};
    return VArray::For<VArrayImpl_For_DerivedSpan<StructT, T, GetFunc>>(span);
  }

  /**
   * Construct a new virtual array for an existing container. Every container that lays out the
   * elements in a plain array works. This takes ownership of the passed in container. If that is
   * not desired, use #ForSpan instead.
   */
  template<typename ContainerT> static VArray ForContainer(ContainerT container)
  {
    return VArray::For<VArrayImpl_For_ArrayContainer<ContainerT>>(std::move(container));
  }

  VArray &operator=(const VArray &other)
  {
    this->copy_from(other);
    return *this;
  }

  VArray &operator=(VArray &&other) noexcept
  {
    this->move_from(std::move(other));
    return *this;
  }
};

/**
 * Similar to #VArray but references a virtual array that can be modified.
 */
template<typename T> class VMutableArray : public VArrayCommon<T> {
 public:
  VMutableArray() = default;
  VMutableArray(const VMutableArray &other) = default;
  VMutableArray(VMutableArray &&other) noexcept = default;

  VMutableArray(const VMutableArrayImpl<T> *impl) : VArrayCommon<T>(impl)
  {
  }

  VMutableArray(std::shared_ptr<const VMutableArrayImpl<T>> impl)
      : VArrayCommon<T>(std::move(impl))
  {
  }

  /**
   * Construct a new virtual array for a custom #VMutableArrayImpl.
   */
  template<typename ImplT, typename... Args> static VMutableArray For(Args &&...args)
  {
    static_assert(std::is_base_of_v<VMutableArrayImpl<T>, ImplT>);
    VMutableArray varray;
    varray.template emplace<ImplT>(std::forward<Args>(args)...);
    return varray;
  }

  /**
   * Construct a new virtual array for an existing span. This does not take ownership of the span.
   */
  static VMutableArray ForSpan(MutableSpan<T> values)
  {
    return VMutableArray::For<VArrayImpl_For_Span_final<T>>(values);
  }

  /**
   * Construct a new virtual array for an existing span with a mapping function. This does not take
   * ownership of the span.
   */
  template<typename StructT, T (*GetFunc)(const StructT &), void (*SetFunc)(StructT &, T)>
  static VMutableArray ForDerivedSpan(MutableSpan<StructT> values)
  {
    return VMutableArray::For<VArrayImpl_For_DerivedSpan<StructT, T, GetFunc, SetFunc>>(values);
  }

  /** Convert to a #VArray by copying. */
  operator VArray<T>() const &
  {
    VArray<T> varray;
    varray.copy_from(*this);
    return varray;
  }

  /** Convert to a #VArray by moving. */
  operator VArray<T>() &&noexcept
  {
    VArray<T> varray;
    varray.move_from(std::move(*this));
    return varray;
  }

  VMutableArray &operator=(const VMutableArray &other)
  {
    this->copy_from(other);
    return *this;
  }

  VMutableArray &operator=(VMutableArray &&other) noexcept
  {
    this->move_from(std::move(other));
    return *this;
  }

  /**
   * Get access to the internal span. This invokes undefined behavior if the #is_span returned
   * false.
   */
  MutableSpan<T> get_internal_span() const
  {
    BLI_assert(this->is_span());
    const CommonVArrayInfo info = this->get_impl()->common_info();
    return MutableSpan<T>(const_cast<T *>(static_cast<const T *>(info.data)), this->size());
  }

  /**
   * Set the value at the given index.
   */
  void set(const int64_t index, T value)
  {
    BLI_assert(index >= 0);
    BLI_assert(index < this->size());
    this->get_impl()->set(index, std::move(value));
  }

  /**
   * Copy the values from the source span to all elements in the virtual array.
   */
  void set_all(Span<T> src)
  {
    BLI_assert(src.size() == this->size());
    this->get_impl()->set_all(src);
  }

  /** See #GVMutableArrayImpl::try_assign_GVMutableArray. */
  bool try_assign_GVMutableArray(GVMutableArray &varray) const
  {
    return this->get_impl()->try_assign_GVMutableArray(varray);
  }

 private:
  /** Utility to get the pointer to the wrapped #VMutableArrayImpl. */
  VMutableArrayImpl<T> *get_impl() const
  {
    /* This cast is valid by the invariant that a #VMutableArray->impl_ is always a
     * #VMutableArrayImpl. */
    return (VMutableArrayImpl<T> *)this->impl_;
  }
};

template<typename T> static constexpr bool is_VArray_v = false;
template<typename T> static constexpr bool is_VArray_v<VArray<T>> = true;

template<typename T> static constexpr bool is_VMutableArray_v = false;
template<typename T> static constexpr bool is_VMutableArray_v<VMutableArray<T>> = true;

/**
 * In many cases a virtual array is a span internally. In those cases, access to individual could
 * be much more efficient than calling a virtual method. When the underlying virtual array is not a
 * span, this class allocates a new array and copies the values over.
 *
 * This should be used in those cases:
 *  - All elements in the virtual array are accessed multiple times.
 *  - In most cases, the underlying virtual array is a span, so no copy is necessary to benefit
 *    from faster access.
 *  - An API is called, that does not accept virtual arrays, but only spans.
 */
template<typename T> class VArraySpan final : public Span<T> {
 private:
  VArray<T> varray_;
  Array<T> owned_data_;

 public:
  VArraySpan() = default;

  VArraySpan(VArray<T> varray) : Span<T>(), varray_(std::move(varray))
  {
    if (!varray_) {
      return;
    }
    this->size_ = varray_.size();
    const CommonVArrayInfo info = varray_.common_info();
    if (info.type == CommonVArrayInfo::Type::Span) {
      this->data_ = static_cast<const T *>(info.data);
    }
    else {
      owned_data_.~Array();
      new (&owned_data_) Array<T>(varray_.size(), NoInitialization{});
      varray_.materialize_to_uninitialized(owned_data_);
      this->data_ = owned_data_.data();
    }
  }

  VArraySpan(VArraySpan &&other)
      : varray_(std::move(other.varray_)), owned_data_(std::move(other.owned_data_))
  {
    if (!varray_) {
      return;
    }
    this->size_ = varray_.size();
    const CommonVArrayInfo info = varray_.common_info();
    if (info.type == CommonVArrayInfo::Type::Span) {
      this->data_ = static_cast<const T *>(info.data);
    }
    else {
      this->data_ = owned_data_.data();
    }
    other.data_ = nullptr;
    other.size_ = 0;
  }

  VArraySpan &operator=(VArraySpan &&other)
  {
    if (this == &other) {
      return *this;
    }
    std::destroy_at(this);
    new (this) VArraySpan(std::move(other));
    return *this;
  }
};

/**
 * Same as #VArraySpan, but for a mutable span.
 * The important thing to note is that when changing this span, the results might not be
 * immediately reflected in the underlying virtual array (only when the virtual array is a span
 * internally). The #save method can be used to write all changes to the underlying virtual array,
 * if necessary.
 */
template<typename T> class MutableVArraySpan final : public MutableSpan<T> {
 private:
  VMutableArray<T> varray_;
  Array<T> owned_data_;
  bool save_has_been_called_ = false;
  bool show_not_saved_warning_ = true;

 public:
  MutableVArraySpan() = default;

  /* Create a span for any virtual array. This is cheap when the virtual array is a span itself. If
   * not, a new array has to be allocated as a wrapper for the underlying virtual array. */
  MutableVArraySpan(VMutableArray<T> varray, const bool copy_values_to_span = true)
      : MutableSpan<T>(), varray_(std::move(varray))
  {
    if (!varray_) {
      return;
    }

    this->size_ = varray_.size();
    const CommonVArrayInfo info = varray_.common_info();
    if (info.type == CommonVArrayInfo::Type::Span) {
      this->data_ = const_cast<T *>(static_cast<const T *>(info.data));
    }
    else {
      if (copy_values_to_span) {
        owned_data_.~Array();
        new (&owned_data_) Array<T>(varray_.size(), NoInitialization{});
        varray_.materialize_to_uninitialized(owned_data_);
      }
      else {
        owned_data_.reinitialize(varray_.size());
      }
      this->data_ = owned_data_.data();
    }
  }

  MutableVArraySpan(MutableVArraySpan &&other)
      : varray_(std::move(other.varray_)),
        owned_data_(std::move(other.owned_data_)),
        show_not_saved_warning_(other.show_not_saved_warning_)
  {
    if (!varray_) {
      return;
    }

    this->size_ = varray_.size();
    const CommonVArrayInfo info = varray_.common_info();
    if (info.type == CommonVArrayInfo::Type::Span) {
      this->data_ = static_cast<T *>(const_cast<void *>(info.data));
    }
    else {
      this->data_ = owned_data_.data();
    }
    other.data_ = nullptr;
    other.size_ = 0;
  }

  ~MutableVArraySpan()
  {
    if (varray_) {
      if (show_not_saved_warning_) {
        if (!save_has_been_called_) {
          std::cout << "Warning: Call `save()` to make sure that changes persist in all cases.\n";
        }
      }
    }
  }

  MutableVArraySpan &operator=(MutableVArraySpan &&other)
  {
    if (this == &other) {
      return *this;
    }
    std::destroy_at(this);
    new (this) MutableVArraySpan(std::move(other));
    return *this;
  }

  const VMutableArray<T> &varray() const
  {
    return varray_;
  }

  /* Write back all values from a temporary allocated array to the underlying virtual array. */
  void save()
  {
    save_has_been_called_ = true;
    if (this->data_ != owned_data_.data()) {
      return;
    }
    varray_.set_all(owned_data_);
  }

  void disable_not_applied_warning()
  {
    show_not_saved_warning_ = false;
  }
};

template<typename T> class SingleAsSpan {
 private:
  T value_;
  int64_t size_;

 public:
  SingleAsSpan(T value, int64_t size) : value_(std::move(value)), size_(size)
  {
    BLI_assert(size_ >= 0);
  }

  SingleAsSpan(const VArray<T> &varray) : SingleAsSpan(varray.get_internal_single(), varray.size())
  {
  }

  const T &operator[](const int64_t index) const
  {
    BLI_assert(index >= 0);
    BLI_assert(index < size_);
    UNUSED_VARS_NDEBUG(index);
    return value_;
  }
};

}  // namespace blender
