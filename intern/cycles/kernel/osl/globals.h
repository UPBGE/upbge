/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#ifndef __OSL_GLOBALS_H__
#define __OSL_GLOBALS_H__

#ifdef WITH_OSL

#  include <OSL/oslexec.h>

#  include <OpenImageIO/refcnt.h>
#  include <OpenImageIO/unordered_map_concurrent.h>

#  include "util/map.h"
#  include "util/param.h"
#  include "util/thread.h"
#  include "util/unique_ptr.h"
#  include "util/vector.h"

#  ifndef WIN32
using std::isfinite;
#  endif

CCL_NAMESPACE_BEGIN

class OSLRenderServices;
class ColorSpaceProcessor;

/* OSL Globals
 *
 * Data needed by OSL render services, that is global to a rendering session.
 * This includes all OSL shaders, name to attribute mapping and texture handles.
 */

struct OSLGlobals {
  OSLGlobals()
  {
    ss = NULL;
    ts = NULL;
    services = NULL;
    use = false;
  }

  bool use;

  /* shading system */
  OSL::ShadingSystem *ss;
  OSL::TextureSystem *ts;
  OSLRenderServices *services;

  /* shader states */
  vector<OSL::ShaderGroupRef> surface_state;
  vector<OSL::ShaderGroupRef> volume_state;
  vector<OSL::ShaderGroupRef> displacement_state;
  vector<OSL::ShaderGroupRef> bump_state;
  OSL::ShaderGroupRef background_state;

  /* attributes */
  struct Attribute {
    TypeDesc type;
    AttributeDescriptor desc;
    ParamValue value;
  };

  typedef unordered_map<ustring, Attribute, ustringHash> AttributeMap;
  typedef unordered_map<ustring, int, ustringHash> ObjectNameMap;

  vector<AttributeMap> attribute_map;
  ObjectNameMap object_name_map;
  vector<ustring> object_names;
};

/* trace() call result */
struct OSLTraceData {
  Ray ray;
  Intersection isect;
  ShaderData sd;
  bool setup;
  bool init;
  bool hit;
};

/* thread key for thread specific data lookup */
struct OSLThreadData {
  OSL::ShaderGlobals globals;
  OSL::PerThreadInfo *osl_thread_info;
  OSLTraceData tracedata;
  OSL::ShadingContext *context;
  OIIO::TextureSystem::Perthread *oiio_thread_info;
};

CCL_NAMESPACE_END

#endif

#endif /* __OSL_GLOBALS_H__ */
