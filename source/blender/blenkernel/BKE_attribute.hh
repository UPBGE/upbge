/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_color.hh"
#include "BLI_function_ref.hh"
#include "BLI_generic_span.hh"
#include "BLI_generic_virtual_array.hh"
#include "BLI_math_vec_types.hh"
#include "BLI_set.hh"

#include "BKE_anonymous_attribute.hh"
#include "BKE_attribute.h"

struct Mesh;
struct PointCloud;

namespace blender::bke {

/**
 * Identifies an attribute that is either named or anonymous.
 * It does not own the identifier, so it is just a reference.
 */
class AttributeIDRef {
 private:
  StringRef name_;
  const AnonymousAttributeID *anonymous_id_ = nullptr;

 public:
  AttributeIDRef();
  AttributeIDRef(StringRef name);
  AttributeIDRef(StringRefNull name);
  AttributeIDRef(const char *name);
  AttributeIDRef(const std::string &name);
  AttributeIDRef(const AnonymousAttributeID *anonymous_id);

  operator bool() const;
  uint64_t hash() const;
  bool is_named() const;
  bool is_anonymous() const;
  StringRef name() const;
  const AnonymousAttributeID &anonymous_id() const;
  bool should_be_kept() const;

  friend bool operator==(const AttributeIDRef &a, const AttributeIDRef &b);
  friend std::ostream &operator<<(std::ostream &stream, const AttributeIDRef &attribute_id);
};

/**
 * Contains information about an attribute in a geometry component.
 * More information can be added in the future. E.g. whether the attribute is builtin and how it is
 * stored (uv map, vertex group, ...).
 */
struct AttributeMetaData {
  eAttrDomain domain;
  eCustomDataType data_type;

  constexpr friend bool operator==(AttributeMetaData a, AttributeMetaData b)
  {
    return (a.domain == b.domain) && (a.data_type == b.data_type);
  }
};

struct AttributeKind {
  eAttrDomain domain;
  eCustomDataType data_type;
};

/**
 * Base class for the attribute initializer types described below.
 */
struct AttributeInit {
  enum class Type {
    Default,
    VArray,
    MoveArray,
  };
  Type type;
  AttributeInit(const Type type) : type(type)
  {
  }
};

/**
 * Create an attribute using the default value for the data type.
 * The default values may depend on the attribute provider implementation.
 */
struct AttributeInitDefault : public AttributeInit {
  AttributeInitDefault() : AttributeInit(Type::Default)
  {
  }
};

/**
 * Create an attribute by copying data from an existing virtual array. The virtual array
 * must have the same type as the newly created attribute.
 *
 * Note that this can be used to fill the new attribute with the default
 */
struct AttributeInitVArray : public AttributeInit {
  blender::GVArray varray;

  AttributeInitVArray(blender::GVArray varray)
      : AttributeInit(Type::VArray), varray(std::move(varray))
  {
  }
};

/**
 * Create an attribute with a by passing ownership of a pre-allocated contiguous array of data.
 * Sometimes data is created before a geometry component is available. In that case, it's
 * preferable to move data directly to the created attribute to avoid a new allocation and a copy.
 *
 * Note that this will only have a benefit for attributes that are stored directly as contiguous
 * arrays, so not for some built-in attributes.
 *
 * The array must be allocated with MEM_*, since `attribute_try_create` will free the array if it
 * can't be used directly, and that is generally how Blender expects custom data to be allocated.
 */
struct AttributeInitMove : public AttributeInit {
  void *data = nullptr;

  AttributeInitMove(void *data) : AttributeInit(Type::MoveArray), data(data)
  {
  }
};

/* Returns false when the iteration should be stopped. */
using AttributeForeachCallback =
    FunctionRef<bool(const AttributeIDRef &attribute_id, const AttributeMetaData &meta_data)>;

/**
 * Result when looking up an attribute from some geometry with the intention of only reading from
 * it.
 */
template<typename T> struct AttributeReader {
  /**
   * Virtual array that provides access to the attribute data. This may be empty.
   */
  VArray<T> varray;
  /**
   * Domain where the attribute is stored. This also determines the size of the virtual array.
   */
  eAttrDomain domain;

  operator bool() const
  {
    return this->varray;
  }
};

/**
 * Result when looking up an attribute from some geometry with read an write access. After writing
 * to the attribute, the #finish method has to be called. This may invalidate caches based on this
 * attribute.
 */
template<typename T> struct AttributeWriter {
  /**
   * Virtual array giving read and write access to the attribute. This may be empty.
   * Consider using #SpanAttributeWriter when you want to access the virtual array as a span.
   */
  VMutableArray<T> varray;
  /**
   * Domain where the attribute is stored on the geometry. Also determines the size of the virtual
   * array.
   */
  eAttrDomain domain;
  /**
   * A function that has to be called after the attribute has been edited. This may be empty.
   */
  std::function<void()> tag_modified_fn;

  operator bool() const
  {
    return this->varray;
  }

  /**
   * Has to be called after the attribute has been modified.
   */
  void finish()
  {
    if (this->tag_modified_fn) {
      this->tag_modified_fn();
    }
  }
};

/**
 * A version of #AttributeWriter for the common case when the user of the attribute wants to write
 * to a span instead of a virtual array. Since most attributes are spans internally, this can
 * result in better performance and also simplifies code.
 */
template<typename T> struct SpanAttributeWriter {
  /**
   * A span based on the virtual array that contains the attribute data. This may be empty.
   */
  MutableVArraySpan<T> span;
  /**
   * Domain of the attribute. Also determines the size of the span.
   */
  eAttrDomain domain;
  /**
   * Has to be called after writing to the span.
   */
  std::function<void()> tag_modified_fn;

  SpanAttributeWriter() = default;

  SpanAttributeWriter(AttributeWriter<T> &&other, const bool copy_values_to_span)
      : span(std::move(other.varray), copy_values_to_span),
        domain(other.domain),
        tag_modified_fn(std::move(other.tag_modified_fn))
  {
  }

  operator bool() const
  {
    return span.varray();
  }

  /**
   * Has to be called when done writing to the attribute. This makes sure that the data is copied
   * to the underlying attribute if it was not stored as an array. Furthermore, this may invalidate
   * other data depending on the modified attribute.
   */
  void finish()
  {
    this->span.save();
    if (this->tag_modified_fn) {
      this->tag_modified_fn();
    }
  }
};

/**
 * A generic version of #AttributeReader.
 */
struct GAttributeReader {
  GVArray varray;
  eAttrDomain domain;

  operator bool() const
  {
    return this->varray;
  }

  template<typename T> AttributeReader<T> typed() const
  {
    return {varray.typed<T>(), domain};
  }
};

/**
 * A generic version of #AttributeWriter.
 */
struct GAttributeWriter {
  GVMutableArray varray;
  eAttrDomain domain;
  std::function<void()> tag_modified_fn;

  operator bool() const
  {
    return this->varray;
  }

  void finish()
  {
    if (this->tag_modified_fn) {
      this->tag_modified_fn();
    }
  }

  template<typename T> AttributeWriter<T> typed() const
  {
    return {varray.typed<T>(), domain, tag_modified_fn};
  }
};

/**
 * A generic version of #SpanAttributeWriter.
 */
struct GSpanAttributeWriter {
  GMutableVArraySpan span;
  eAttrDomain domain;
  std::function<void()> tag_modified_fn;

  GSpanAttributeWriter() = default;

  GSpanAttributeWriter(GAttributeWriter &&other, const bool copy_values_to_span)
      : span(std::move(other.varray), copy_values_to_span),
        domain(other.domain),
        tag_modified_fn(std::move(other.tag_modified_fn))
  {
  }

  operator bool() const
  {
    return span.varray();
  }

  void finish()
  {
    this->span.save();
    if (this->tag_modified_fn) {
      this->tag_modified_fn();
    }
  }
};

/**
 * Core functions which make up the attribute API. They should not be called directly, but through
 * #AttributesAccessor or #MutableAttributesAccessor.
 *
 * This is similar to a virtual function table. A struct of function pointers is used instead,
 * because this way the attribute accessors can be trivial and can be passed around by value. This
 * makes it easy to return the attribute accessor for a geometry from a function.
 */
struct AttributeAccessorFunctions {
  bool (*contains)(const void *owner, const AttributeIDRef &attribute_id);
  std::optional<AttributeMetaData> (*lookup_meta_data)(const void *owner,
                                                       const AttributeIDRef &attribute_id);
  bool (*domain_supported)(const void *owner, eAttrDomain domain);
  int (*domain_size)(const void *owner, eAttrDomain domain);
  bool (*is_builtin)(const void *owner, const AttributeIDRef &attribute_id);
  GAttributeReader (*lookup)(const void *owner, const AttributeIDRef &attribute_id);
  GVArray (*adapt_domain)(const void *owner,
                          const GVArray &varray,
                          eAttrDomain from_domain,
                          eAttrDomain to_domain);
  bool (*for_all)(const void *owner,
                  FunctionRef<bool(const AttributeIDRef &, const AttributeMetaData &)> fn);

  GAttributeWriter (*lookup_for_write)(void *owner, const AttributeIDRef &attribute_id);
  bool (*remove)(void *owner, const AttributeIDRef &attribute_id);
  bool (*add)(void *owner,
              const AttributeIDRef &attribute_id,
              eAttrDomain domain,
              eCustomDataType data_type,
              const AttributeInit &initializer);
};

/**
 * Provides read-only access to the set of attributes on some geometry.
 *
 * Note, this does not own the attributes. When the owner is freed, it is invalid to access its
 * attributes.
 */
class AttributeAccessor {
 protected:
  /**
   * The data that actually owns the attributes, for example, a pointer to a #Mesh or #PointCloud
   * Most commonly this is a pointer to a #Mesh or #PointCloud.
   * Under some circumstances this can be null. In that case most methods can't be used. Allowed
   * methods are #domain_size, #for_all and #is_builtin. We could potentially make these methods
   * accessible without #AttributeAccessor and then #owner_ could always be non-null.
   *
   * \note This class cannot modify the owner's attributes, but the pointer is still non-const, so
   * this class can be a base class for the mutable version.
   */
  void *owner_;
  /**
   * Functions that know how to access the attributes stored in the owner above.
   */
  const AttributeAccessorFunctions *fn_;

 public:
  AttributeAccessor(const void *owner, const AttributeAccessorFunctions &fn)
      : owner_(const_cast<void *>(owner)), fn_(&fn)
  {
  }

  /**
   * \return True, when the attribute is available.
   */
  bool contains(const AttributeIDRef &attribute_id) const
  {
    return fn_->contains(owner_, attribute_id);
  }

  /**
   * \return Information about the attribute if it exists.
   */
  std::optional<AttributeMetaData> lookup_meta_data(const AttributeIDRef &attribute_id) const
  {
    return fn_->lookup_meta_data(owner_, attribute_id);
  }

  /**
   * \return True, when attributes can exist on that domain.
   */
  bool domain_supported(const eAttrDomain domain) const
  {
    return fn_->domain_supported(owner_, domain);
  }

  /**
   * \return Number of elements in the given domain.
   */
  int domain_size(const eAttrDomain domain) const
  {
    return fn_->domain_size(owner_, domain);
  }

  /**
   * \return True, when the attribute has a special meaning for Blender and can't be used for
   * arbitrary things.
   */
  bool is_builtin(const AttributeIDRef &attribute_id) const
  {
    return fn_->is_builtin(owner_, attribute_id);
  }

  /**
   * Get read-only access to the attribute. If the attribute does not exist, the return value is
   * empty.
   */
  GAttributeReader lookup(const AttributeIDRef &attribute_id) const
  {
    return fn_->lookup(owner_, attribute_id);
  }

  /**
   * Get read-only access to the attribute. If necessary, the attribute is interpolated to the
   * given domain, and converted to the given type, in that order.  The result may be empty.
   */
  GVArray lookup(const AttributeIDRef &attribute_id,
                 const std::optional<eAttrDomain> domain,
                 const std::optional<eCustomDataType> data_type) const;

  /**
   * Get read-only access to the attribute whereby the attribute is interpolated to the given
   * domain. The result may be empty.
   */
  GVArray lookup(const AttributeIDRef &attribute_id, const eAttrDomain domain) const
  {
    return this->lookup(attribute_id, domain, std::nullopt);
  }

  /**
   * Get read-only access to the attribute whereby the attribute is converted to the given type.
   * The result may be empty.
   */
  GVArray lookup(const AttributeIDRef &attribute_id, const eCustomDataType data_type) const
  {
    return this->lookup(attribute_id, std::nullopt, data_type);
  }

  /**
   * Get read-only access to the attribute. If necessary, the attribute is interpolated to the
   * given domain and then converted to the given type, in that order. The result may be empty.
   */
  template<typename T>
  VArray<T> lookup(const AttributeIDRef &attribute_id,
                   const std::optional<eAttrDomain> domain = std::nullopt) const
  {
    const CPPType &cpp_type = CPPType::get<T>();
    const eCustomDataType data_type = cpp_type_to_custom_data_type(cpp_type);
    return this->lookup(attribute_id, domain, data_type).typed<T>();
  }

  /**
   * Get read-only access to the attribute. If necessary, the attribute is interpolated to the
   * given domain and then converted to the given data type, in that order.
   * If the attribute does not exist, a virtual array with the given default value is returned.
   * If the passed in default value is null, the default value of the type is used (generally 0).
   */
  GVArray lookup_or_default(const AttributeIDRef &attribute_id,
                            const eAttrDomain domain,
                            const eCustomDataType data_type,
                            const void *default_value = nullptr) const;

  /**
   * Same as the generic version above, but should be used when the type is known at compile time.
   */
  template<typename T>
  VArray<T> lookup_or_default(const AttributeIDRef &attribute_id,
                              const eAttrDomain domain,
                              const T &default_value) const
  {
    if (VArray<T> varray = this->lookup<T>(attribute_id, domain)) {
      return varray;
    }
    return VArray<T>::ForSingle(default_value, this->domain_size(domain));
  }

  /**
   * Interpolate data from one domain to another.
   */
  GVArray adapt_domain(const GVArray &varray,
                       const eAttrDomain from_domain,
                       const eAttrDomain to_domain) const
  {
    return fn_->adapt_domain(owner_, varray, from_domain, to_domain);
  }

  /**
   * Interpolate data from one domain to another.
   */
  template<typename T>
  VArray<T> adapt_domain(const VArray<T> &varray,
                         const eAttrDomain from_domain,
                         const eAttrDomain to_domain) const
  {
    return this->adapt_domain(GVArray(varray), from_domain, to_domain).typed<T>();
  }

  /**
   * Run the provided function for every attribute.
   */
  bool for_all(const AttributeForeachCallback fn) const
  {
    if (owner_ != nullptr) {
      return fn_->for_all(owner_, fn);
    }
    return true;
  }

  /**
   * Get a set of all attributes.
   */
  Set<AttributeIDRef> all_ids() const;
};

/**
 * Extends #AttributeAccessor with methods that allow modifying individual attributes as well as
 * the set of attributes.
 */
class MutableAttributeAccessor : public AttributeAccessor {
 public:
  MutableAttributeAccessor(void *owner, const AttributeAccessorFunctions &fn)
      : AttributeAccessor(owner, fn)
  {
  }

  /**
   * Get a writable attribute or none if it does not exist.
   * Make sure to call #finish after changes are done.
   */
  GAttributeWriter lookup_for_write(const AttributeIDRef &attribute_id);

  /**
   * Get a writable attribute or non if it does not exist.
   * Make sure to call #finish after changes are done.
   */
  template<typename T> AttributeWriter<T> lookup_for_write(const AttributeIDRef &attribute_id)
  {
    GAttributeWriter attribute = this->lookup_for_write(attribute_id);
    if (!attribute) {
      return {};
    }
    if (!attribute.varray.type().is<T>()) {
      return {};
    }
    return attribute.typed<T>();
  }

  /**
   * Create a new attribute.
   * \return True, when a new attribute has been created. False, when it's not possible to create
   * this attribute or there is already an attribute with that id.
   */
  bool add(const AttributeIDRef &attribute_id,
           const eAttrDomain domain,
           const eCustomDataType data_type,
           const AttributeInit &initializer)
  {
    return fn_->add(owner_, attribute_id, domain, data_type, initializer);
  }

  /**
   * Find an attribute with the given id, domain and data type. If it does not exist, create a new
   * attribute. If the attribute does not exist and can't be created (e.g. because it already
   * exists on a different domain or with a different type), none is returned.
   */
  GAttributeWriter lookup_or_add_for_write(
      const AttributeIDRef &attribute_id,
      const eAttrDomain domain,
      const eCustomDataType data_type,
      const AttributeInit &initializer = AttributeInitDefault());

  /**
   * Same as above, but returns a type that makes it easier to work with the attribute as a span.
   * If the caller newly initializes the attribute, it's better to use
   * #lookup_or_add_for_write_only_span.
   */
  GSpanAttributeWriter lookup_or_add_for_write_span(
      const AttributeIDRef &attribute_id,
      const eAttrDomain domain,
      const eCustomDataType data_type,
      const AttributeInit &initializer = AttributeInitDefault());

  /**
   * Same as above, but should be used when the type is known at compile time.
   */
  template<typename T>
  AttributeWriter<T> lookup_or_add_for_write(
      const AttributeIDRef &attribute_id,
      const eAttrDomain domain,
      const AttributeInit &initializer = AttributeInitDefault())
  {
    const CPPType &cpp_type = CPPType::get<T>();
    const eCustomDataType data_type = cpp_type_to_custom_data_type(cpp_type);
    return this->lookup_or_add_for_write(attribute_id, domain, data_type, initializer).typed<T>();
  }

  /**
   * Same as above, but should be used when the type is known at compile time.
   */
  template<typename T>
  SpanAttributeWriter<T> lookup_or_add_for_write_span(
      const AttributeIDRef &attribute_id,
      const eAttrDomain domain,
      const AttributeInit &initializer = AttributeInitDefault())
  {
    AttributeWriter<T> attribute = this->lookup_or_add_for_write<T>(
        attribute_id, domain, initializer);
    if (attribute) {
      return SpanAttributeWriter<T>{std::move(attribute), true};
    }
    return {};
  }

  /**
   * Find an attribute with the given id, domain and data type. If it does not exist, create a new
   * attribute. If the attribute does not exist and can't be created, none is returned.
   *
   * The "only" in the name indicates that the caller should not read existing values from the
   * span. If the attribute is not stored as span internally, the existing values won't be copied
   * over to the span.
   */
  GSpanAttributeWriter lookup_or_add_for_write_only_span(const AttributeIDRef &attribute_id,
                                                         const eAttrDomain domain,
                                                         const eCustomDataType data_type);

  /**
   * Same as above, but should be used when the type is known at compile time.
   */
  template<typename T>
  SpanAttributeWriter<T> lookup_or_add_for_write_only_span(const AttributeIDRef &attribute_id,
                                                           const eAttrDomain domain)
  {
    AttributeWriter<T> attribute = this->lookup_or_add_for_write<T>(attribute_id, domain);
    if (attribute) {
      return SpanAttributeWriter<T>{std::move(attribute), false};
    }
    return {};
  }

  /**
   * Remove an attribute.
   * \return True, when the attribute has been deleted. False, when it's not possible to delete
   * this attribute or if there is no attribute with that id.
   */
  bool remove(const AttributeIDRef &attribute_id)
  {
    return fn_->remove(owner_, attribute_id);
  }

  /**
   * Remove all anonymous attributes.
   */
  void remove_anonymous();
};

struct AttributeTransferData {
  /* Expect that if an attribute exists, it is stored as a contiguous array internally anyway. */
  GVArraySpan src;
  AttributeMetaData meta_data;
  bke::GSpanAttributeWriter dst;
};
/**
 * Retrieve attribute arrays and writers for attributes that should be transferred between
 * data-blocks of the same type.
 */
Vector<AttributeTransferData> retrieve_attributes_for_transfer(
    const bke::AttributeAccessor src_attributes,
    bke::MutableAttributeAccessor dst_attributes,
    eAttrDomainMask domain_mask,
    const Set<std::string> &skip = {});

bool allow_procedural_attribute_access(StringRef attribute_name);
extern const char *no_procedural_access_message;

eCustomDataType attribute_data_type_highest_complexity(Span<eCustomDataType> data_types);
/**
 * Domains with a higher "information density" have a higher priority,
 * in order to choose a domain that will not lose data through domain conversion.
 */
eAttrDomain attribute_domain_highest_priority(Span<eAttrDomain> domains);

/**
 * A basic container around DNA CustomData so that its users
 * don't have to implement special copy and move constructors.
 */
class CustomDataAttributes {
  /**
   * #CustomData needs a size to be freed, and unfortunately it isn't stored in the struct
   * itself, so keep track of the size here so this class can implement its own destructor.
   * If the implementation of the attribute storage changes, this could be removed.
   */
  int size_;

 public:
  CustomData data;

  CustomDataAttributes();
  ~CustomDataAttributes();
  CustomDataAttributes(const CustomDataAttributes &other);
  CustomDataAttributes(CustomDataAttributes &&other);
  CustomDataAttributes &operator=(const CustomDataAttributes &other);

  void reallocate(int size);

  void clear();

  std::optional<blender::GSpan> get_for_read(const AttributeIDRef &attribute_id) const;

  /**
   * Return a virtual array for a stored attribute, or a single value virtual array with the
   * default value if the attribute doesn't exist. If no default value is provided, the default
   * value for the type will be used.
   */
  blender::GVArray get_for_read(const AttributeIDRef &attribute_id,
                                eCustomDataType data_type,
                                const void *default_value) const;

  template<typename T>
  blender::VArray<T> get_for_read(const AttributeIDRef &attribute_id, const T &default_value) const
  {
    const blender::CPPType &cpp_type = blender::CPPType::get<T>();
    const eCustomDataType type = blender::bke::cpp_type_to_custom_data_type(cpp_type);
    GVArray varray = this->get_for_read(attribute_id, type, &default_value);
    return varray.typed<T>();
  }

  std::optional<blender::GMutableSpan> get_for_write(const AttributeIDRef &attribute_id);
  bool create(const AttributeIDRef &attribute_id, eCustomDataType data_type);
  bool create_by_move(const AttributeIDRef &attribute_id, eCustomDataType data_type, void *buffer);
  bool remove(const AttributeIDRef &attribute_id);

  /**
   * Change the order of the attributes to match the order of IDs in the argument.
   */
  void reorder(Span<AttributeIDRef> new_order);

  bool foreach_attribute(const AttributeForeachCallback callback, eAttrDomain domain) const;
};

AttributeAccessor mesh_attributes(const Mesh &mesh);
MutableAttributeAccessor mesh_attributes_for_write(Mesh &mesh);

AttributeAccessor pointcloud_attributes(const PointCloud &pointcloud);
MutableAttributeAccessor pointcloud_attributes_for_write(PointCloud &pointcloud);

/* -------------------------------------------------------------------- */
/** \name #AttributeIDRef Inline Methods
 * \{ */

inline AttributeIDRef::AttributeIDRef() = default;

inline AttributeIDRef::AttributeIDRef(StringRef name) : name_(name)
{
}

inline AttributeIDRef::AttributeIDRef(StringRefNull name) : name_(name)
{
}

inline AttributeIDRef::AttributeIDRef(const char *name) : name_(name)
{
}

inline AttributeIDRef::AttributeIDRef(const std::string &name) : name_(name)
{
}

/* The anonymous id is only borrowed, the caller has to keep a reference to it. */
inline AttributeIDRef::AttributeIDRef(const AnonymousAttributeID *anonymous_id)
    : anonymous_id_(anonymous_id)
{
}

inline bool operator==(const AttributeIDRef &a, const AttributeIDRef &b)
{
  return a.anonymous_id_ == b.anonymous_id_ && a.name_ == b.name_;
}

inline AttributeIDRef::operator bool() const
{
  return this->is_named() || this->is_anonymous();
}

inline uint64_t AttributeIDRef::hash() const
{
  return get_default_hash_2(name_, anonymous_id_);
}

inline bool AttributeIDRef::is_named() const
{
  return !name_.is_empty();
}

inline bool AttributeIDRef::is_anonymous() const
{
  return anonymous_id_ != nullptr;
}

inline StringRef AttributeIDRef::name() const
{
  BLI_assert(this->is_named());
  return name_;
}

inline const AnonymousAttributeID &AttributeIDRef::anonymous_id() const
{
  BLI_assert(this->is_anonymous());
  return *anonymous_id_;
}

/**
 * \return True if the attribute should not be removed automatically as an optimization during
 * processing or copying. Anonymous attributes can be removed when they no longer have any
 * references.
 */
inline bool AttributeIDRef::should_be_kept() const
{
  return this->is_named() || BKE_anonymous_attribute_id_has_strong_references(anonymous_id_);
}

}  // namespace blender::bke
