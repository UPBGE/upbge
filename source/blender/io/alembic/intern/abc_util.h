/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/** \file
 * \ingroup balembic
 */

#include <Alembic/Abc/All.h>
#include <Alembic/AbcGeom/All.h>

using Alembic::Abc::chrono_t;

struct ID;
struct Object;

namespace blender::io::alembic {

class AbcObjectReader;
struct ImportSettings;

std::string get_id_name(const ID *const id);
std::string get_id_name(const Object *const ob);
std::string get_valid_abc_name(const char *name);
/**
 * \brief get_object_dag_path_name returns the name under which the object
 *  will be exported in the Alembic file. It is of the form
 *  "[../grandparent/]parent/object" if dupli_parent is NULL, or
 *  "dupli_parent/[../grandparent/]parent/object" otherwise.
 * \param ob:
 * \param dupli_parent:
 * \return
 */
std::string get_object_dag_path_name(const Object *const ob, Object *dupli_parent);

/* Convert from float to Alembic matrix representations. Does NOT convert from Z-up to Y-up. */
Imath::M44d convert_matrix_datatype(float mat[4][4]);
/* Convert from Alembic to float matrix representations. Does NOT convert from Y-up to Z-up. */
void convert_matrix_datatype(const Imath::M44d &xform, float r_mat[4][4]);

void split(const std::string &s, char delim, std::vector<std::string> &tokens);

template<class TContainer> bool begins_with(const TContainer &input, const TContainer &match)
{
  return input.size() >= match.size() && std::equal(match.begin(), match.end(), input.begin());
}

template<typename Schema>
void get_min_max_time_ex(const Schema &schema, chrono_t &min, chrono_t &max)
{
  const Alembic::Abc::TimeSamplingPtr &time_samp = schema.getTimeSampling();

  if (!schema.isConstant()) {
    const size_t num_samps = schema.getNumSamples();

    if (num_samps > 0) {
      const chrono_t min_time = time_samp->getSampleTime(0);
      min = std::min(min, min_time);

      const chrono_t max_time = time_samp->getSampleTime(num_samps - 1);
      max = std::max(max, max_time);
    }
  }
}

template<typename Schema>
void get_min_max_time(const Alembic::AbcGeom::IObject &object,
                      const Schema &schema,
                      chrono_t &min,
                      chrono_t &max)
{
  get_min_max_time_ex(schema, min, max);

  const Alembic::AbcGeom::IObject &parent = object.getParent();
  if (parent.valid() && Alembic::AbcGeom::IXform::matches(parent.getMetaData())) {
    Alembic::AbcGeom::IXform xform(parent, Alembic::AbcGeom::kWrapExisting);
    get_min_max_time_ex(xform.getSchema(), min, max);
  }
}

bool has_property(const Alembic::Abc::ICompoundProperty &prop, const std::string &name);

double get_weight_and_index(Alembic::AbcCoreAbstract::chrono_t time,
                            const Alembic::AbcCoreAbstract::TimeSamplingPtr &time_sampling,
                            int samples_number,
                            Alembic::AbcGeom::index_t &i0,
                            Alembic::AbcGeom::index_t &i1);

AbcObjectReader *create_reader(const Alembic::AbcGeom::IObject &object, ImportSettings &settings);

/* *************************** */

#undef ABC_DEBUG_TIME

class ScopeTimer {
  const char *m_message;
  double m_start;

 public:
  ScopeTimer(const char *message);
  ~ScopeTimer();
};

#ifdef ABC_DEBUG_TIME
#  define SCOPE_TIMER(message) ScopeTimer prof(message)
#else
#  define SCOPE_TIMER(message)
#endif

/* *************************** */

/**
 * Utility class whose purpose is to more easily log related information. An
 * instance of the SimpleLogger can be created in any context, and will hold a
 * copy of all the strings passed to its output stream.
 *
 * Different instances of the class may be accessed from different threads,
 * although accessing the same instance from different threads will lead to race
 * conditions.
 */
class SimpleLogger {
  std::ostringstream m_stream;

 public:
  /**
   * Return a copy of the string contained in the SimpleLogger's stream.
   */
  std::string str() const;

  /**
   * Remove the bits set on the SimpleLogger's stream and clear its string.
   */
  void clear();

  /**
   * Return a reference to the SimpleLogger's stream, in order to e.g. push
   * content into it.
   */
  std::ostringstream &stream();
};

#define ABC_LOG(logger) logger.stream()

/**
 * Pass the content of the logger's stream to the specified std::ostream.
 */
std::ostream &operator<<(std::ostream &os, const SimpleLogger &logger);

}  // namespace blender::io::alembic
